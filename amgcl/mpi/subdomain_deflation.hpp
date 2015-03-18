#ifndef AMGCL_MPI_SUBDOMAIN_DEFLATION_HPP
#define AMGCL_MPI_SUBDOMAIN_DEFLATION_HPP

/*
The MIT License

Copyright (c) 2012-2015 Denis Demidov <dennis.demidov@gmail.com>
Copyright (c) 2014, Riccardo Rossi, CIMNE (International Center for Numerical Methods in Engineering)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   amgcl/mpi/subdomain_deflatedion.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  Distributed solver based on subdomain deflation.
 */

#include <vector>
#include <map>

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/range/numeric.hpp>
#include <boost/multi_array.hpp>

#include <mpi.h>

#include <amgcl/amgcl.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/mpi/util.hpp>
#include <amgcl/mpi/skyline_lu.hpp>

namespace amgcl {

/// Distributed algorithms and structures.
namespace mpi {

namespace detail {
struct mpi_inner_product {
    communicator comm;

    mpi_inner_product(MPI_Comm comm) : comm(comm) {}

    template <class Vec1, class Vec2>
    typename backend::value_type<Vec1>::type
    operator()(const Vec1 &x, const Vec2 &y) const {
        TIC("inner product");
        typedef typename backend::value_type<Vec1>::type value_type;

        value_type lsum = backend::inner_product(x, y);
        value_type gsum;

        MPI_Allreduce(&lsum, &gsum, 1, datatype<value_type>::get(), MPI_SUM, comm);

        TOC("inner product");
        return gsum;
    }
};

} // namespace detail

/// Pointwise constant deflation vectors.
struct constant_deflation {
    const int block_size;
    /// Constructor
    /**
     * \param block_size Number of degrees of freedom per grid point
     */
    constant_deflation(int block_size = 1) : block_size(block_size) {}

    int dim() const {
        return block_size;
    }

    int operator()(ptrdiff_t row, int j) const {
        return row % block_size == j;
    }
};

/// Distributed solver based on subdomain deflation.
/**
 * \sa \cite Frank2001
 */
template <
    class                         Backend,
    class                         Coarsening,
    template <class> class        Relax,
    template <class, class> class IterativeSolver,
    class                         DirectSolver = mpi::skyline_lu<typename Backend::value_type>
    >
class subdomain_deflation {
    public:
        typedef amg<
            Backend, Coarsening, Relax
            > AMG;

        typedef IterativeSolver<
            Backend, detail::mpi_inner_product
            > Solver;

        struct params {
            typename AMG::params          amg;
            typename Solver::params       solver;
            typename DirectSolver::params direct_solver;

            params() {}

            params(const boost::property_tree::ptree &p)
                : AMGCL_PARAMS_IMPORT_CHILD(p, amg),
                  AMGCL_PARAMS_IMPORT_CHILD(p, solver),
                  AMGCL_PARAMS_IMPORT_CHILD(p, direct_solver)
            {}

            void get(boost::property_tree::ptree &p, const std::string &path) const {
                AMGCL_PARAMS_EXPORT_CHILD(p, path, amg);
                AMGCL_PARAMS_EXPORT_CHILD(p, path, solver);
                AMGCL_PARAMS_EXPORT_CHILD(p, path, direct_solver);
            }
        };

        typedef typename Backend::value_type value_type;
        typedef typename Backend::matrix     matrix;
        typedef typename Backend::vector     vector;

        template <class Matrix, class DeflationVectors>
        subdomain_deflation(
                MPI_Comm mpi_comm,
                const Matrix &Astrip,
                const DeflationVectors &def_vec,
                const params &prm = params()
                )
        : comm(mpi_comm),
          nrows(backend::rows(Astrip)), ndv(def_vec.dim()),
          dtype( datatype<value_type>::get() ), dv_start(comm.size + 1, 0),
          Z( ndv ), q( Backend::create_vector(nrows, prm.amg.backend) ),
          req(2 * comm.size)
        {
            MPI_Datatype mpi_ptrdiff_t = mpi::datatype<ptrdiff_t>::get();

            TIC("setup deflation");
            typedef backend::crs<value_type, ptrdiff_t>                build_matrix;
            typedef typename backend::row_iterator<Matrix>::type       row_iterator1;
            typedef typename backend::row_iterator<build_matrix>::type row_iterator2;

            // Lets see how many deflation vectors are there.
            dv_size.resize(comm.size);
            MPI_Allgather(&ndv, 1, MPI_INT, &dv_size[0], 1, MPI_INT, comm);
            boost::partial_sum(dv_size, dv_start.begin() + 1);
            nz = dv_start.back();

            df.resize(ndv);
            dx.resize(nz);
            dd = Backend::create_vector(nz, prm.amg.backend);

            boost::shared_ptr<build_matrix> aloc = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> arem = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> az   = boost::make_shared<build_matrix>();

            // Get sizes of each domain in comm.
            std::vector<ptrdiff_t> domain(comm.size + 1, 0);
            MPI_Allgather(&nrows, 1, mpi_ptrdiff_t, &domain[1], 1, mpi_ptrdiff_t, comm);
            boost::partial_sum(domain, domain.begin());
            ptrdiff_t chunk_start = domain[comm.rank];

            // Fill deflation vectors.
            TIC("copy deflation vectors");
            {
                std::vector<value_type> z(nrows);
                for(int j = 0; j < ndv; ++j) {
                    for(ptrdiff_t i = 0; i < nrows; ++i)
                        z[i] = def_vec(i, j);
                    Z[j] = Backend::copy_vector(z, prm.amg.backend);
                }
            }
            TOC("copy deflation vectors");

            // Number of nonzeros in local and remote parts of the matrix.
            ptrdiff_t loc_nnz = 0, rem_nnz = 0;

            // Maps remote column numbers to local ids:
            std::map<ptrdiff_t, ptrdiff_t> rc;
            std::map<ptrdiff_t, ptrdiff_t>::iterator rc_it = rc.begin();

            TIC("first pass");
            // First pass over Astrip rows:
            // 1. Count local and remote nonzeros,
            // 2. Build set of remote columns,
            // 3. Build sparsity pattern of matrix AZ.
            az->nrows = nrows;
            az->ncols = nz;
            az->ptr.resize(nrows + 1, 0);

            std::vector<ptrdiff_t> marker(nz, -1);

            for(ptrdiff_t i = 0; i < nrows; ++i) {
                for(row_iterator1 a = backend::row_begin(Astrip, i); a; ++a) {
                    ptrdiff_t c = a.col();

                    // Domain the column belongs to
                    ptrdiff_t d = boost::upper_bound(domain, c) - domain.begin() - 1;

                    if (d == comm.rank) {
                        ++loc_nnz;
                    } else {
                        ++rem_nnz;
                        rc_it = rc.insert(rc_it, std::make_pair(c, 0));
                    }

                    if (marker[d] != i) {
                        marker[d] = i;
                        az->ptr[i + 1] += dv_size[d];
                    }
                }
            }
            TOC("first pass");

            TIC("setup communication");
            // Find out:
            // 1. How many columns do we need from each process,
            // 2. What columns do we need from them.
            //
            // Renumber remote columns while at it.
            std::vector<ptrdiff_t> num_recv(comm.size, 0);
            std::vector<ptrdiff_t> recv_cols;
            recv_cols.reserve(rc.size());
            ptrdiff_t id = 0, cur_nbr = 0;
            for(rc_it = rc.begin(); rc_it != rc.end(); ++rc_it) {
                rc_it->second = id++;
                recv_cols.push_back(rc_it->first);

                while(rc_it->first >= domain[cur_nbr + 1]) cur_nbr++;
                num_recv[cur_nbr]++;
            }

            /*** Set up communication pattern. ***/
            // Who sends to whom and how many
            boost::multi_array<ptrdiff_t, 2> comm_matrix(
                    boost::extents[comm.size][comm.size]
                    );

            MPI_Allgather(
                    num_recv.data(),    comm.size, mpi_ptrdiff_t,
                    comm_matrix.data(), comm.size, mpi_ptrdiff_t,
                    comm
                    );

            ptrdiff_t snbr = 0, rnbr = 0, send_size = 0;
            for(int i = 0; i < comm.size; ++i) {
                if (comm_matrix[comm.rank][i]) {
                    ++rnbr;
                }

                if (comm_matrix[i][comm.rank]) {
                    ++snbr;
                    send_size += comm_matrix[i][comm.rank];
                }
            }

            recv.nbr.reserve(rnbr);
            recv.ptr.reserve(rnbr + 1);
            recv.val.resize(rc.size());
            recv.req.resize(rnbr);

            dv = Backend::create_vector( rc.size(), prm.amg.backend );

            send.nbr.reserve(snbr);
            send.ptr.reserve(snbr + 1);
            send.val.resize(send_size);
            send.req.resize(snbr);

            std::vector<ptrdiff_t> send_col(send_size);

            // Count how many columns to send and to receive.
            recv.ptr.push_back(0);
            send.ptr.push_back(0);
            for(int i = 0; i < comm.size; ++i) {
                if (ptrdiff_t nr = comm_matrix[comm.rank][i]) {
                    recv.nbr.push_back( i );
                    recv.ptr.push_back( recv.ptr.back() + nr );
                }

                if (ptrdiff_t ns = comm_matrix[i][comm.rank]) {
                    send.nbr.push_back( i );
                    send.ptr.push_back( send.ptr.back() + ns );
                }
            }

            // What columns do you need from me?
            for(size_t i = 0; i < send.nbr.size(); ++i)
                MPI_Irecv(&send_col[send.ptr[i]], comm_matrix[send.nbr[i]][comm.rank],
                        mpi_ptrdiff_t, send.nbr[i], tag_exc_cols, comm, &send.req[i]);

            // Here is what I need from you:
            for(size_t i = 0; i < recv.nbr.size(); ++i)
                MPI_Isend(&recv_cols[recv.ptr[i]], comm_matrix[comm.rank][recv.nbr[i]],
                        mpi_ptrdiff_t, recv.nbr[i], tag_exc_cols, comm, &recv.req[i]);

            TOC("setup communication");
            /* While messages are in flight, */

            TIC("second pass");
            // Second pass over Astrip rows:
            // 1. Build local and remote matrix parts.
            // 2. Build local part of AZ matrix.
            aloc->nrows = nrows;
            aloc->ncols = nrows;
            aloc->ptr.reserve(nrows + 1);
            aloc->col.reserve(loc_nnz);
            aloc->val.reserve(loc_nnz);
            aloc->ptr.push_back(0);

            arem->nrows = nrows;
            arem->ncols = rc.size();
            arem->ptr.reserve(nrows + 1);
            arem->col.reserve(rem_nnz);
            arem->val.reserve(rem_nnz);
            arem->ptr.push_back(0);

            boost::partial_sum(az->ptr, az->ptr.begin());
            az->col.resize(az->ptr.back());
            az->val.resize(az->ptr.back());
            boost::fill(marker, -1);

            for(ptrdiff_t i = 0; i < nrows; ++i) {
                ptrdiff_t az_row_beg = az->ptr[i];
                ptrdiff_t az_row_end = az_row_beg;

                for(row_iterator1 a = backend::row_begin(Astrip, i); a; ++a) {
                    ptrdiff_t  c = a.col();
                    value_type v = a.value();

                    if ( domain[comm.rank] <= c && c < domain[comm.rank + 1] ) {
                        ptrdiff_t loc_c = c - chunk_start;
                        aloc->col.push_back(loc_c);
                        aloc->val.push_back(v);

                        for(ptrdiff_t j = 0, k = dv_start[comm.rank]; j < ndv; ++j, ++k) {
                            if (marker[k] < az_row_beg) {
                                marker[k] = az_row_end;
                                az->col[az_row_end] = k;
                                az->val[az_row_end] = v * def_vec(loc_c, j);
                                ++az_row_end;
                            } else {
                                az->val[marker[k]] += v * def_vec(loc_c, j);
                            }
                        }
                    } else {
                        arem->col.push_back(rc[c]);
                        arem->val.push_back(v);
                    }
                }

                az->ptr[i] = az_row_end;

                aloc->ptr.push_back(aloc->col.size());
                arem->ptr.push_back(arem->col.size());
            }
            TOC("second pass");

            /* Finish communication pattern setup. */
            MPI_Waitall(recv.req.size(), recv.req.data(), MPI_STATUSES_IGNORE);
            MPI_Waitall(send.req.size(), send.req.data(), MPI_STATUSES_IGNORE);

            // Shift columns to send to local numbering:
            BOOST_FOREACH(ptrdiff_t &c, send_col) c -= chunk_start;


            TIC("A*Z");
            /* Finish construction of AZ */
            // Exchange deflation vectors
            std::vector<ptrdiff_t> zrecv_ptr(recv.nbr.size() + 1, 0);
            std::vector<ptrdiff_t> zcol_ptr;
            zcol_ptr.reserve(recv.val.size() + 1);
            zcol_ptr.push_back(0);

            for(size_t i = 0; i < recv.nbr.size(); ++i) {
                ptrdiff_t size = dv_size[recv.nbr[i]] * (recv.ptr[i + 1] - recv.ptr[i]);
                zrecv_ptr[i + 1] = zrecv_ptr[i] + size;

                for(ptrdiff_t j = 0; j < size; ++j)
                    zcol_ptr.push_back(zcol_ptr.back() + dv_size[recv.nbr[i]]);
            }

            std::vector<value_type> zrecv(zrecv_ptr.back());
            std::vector<value_type> zsend(send.val.size() * ndv);

            for(size_t i = 0; i < recv.nbr.size(); ++i) {
                ptrdiff_t begin = zrecv_ptr[i];
                ptrdiff_t size  = zrecv_ptr[i + 1] - begin;

                MPI_Irecv(&zrecv[begin], size, dtype, recv.nbr[i],
                        tag_exc_vals, comm, &recv.req[i]);
            }

            for(size_t i = 0, k = 0; i < send_col.size(); ++i)
                for(ptrdiff_t j = 0; j < ndv; ++j, ++k)
                    zsend[k] = def_vec(send_col[i], j);

            for(size_t i = 0; i < send.nbr.size(); ++i)
                MPI_Isend(
                        &zsend[ndv * send.ptr[i]], ndv * (send.ptr[i+1] - send.ptr[i]),
                        dtype, send.nbr[i], tag_exc_vals, comm, &send.req[i]);

            MPI_Waitall(recv.req.size(), recv.req.data(), MPI_STATUSES_IGNORE);

            boost::fill(marker, -1);

            // AZ += Arem * Z
            for(ptrdiff_t i = 0; i < nrows; ++i) {
                ptrdiff_t az_row_beg = az->ptr[i];
                ptrdiff_t az_row_end = az_row_beg;

                for(row_iterator2 a = backend::row_begin(*arem, i); a; ++a) {
                    ptrdiff_t  c = a.col();
                    value_type v = a.value();

                    // Domain the column belongs to
                    ptrdiff_t d = recv.nbr[boost::upper_bound(recv.ptr, c) - recv.ptr.begin() - 1];

                    value_type *zval = &zrecv[ zcol_ptr[c] ];
                    for(ptrdiff_t j = 0, k = dv_start[d]; j < dv_size[d]; ++j, ++k) {
                        if (marker[k] < az_row_beg) {
                            marker[k] = az_row_end;
                            az->col[az_row_end] = k;
                            az->val[az_row_end] = v * zval[j];
                            ++az_row_end;
                        } else {
                            az->val[marker[k]] += v * zval[j];
                        }
                    }
                }

                az->ptr[i] = az_row_end;
            }

            std::rotate(az->ptr.begin(), az->ptr.end() - 1, az->ptr.end());
            az->ptr.front() = 0;
            TOC("A*Z");

            MPI_Waitall(send.req.size(), send.req.data(), MPI_STATUSES_IGNORE);

            /* Build deflated matrix E. */
            TIC("assemble E");
            // Who is responsible for solution of coarse problem
            nmasters = std::min(comm.size, DirectSolver::comm_size(nz));
            nslaves  = (comm.size + nmasters - 1) / nmasters;

            master = comm.rank / nslaves;

            if (comm.rank < nmasters) {
                slaves.resize(nmasters + 1, 0);

                for(int p = 0; p <= nmasters; ++p)
                    slaves[p] = std::min(p * nslaves, comm.size);

                nslaves = slaves[comm.rank + 1] - slaves[comm.rank];
            }

            // Count nonzeros in E.
            std::vector<int> eptr(ndv + 1, 0);
            for(int j = 0; j < comm.size; ++j) {
                if (j == comm.rank || comm_matrix[comm.rank][j]
                        || comm_matrix[j][comm.rank] // To keep coarse matrix graph symmetric
                   )
                {
                    for(int k = 0; k < ndv; ++k)
                        eptr[k + 1] += dv_size[j];
                }
            }

            std::vector<int> Eptr;

            if (comm.rank < nmasters) {
                Eptr.resize(dv_start[slaves[comm.rank+1]] - dv_start[slaves[comm.rank]] + 1);

                for(int p = slaves[comm.rank], offset = dv_start[p]; p < slaves[comm.rank + 1]; ++p) {
                    int begin = dv_start[p] - offset + 1;
                    int size  = dv_start[p + 1] - dv_start[p];
                    MPI_Irecv(&Eptr[begin], size, MPI_INT, p, tag_exc_lnnz, comm, &req[p]);
                }
            }

            MPI_Send(&eptr[1], ndv, MPI_INT, master, tag_exc_lnnz, comm);
            boost::partial_sum(eptr, eptr.begin());

            // Build local strip of E.
            boost::multi_array<value_type, 2> erow(boost::extents[ndv][nz]);
            std::fill_n(erow.data(), erow.num_elements(), 0);

            for(ptrdiff_t i = 0; i < nrows; ++i) {
                for(row_iterator2 a = backend::row_begin(*az, i); a; ++a) {
                    ptrdiff_t  c = a.col();
                    value_type v = a.value();

                    for(ptrdiff_t j = 0; j < ndv; ++j)
                        erow[j][c] += v * def_vec(i, j);
                }
            }

            std::vector<int>        ecol(eptr.back());
            std::vector<value_type> eval(eptr.back());
            for(int i = 0; i < ndv; ++i) {
                int row_head = eptr[i];
                for(int j = 0; j < comm.size; ++j) {
                    if (j == comm.rank || comm_matrix[comm.rank][j]
                            || comm_matrix[j][comm.rank] // To keep coarse matrix graph symmetric
                       )
                    {
                        for(int k = 0; k < dv_size[j]; ++k) {
                            int c = dv_start[j] + k;
                            ecol[row_head] = c;
                            eval[row_head] = erow[i][c];
                            ++row_head;
                        }
                    }
                }
            }

            // Exchange strips of E.
            std::vector<int>        Ecol;
            std::vector<value_type> Eval;
            if (comm.rank < nmasters) {
                MPI_Waitall(nslaves, &req[slaves[comm.rank]], MPI_STATUSES_IGNORE);
                boost::partial_sum(Eptr, Eptr.begin());

                Ecol.resize(Eptr.back());
                Eval.resize(Eptr.back());

                for(int p = slaves[comm.rank], offset = dv_start[p]; p < slaves[comm.rank + 1]; ++p) {
                    int begin = Eptr[dv_start[p]     - offset];
                    int size  = Eptr[dv_start[p + 1] - offset] - begin;

                    MPI_Irecv(&Ecol[begin], size, MPI_INT, p, tag_exc_dmat, comm, &req[2 * p + 0]);
                    MPI_Irecv(&Eval[begin], size, dtype,   p, tag_exc_dmat, comm, &req[2 * p + 1]);
                }
            }

            MPI_Send(ecol.data(), ecol.size(), MPI_INT, master, tag_exc_dmat, comm);
            MPI_Send(eval.data(), eval.size(), dtype,   master, tag_exc_dmat, comm);
            TOC("assemble E");

            // Prepare E factorization.
            TIC("factorize E");
            MPI_Comm_split(comm,
                    comm.rank < nmasters ? 0 : MPI_UNDEFINED,
                    comm.rank, &masters_comm
                    );

            if (comm.rank < nmasters) {
                MPI_Waitall(2 * nslaves, &req[2 * slaves[comm.rank]], MPI_STATUSES_IGNORE);

                E = boost::make_shared<DirectSolver>(
                        masters_comm, Eptr.size() - 1, Eptr, Ecol, Eval, prm.direct_solver
                        );

                cf.resize(Eptr.size() - 1);
                cx.resize(Eptr.size() - 1);
            }
            TOC("factorize E");

            TOC("setup deflation");

            // Create local AMG preconditioner.
            P = boost::make_shared<AMG>( *aloc, prm.amg );

            // Create iterative solver instance.
            solve = boost::make_shared<Solver>(
                    nrows, prm.solver, prm.amg.backend,
                    detail::mpi_inner_product(mpi_comm)
                    );

            // Move matrices to backend.
            Arem = Backend::copy_matrix(arem, prm.amg.backend);
            AZ   = Backend::copy_matrix(az,   prm.amg.backend);

            // Columns gatherer. Will retrieve columns to send from backend.
            gather = boost::make_shared<typename Backend::gather>(
                    nrows, send_col, prm.amg.backend);
        }

        ~subdomain_deflation() {
            E.reset();
            if (masters_comm != MPI_COMM_NULL) MPI_Comm_free(&masters_comm);
        }

        /// Fills the property tree with the actual parameters used.
        void get_params(boost::property_tree::ptree &p) const {
            P->prm.get(p, "amg.");
            solve->prm.get(p, "solver.");
        }

        template <class Vec1, class Vec2>
        boost::tuple<size_t, value_type>
        operator()(const Vec1 &rhs, Vec2 &x) const {
            boost::tuple<size_t, value_type> cnv = (*solve)(*this, *P, rhs, x);
            postprocess(rhs, x);
            return cnv;
        }

        template <class Vec1, class Vec2>
        void mul_n_project(value_type alpha, const Vec1 &x, value_type beta, Vec2 &y) const {
            mul(alpha, x, beta, y);
            project(y);
        }

        template <class Vec1, class Vec2, class Vec3>
        void residual(const Vec1 &f, const Vec2 &x, Vec3 &r) const {
            TIC("top/residual");
            start_exchange(x);
            backend::residual(f, P->top_matrix(), x, r);

            finish_exchange();

            if (!recv.val.empty()) {
                backend::copy_to_backend(recv.val, *dv);
                backend::spmv(-1, *Arem, *dv, 1, r);
            }
            TOC("top/residual");

            project(r);
        }
    private:
        static const int tag_exc_cols = 1001;
        static const int tag_exc_vals = 2001;
        static const int tag_exc_dmat = 3001;
        static const int tag_exc_dvec = 4001;
        static const int tag_exc_lnnz = 5001;

        communicator comm;
        ptrdiff_t nrows;
        int ndv, nz;

        MPI_Datatype dtype;

        boost::shared_ptr<matrix> Arem;

        boost::shared_ptr<AMG>    P;
        boost::shared_ptr<Solver> solve;

        mutable std::vector<value_type> df, dx, cf, cx;
        std::vector<int> dv_start, dv_size;

        std::vector< boost::shared_ptr<vector> > Z;

        MPI_Comm masters_comm;
        int nmasters, nslaves, master;
        std::vector<int> slaves;
        boost::shared_ptr<DirectSolver> E;

        boost::shared_ptr<matrix> AZ;
        boost::shared_ptr<vector> q;
        boost::shared_ptr<vector> dd;
        boost::shared_ptr<vector> dv;

        boost::shared_ptr< typename Backend::gather > gather;

        mutable std::vector<MPI_Request> req;

        struct {
            std::vector<ptrdiff_t> nbr;
            std::vector<ptrdiff_t> ptr;

            mutable std::vector<value_type>  val;
            mutable std::vector<MPI_Request> req;
        } recv;

        struct {
            std::vector<ptrdiff_t> nbr;
            std::vector<ptrdiff_t> ptr;

            mutable std::vector<value_type>  val;
            mutable std::vector<MPI_Request> req;
        } send;

        template <class Vec1, class Vec2>
        void mul(value_type alpha, const Vec1 &x, value_type beta, Vec2 &y) const {
            TIC("top/spmv");

            start_exchange(x);
            backend::spmv(alpha, P->top_matrix(), x, beta, y);

            finish_exchange();

            if (!recv.val.empty()) {
                backend::copy_to_backend(recv.val, *dv);
                backend::spmv(alpha, *Arem, *dv, 1, y);
            }

            TOC("top/spmv");
        }

        template <class Vector>
        void project(Vector &x) const {
            TIC("project");

            TIC("local inner product");
            for(ptrdiff_t j = 0; j < ndv; ++j)
                df[j] = backend::inner_product(x, *Z[j]);
            TOC("local inner product");

            coarse_solve(df, dx);

            TIC("spmv");
            backend::copy_to_backend(dx, *dd);
            backend::spmv(-1, *AZ, *dd, 1, x);
            TOC("spmv");

            TOC("project");
        }

        template <class Vec1, class Vec2>
        void postprocess(const Vec1 &rhs, Vec2 &x) const {
            TIC("postprocess");

            // q = Ax
            mul(1, x, 0, *q);

            // df = transp(Z) * (rhs - Ax)
            TIC("local inner product");
            for(ptrdiff_t j = 0; j < ndv; ++j)
                df[j] = backend::inner_product(rhs, *Z[j])
                      - backend::inner_product(*q,  *Z[j]);
            TOC("local inner product");

            // dx = inv(E) * df
            coarse_solve(df, dx);

            // x += Z * dx
            ptrdiff_t j = 0, k = dv_start[comm.rank];
            for(; j + 1 < ndv; j += 2, k += 2)
                backend::axpbypcz(dx[k], *Z[j], dx[k+1], *Z[j+1], 1, x);

            for(; j < ndv; ++j, ++k)
                backend::axpby(dx[k], *Z[j], 1, x);

            TOC("postprocess");
        }

        template <class Vector>
        void start_exchange(const Vector &x) const {
            // Start receiving ghost values from our neighbours.
            for(size_t i = 0; i < recv.nbr.size(); ++i)
                MPI_Irecv(
                        &recv.val[recv.ptr[i]], recv.ptr[i+1] - recv.ptr[i],
                        dtype, recv.nbr[i], tag_exc_vals, comm, &recv.req[i]);

            // Gather values to send to our neighbours.
            if (!send.val.empty()) (*gather)(x, send.val);

            // Start sending our data to neighbours.
            for(size_t i = 0; i < send.nbr.size(); ++i)
                MPI_Isend(
                        &send.val[send.ptr[i]], send.ptr[i+1] - send.ptr[i],
                        dtype, send.nbr[i], tag_exc_vals, comm, &send.req[i]);
        }

        void finish_exchange() const {
            MPI_Waitall(recv.req.size(), recv.req.data(), MPI_STATUSES_IGNORE);
            MPI_Waitall(send.req.size(), send.req.data(), MPI_STATUSES_IGNORE);
        }

        void coarse_solve(std::vector<value_type> &f, std::vector<value_type> &x) const
        {
            TIC("coarse solve");
            TIC("exchange rhs");
            MPI_Gatherv(f.data(), f.size(), dtype, x.data(), dv_size.data(), dv_start.data(), dtype, 0, comm);

            if (comm.rank == 0) {
                for(int p = 0; p < nmasters; ++p) {
                    MPI_Isend(
                            &x[dv_start[slaves[p]]],
                            dv_start[slaves[p + 1]] - dv_start[slaves[p]],
                            dtype, p, tag_exc_dvec, comm, &req[p]
                            );
                }
            }

            if (comm.rank < nmasters) {
                MPI_Recv(
                        cf.data(),
                        dv_start[slaves[comm.rank + 1]] - dv_start[slaves[comm.rank]],
                        dtype, 0, tag_exc_dvec, comm, MPI_STATUS_IGNORE
                        );
            }

            if (comm.rank == 0) MPI_Waitall(nslaves, req.data(), MPI_STATUSES_IGNORE);
            TOC("exchange rhs");

            if (comm.rank < nmasters) {
                TIC("call solver");
                (*E)(cf, cx);
                TOC("call solver");

                TIC("gather result");
                if (comm.rank == 0) {
                    for(int p = 0; p < nmasters; ++p) {
                        int begin = dv_start[slaves[p]];
                        int size  = dv_start[slaves[p+1]] - begin;
                        MPI_Irecv(&x[begin], size, dtype, p, tag_exc_dvec, comm, &req[p]);
                    }
                }

                MPI_Send(cx.data(), cx.size(), dtype, 0, tag_exc_dvec, comm);

                if (comm.rank == 0)
                    MPI_Waitall(nmasters, req.data(), MPI_STATUSES_IGNORE);
                TOC("gather result");
            }

            TIC("broadcast result");
            MPI_Bcast(x.data(), x.size(), dtype, 0, comm);
            TOC("broadcast result");
            TOC("coarse solve");
        }
};

} // namespace mpi

namespace backend {

template <
    class                         Backend,
    class                         Coarsening,
    template <class> class        Relax,
    template <class, class> class IterativeSolver,
    class                         DirectSolver,
    class Vec1,
    class Vec2
    >
struct spmv_impl<
    mpi::subdomain_deflation<
        Backend,
        Coarsening,
        Relax,
        IterativeSolver,
        DirectSolver
        >,
    Vec1, Vec2
    >
{
    typedef
        mpi::subdomain_deflation<
            Backend,
            Coarsening,
            Relax,
            IterativeSolver,
            DirectSolver
            >
        M;

    typedef typename Backend::value_type V;

    static void apply(V alpha, const M &A, const Vec1 &x, V beta, Vec2 &y)
    {
        A.mul_n_project(alpha, x, beta, y);
    }
};

template <
    class                         Backend,
    class                         Coarsening,
    template <class> class        Relax,
    template <class, class> class IterativeSolver,
    class                         DirectSolver,
    class Vec1,
    class Vec2,
    class Vec3
    >
struct residual_impl<
    mpi::subdomain_deflation<
        Backend,
        Coarsening,
        Relax,
        IterativeSolver,
        DirectSolver
        >,
    Vec1, Vec2, Vec3
    >
{
    typedef
        mpi::subdomain_deflation<
            Backend,
            Coarsening,
            Relax,
            IterativeSolver,
            DirectSolver
            >
        M;

    typedef typename Backend::value_type V;

    static void apply(const Vec1 &rhs, const M &A, const Vec2 &x, Vec3 &r) {
        A.residual(rhs, x, r);
    }
};

} // namespace backend

} // namespace amgcl

#endif
