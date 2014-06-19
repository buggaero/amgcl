#ifndef AMGCL_MPI_DEFLATION_HPP
#define AMGCL_MPI_DEFLATION_HPP

/*
The MIT License

Copyright (c) 2012-2014 Denis Demidov <dennis.demidov@gmail.com>

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
 * \file   amgcl/mpi/deflatedion.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  Subdomain deflation utilities.
 */

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/range/numeric.hpp>
#include <boost/multi_array.hpp>
#include <boost/mpi.hpp>

#include <amgcl/amgcl.hpp>
#include <amgcl/backend/builtin.hpp>

namespace amgcl {

/// Algorithms and structures for distributed computing.
namespace mpi {

namespace detail {
struct mpi_inner_product {
    boost::mpi::communicator comm;

    mpi_inner_product(MPI_Comm comm) : comm(comm, boost::mpi::comm_attach) {}

    template <class Vec1, class Vec2>
    typename backend::value_type<Vec1>::type
    operator()(const Vec1 &x, const Vec2 &y) const {
        typedef typename backend::value_type<Vec1>::type value_type;

        value_type lsum = backend::inner_product(x, y);
        value_type gsum;

        all_reduce( comm, lsum, gsum, std::plus<value_type>() );

        return gsum;
    }
};

} // namespace detail

/// Distributed solver with subdomain deflation.
/**
 * \sa \cite Frank2001
 */
template <
    class                         Backend,
    class                         Coarsening,
    template <class> class        Relax,
    template <class, class> class IterativeSolver
    >
class subdomain_deflation {
    public:
        typedef amg<
            Backend, Coarsening, Relax
            > AMG;

        typedef IterativeSolver<
            Backend, detail::mpi_inner_product
            > Solver;

        typedef
            typename AMG::params
            AMG_params;

        typedef
            typename Solver::params
            Solver_params;

        typedef typename Backend::value_type value_type;
        typedef typename Backend::matrix     matrix;
        typedef typename Backend::vector     vector;

        template <class Matrix>
        subdomain_deflation(
                MPI_Comm mpi_comm,
                const Matrix &Astrip,
                const AMG_params    &amg_params    = AMG_params(),
                const Solver_params &solver_params = Solver_params()
                )
        : comm(mpi_comm, boost::mpi::comm_attach), nrows(backend::rows(Astrip)),
          df( comm.size() ), dx( comm.size() ),
          E( boost::extents[comm.size()][comm.size()] ),
          q(Backend::create_vector(nrows, amg_params.backend))
        {
            typedef typename backend::row_iterator<Matrix>::type row_iterator;
            typedef backend::crs<value_type, long> build_matrix;

            boost::shared_ptr<build_matrix> aloc = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> arem = boost::make_shared<build_matrix>();
            boost::shared_ptr<build_matrix> az   = boost::make_shared<build_matrix>();

            // Get sizes of each domain in comm.
            std::vector<long> domain(comm.size() + 1, 0);
            all_gather(comm, nrows, &domain[1]);
            boost::partial_sum(domain, domain.begin());
            long chunk_start = domain[comm.rank()];

            // Number of nonzeros in local and remote parts of the matrix.
            long loc_nnz = 0, rem_nnz = 0;

            // Local contribution to E.
            std::vector<value_type> erow(comm.size(), 0);

            // Maps remote column numbers to local ids:
            std::map<long, long> rc;
            std::map<long, long>::iterator rc_it = rc.begin();

            // First pass over Astrip rows:
            // 1. Count local and remote nonzeros,
            // 2. Build set of remote columns,
            // 3. Compute local contribution to E = (Z^t A Z),
            // 4. Build sparsity pattern of matrix AZ.
            az->nrows = nrows;
            az->ncols = comm.size();
            az->ptr.resize(nrows + 1, 0);

            std::vector<long> marker(comm.size(), -1);
            for(long i = 0; i < nrows; ++i) {
                for(row_iterator a = backend::row_begin(Astrip, i); a; ++a) {
                    long       c = a.col();
                    value_type v = a.value();

                    // Domain the column belongs to
                    long d = boost::upper_bound(domain, c) - domain.begin() - 1;

                    if (d == comm.rank()) {
                        ++loc_nnz;
                    } else {
                        ++rem_nnz;
                        rc_it = rc.insert(rc_it, std::make_pair(c, 0));
                    }

                    erow[d] += v;

                    if (marker[d] != i) {
                        marker[d] = i;
                        ++( az->ptr[i + 1] );
                    }
                }
            }

            // Exchange rows of E.
            all_gather( comm, erow.data(), comm.size(), E.data() );

            // Invert E.
            amgcl::detail::gaussj(comm.size(), E.data());

            // Find out:
            // 1. How many columns do we need from each process,
            // 2. What columns do we need from them.
            //
            // Renumber remote columns while at it.
            std::vector<long> num_recv(comm.size(), 0);
            std::vector<long> recv_cols;
            recv_cols.reserve(rc.size());
            long id = 0, cur_nbr = 0;
            for(rc_it = rc.begin(); rc_it != rc.end(); ++rc_it) {
                rc_it->second = id++;
                recv_cols.push_back(rc_it->first);

                while(rc_it->first >= domain[cur_nbr + 1]) cur_nbr++;
                num_recv[cur_nbr]++;
            }

            // Second pass over Astrip rows:
            // 1. Build local and remote matrix parts.
            // 2. Build AZ matrix.
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

            for(long i = 0; i < nrows; ++i) {
                long az_row_beg = az->ptr[i];
                long az_row_end = az_row_beg;

                for(row_iterator a = backend::row_begin(Astrip, i); a; ++a) {
                    long       c = a.col();
                    value_type v = a.value();

                    // Domain the column belongs to
                    long d = boost::upper_bound(domain, c) - domain.begin() - 1;

                    if ( d == comm.rank() ) {
                        aloc->col.push_back(c - chunk_start);
                        aloc->val.push_back(v);
                    } else {
                        arem->col.push_back(rc[c]);
                        arem->val.push_back(v);
                    }

                    if (marker[d] < az_row_beg) {
                        marker[d] = az_row_end;
                        az->col[az_row_end] = d;
                        az->val[az_row_end] = v;
                        ++az_row_end;
                    } else {
                        az->val[marker[d]] += v;
                    }
                }

                aloc->ptr.push_back(aloc->col.size());
                arem->ptr.push_back(arem->col.size());
            }

            // Set up communication pattern.
            boost::multi_array<long, 2> comm_matrix(
                    boost::extents[comm.size()][comm.size()]
                    );

            all_gather(comm, num_recv.data(), comm.size(), comm_matrix.data());

            long snbr = 0, rnbr = 0, send_size = 0;
            for(int i = 0; i < comm.size(); ++i) {
                if (comm_matrix[comm.rank()][i]) {
                    ++rnbr;
                }

                if (comm_matrix[i][comm.rank()]) {
                    ++snbr;
                    send_size += comm_matrix[i][comm.rank()];
                }
            }

            recv.nbr.reserve(rnbr);
            recv.ptr.reserve(rnbr + 1);
            recv.val.resize(rc.size());
            recv.req.resize(rnbr);

            send.nbr.reserve(snbr);
            send.ptr.reserve(snbr + 1);
            send.col.resize(send_size);
            send.val.resize(send_size);
            send.req.resize(snbr);

            recv.ptr.push_back(0);
            send.ptr.push_back(0);
            for(int i = 0; i < comm.size(); ++i) {
                if (long nr = comm_matrix[comm.rank()][i]) {
                    recv.nbr.push_back( i );
                    recv.ptr.push_back( recv.ptr.back() + nr );
                }

                if (long ns = comm_matrix[i][comm.rank()]) {
                    send.nbr.push_back( i );
                    send.ptr.push_back( send.ptr.back() + ns );
                }
            }

            for(size_t i = 0; i < send.nbr.size(); ++i)
                send.req[i] = comm.irecv(send.nbr[i], tag_exc_vals,
                        &send.col[send.ptr[i]], comm_matrix[send.nbr[i]][comm.rank()]);

            for(size_t i = 0; i < recv.nbr.size(); ++i)
                recv.req[i] = comm.isend(recv.nbr[i], tag_exc_vals,
                    &recv_cols[recv.ptr[i]], comm_matrix[comm.rank()][recv.nbr[i]]);

            wait_all(recv.req.begin(), recv.req.end());
            wait_all(send.req.begin(), send.req.end());

            BOOST_FOREACH(long &c, send.col) c -= chunk_start;

            P = boost::make_shared<AMG>( *aloc, amg_params );

            solve = boost::make_shared<Solver>(
                    nrows, solver_params, amg_params.backend,
                    detail::mpi_inner_product(mpi_comm)
                    );

            Arem = Backend::copy_matrix(arem, amg_params.backend);
            AZ   = Backend::copy_matrix(az,   amg_params.backend);
        }

        template <class Vec1, class Vec2>
        boost::tuple<size_t, value_type>
        operator()(const Vec1 &rhs, Vec2 &x) const {
            boost::tuple<size_t, value_type> cnv = (*solve)(*this, *this, rhs, x);
            postprocess(rhs, x);
            return cnv;
        }

        template <class Vec1, class Vec2>
        void apply(const Vec1 &rhs, Vec2 &x) const {
            P->apply(rhs, x);
        }

        template <class Vec1, class Vec2>
        void mul_n_project(value_type alpha, const Vec1 &x, value_type beta, Vec2 &y) const {
            mul(alpha, x, beta, y);
            project(y);
        }

        template <class Vec1, class Vec2, class Vec3>
        void residual(const Vec1 &f, const Vec2 &x, Vec3 &r) const {
            start_exchange(x);
            backend::residual(f, P->top_matrix(), x, r);

            finish_exchange();
            backend::spmv(-1, *Arem, recv.val, 1, r);

            project(r);
        }
    private:
        static const int tag_exc_cols = 1001;
        static const int tag_exc_vals = 2001;

        boost::mpi::communicator comm;
        long nrows;

        boost::shared_ptr<matrix> Arem;

        boost::shared_ptr<AMG>    P;
        boost::shared_ptr<Solver> solve;

        mutable std::vector<value_type> df, dx;
        boost::multi_array<value_type, 2> E;
        boost::shared_ptr<matrix> AZ;
        boost::shared_ptr<vector> q;

        struct {
            std::vector<long> nbr;
            std::vector<long> ptr;

            mutable std::vector<value_type>          val;
            mutable std::vector<boost::mpi::request> req;
        } recv;

        struct {
            std::vector<long> nbr;
            std::vector<long> ptr;
            std::vector<long> col;

            mutable std::vector<value_type>          val;
            mutable std::vector<boost::mpi::request> req;
        } send;

        template <class Vec1, class Vec2>
        void mul(value_type alpha, const Vec1 &x, value_type beta, Vec2 &y) const {
            start_exchange(x);
            backend::spmv(alpha, P->top_matrix(), x, beta, y);

            finish_exchange();
            backend::spmv(alpha, *Arem, recv.val, 1, y);
        }

        template <class Vector>
        void project(Vector &x) const {
            value_type sum_x = backend::sum(x);
            all_gather(comm, sum_x, df.data());

            for(long i = 0; i < comm.size(); ++i) {
                value_type sum = 0;
                for(long j = 0; j < comm.size(); ++j)
                    sum += E[i][j] * df[j];
                dx[i] = sum;
            }

            backend::spmv(-1, *AZ, dx, 1, x);
        }

        template <class Vec1, class Vec2>
        void postprocess(const Vec1 &f, Vec2 &x) const {
            value_type sum = backend::sum(f);
            all_gather(comm, sum, df.data());

            value_type corr = 0;
            for(long j = 0; j < comm.size(); ++j)
                corr += E[comm.rank()][j] * df[j];

            mul(1, x, 0, *q);

            sum = backend::sum(*q);
            all_gather(comm, sum, df.data());

            for(long j = 0; j < comm.size(); ++j)
                corr -= E[comm.rank()][j] * df[j];

            for(long i = 0; i < nrows; ++i) x[i] += corr;
        }

        template <class Vector>
        void start_exchange(const Vector &x) const {
            // Start receiving ghost values from our neighbours.
            for(size_t i = 0; i < recv.nbr.size(); ++i)
                recv.req[i] = comm.irecv(recv.nbr[i], tag_exc_vals,
                        &recv.val[recv.ptr[i]], recv.ptr[i+1] - recv.ptr[i]);

            // Gather values to send to our neighbours.
            for(size_t i = 0; i < send.col.size(); ++i)
                send.val[i] = x[send.col[i]];

            // Start sending our data to neighbours.
            for(size_t i = 0; i < send.nbr.size(); ++i)
                send.req[i] = comm.isend(send.nbr[i], tag_exc_vals,
                        &send.val[send.ptr[i]], send.ptr[i+1] - send.ptr[i]);
        }

        void finish_exchange() const {
            wait_all(recv.req.begin(), recv.req.end());
            wait_all(send.req.begin(), send.req.end());
        }
};

} // namespace mpi

namespace backend {

template <
    class                         Backend,
    class                         Coarsening,
    template <class> class        Relax,
    template <class, class> class IterativeSolver,
    class Vec1,
    class Vec2
    >
struct spmv_impl<
    mpi::subdomain_deflation<Backend, Coarsening, Relax, IterativeSolver>,
    Vec1, Vec2
    >
{
    typedef mpi::subdomain_deflation<Backend, Coarsening, Relax, IterativeSolver> M;
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
    class Vec1,
    class Vec2,
    class Vec3
    >
struct residual_impl<
    mpi::subdomain_deflation<Backend, Coarsening, Relax, IterativeSolver>,
    Vec1, Vec2, Vec3
    >
{
    typedef mpi::subdomain_deflation<Backend, Coarsening, Relax, IterativeSolver> M;
    typedef typename Backend::value_type V;

    static void apply(const Vec1 &rhs, const M &A, const Vec2 &x, Vec3 &r) {
        A.residual(rhs, x, r);
    }
};

} // namespace backend

} // namespace amgcl

#endif
