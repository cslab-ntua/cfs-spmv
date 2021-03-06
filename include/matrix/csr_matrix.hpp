#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <iomanip>
#include <memory>

#include <boost/functional/hash.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/bandwidth.hpp>
#include <boost/graph/cuthill_mckee_ordering.hpp>
#include <boost/graph/properties.hpp>
#include <boost/ref.hpp>

#include <omp.h>
#include <tbb/concurrent_vector.h>

#include "io/mmf.hpp"
#include "utils/allocator.hpp"
#include "utils/platforms.hpp"
#include "utils/runtime.hpp"

#define BALANCING_STEPS 1
#define MAX_THREADS 28
#define MAX_COLORS MAX_THREADS

using namespace std;
using namespace util;
using namespace util::io;
using namespace util::runtime;

namespace matrix {
namespace sparse {

// Forward declarations
template <typename IndexT, typename ValueT> class SparseMatrix;

struct ConflictMap {
  int length;
  short *cpu;
  int *pos;
};

struct WeightedVertex {
  int vid;
  int tid;
  int nnz;

  WeightedVertex() : vid(0), tid(0), nnz(0) {}
  WeightedVertex(int vertex_id, int thread_id, int nnz)
      : vid(vertex_id), tid(thread_id), nnz(nnz) {}
};

struct CompareWeightedVertex {
  bool operator()(const WeightedVertex &lhs, const WeightedVertex &rhs) {
    // return lhs.nnz < rhs.nnz;
    return lhs.vid > rhs.vid;
  }
};

// FIXME: convert to nested class
template <typename IndexT, typename ValueT> struct SymmetryCompressionData {
  int nrows_;            // Number of rows in lower triangular submatrix
  int nrows_h_;          // Number of rows in high-bandwidth submatrix
  int nnz_lower_;        // Number of nonzeros in lower triangular submatrix
  int nnz_h_;            // Number of nonzeros in high-bandwidth submatrix
  int nnz_diag_;         // Number of nonzeros in diagonal
  IndexT *rowptr_;       // Row pointer array for lower triangular submatrix
  IndexT *colind_;       // Column index array for lower triangular submatrix
  ValueT *values_;       // Values array for lower triangular submatrix
  IndexT *rowptr_h_;     // Row pointer array for high-bandwidth submatrix
  IndexT *colind_h_;     // Column index array for high-bandwidth submatrix
  ValueT *values_h_;     // Values array for high-bandwidth submatrix
  ValueT *diagonal_;     // Values of diagonal elements (padded)
  ValueT *local_vector_; // Local vector for reduction-based methods
  IndexT *range_ptr_;    // Number of ranges per color
  IndexT *range_start_, *range_end_; // Ranges start/end
  int map_start_, map_end_;          // Conflict map start/end
  int nranges_;                      // Total number or ranges
  int ncolors_;                      // Number or colors
  vector<int> deps_[MAX_COLORS];
  Platform platform_;

public:
  SymmetryCompressionData()
      : rowptr_(nullptr), colind_(nullptr), values_(nullptr),
        rowptr_h_(nullptr), colind_h_(nullptr), values_h_(nullptr),
        diagonal_(nullptr), local_vector_(nullptr), range_ptr_(nullptr),
        range_start_(nullptr), range_end_(nullptr), ncolors_(0),
        platform_(Platform::cpu) {}

  SymmetryCompressionData(Platform platform)
      : rowptr_(nullptr), colind_(nullptr), values_(nullptr),
        rowptr_h_(nullptr), colind_h_(nullptr), values_h_(nullptr),
        diagonal_(nullptr), local_vector_(nullptr), range_ptr_(nullptr),
        range_start_(nullptr), range_end_(nullptr), ncolors_(0),
        platform_(platform) {}

  ~SymmetryCompressionData() {
    internal_free(rowptr_, platform_);
    internal_free(colind_, platform_);
    internal_free(values_, platform_);
    internal_free(rowptr_h_, platform_);
    internal_free(colind_h_, platform_);
    internal_free(values_h_, platform_);
    internal_free(diagonal_, platform_);
    internal_free(local_vector_, platform_);
    internal_free(range_ptr_, platform_);
    internal_free(range_start_, platform_);
    internal_free(range_end_, platform_);
  }
};

template <typename IndexT, typename ValueT>
class CSRMatrix : public SparseMatrix<IndexT, ValueT> {
  typedef tbb::concurrent_vector<tbb::concurrent_vector<int>> ColoringGraph;

public:
  CSRMatrix() = delete;

  // Initialize CSR from an MMF file
  CSRMatrix(const string &filename, Platform platform = Platform::cpu,
            bool symmetric = false, bool hybrid = false)
      : platform_(platform), hybrid_(hybrid), owns_data_(true) {
    MMF<IndexT, ValueT> mmf(filename);
    symmetric_ = mmf.IsSymmetric();
    if (!symmetric) {
      symmetric_ = false;
#ifdef _LOG_INFO
      cout << "[INFO]: using CSR format to store the sparse matrix..." << endl;
#endif
    }
    if (symmetric) {
      if (symmetric_ != symmetric) {
#ifdef _LOG_INFO
        cout << "[INFO]: matrix is not symmetric!" << endl;
        cout << "[INFO]: rolling back to CSR format..." << endl;
#endif
      } else {
#ifdef _LOG_INFO
        cout << "[INFO]: using SSS format to store the sparse matrix..."
             << endl;
#endif
      }
    }
    nrows_ = mmf.GetNrRows();
    ncols_ = mmf.GetNrCols();
    nnz_ = mmf.GetNrNonzeros();
    rowptr_ =
        (IndexT *)internal_alloc((nrows_ + 1) * sizeof(IndexT), platform_);
    colind_ = (IndexT *)internal_alloc(nnz_ * sizeof(IndexT), platform_);
    values_ = (ValueT *)internal_alloc(nnz_ * sizeof(ValueT), platform_);
    // Hybrid
    rowptr_h_ = colind_h_ = nullptr;
    values_h_ = nullptr;
    // Partitioning
    split_nnz_ = false;
    nthreads_ = get_threads();
    row_split_ = nullptr;
    // Symmetry compression
    cmp_symmetry_ = atomics_ = effective_ranges_ = local_vectors_indexing_ =
        conflict_free_apriori_ = conflict_free_aposteriori_ = false;
    nnz_lower_ = nnz_diag_ = nrows_left_ = nconflicts_ = ncolors_ = nranges_ =
        0;
    rowptr_sym_ = rowind_sym_ = colind_sym_ = nullptr;
    values_sym_ = diagonal_ = nullptr;
    color_ptr_ = nullptr;
    cnfl_map_ = nullptr;

    // Enforce first touch policy
    #pragma omp parallel num_threads(nthreads_)
    {
      #pragma omp for schedule(static)
      for (int i = 0; i < nrows_ + 1; i++) {
        rowptr_[i] = 0;
      }
      #pragma omp for schedule(static)
      for (int i = 0; i < nnz_; i++) {
        colind_[i] = 0;
        values_[i] = 0.0;
      }
    }

    auto iter = mmf.begin();
    auto iter_end = mmf.end();
    IndexT row_i = 0, val_i = 0, row_prev = 0;
    IndexT row, col;
    ValueT val;

    rowptr_[row_i++] = val_i;
    for (; iter != iter_end; ++iter) {
      // MMF returns one-based indices
      row = (*iter).row - 1;
      col = (*iter).col - 1;
      val = (*iter).val;
      assert(row >= row_prev);
      assert(row < nrows_);
      assert(col >= 0 && col < ncols_);
      assert(val_i < nnz_);

      if (row != row_prev) {
        for (IndexT i = 0; i < row - row_prev; i++) {
          rowptr_[row_i++] = val_i;
        }
        row_prev = row;
      }

      colind_[val_i] = (IndexT)col;
      values_[val_i] = val;
      val_i++;
    }

    rowptr_[row_i] = val_i;

    // More sanity checks.
    assert(row_i == nrows_);
    assert(val_i == nnz_);

    // reorder();
    if (nthreads_ == 1)
      hybrid_ = false;
    if (nthreads_ > 1 && hybrid_)
      split_by_bandwidth();
    split_by_nnz(nthreads_);
  }

  // Initialize CSR from another CSR matrix (no ownership)
  CSRMatrix(IndexT *rowptr, IndexT *colind, ValueT *values, IndexT nrows,
            IndexT ncols, bool symmetric = false, bool hybrid = false,
            Platform platform = Platform::cpu)
      : platform_(platform), nrows_(nrows), ncols_(ncols),
        symmetric_(symmetric), hybrid_(hybrid), owns_data_(false) {
    rowptr_ = rowptr;
    colind_ = colind;
    values_ = values;
    nnz_ = rowptr_[nrows];
    // Hybrid
    rowptr_h_ = colind_h_ = nullptr;
    values_h_ = nullptr;
    // Partitioning
    split_nnz_ = false;
    nthreads_ = get_threads();
    row_split_ = nullptr;
    // Symmetry compression
    cmp_symmetry_ = atomics_ = effective_ranges_ = local_vectors_indexing_ =
        conflict_free_apriori_ = conflict_free_aposteriori_ = false;
    nnz_lower_ = nnz_diag_ = nrows_left_ = nconflicts_ = ncolors_ = nranges_ =
        0;
    rowptr_sym_ = rowind_sym_ = colind_sym_ = nullptr;
    values_sym_ = diagonal_ = nullptr;
    color_ptr_ = nullptr;
    cnfl_map_ = nullptr;

    // reorder();
    if (nthreads_ == 1)
      hybrid_ = false;
    if (nthreads_ > 1 && hybrid_)
      split_by_bandwidth();
    split_by_nnz(nthreads_);
  }

  virtual ~CSRMatrix() {
    // If CSRMatrix was initialized using pre-defined arrays, we release
    // ownership.
    if (owns_data_) {
      internal_free(rowptr_, platform_);
      internal_free(colind_, platform_);
      internal_free(values_, platform_);
    } else {
      rowptr_ = nullptr;
      colind_ = nullptr;
      values_ = nullptr;
    }

    internal_free(row_split_, platform_);

    if (hybrid_) {
      internal_free(rowptr_h_, platform_);
      internal_free(colind_h_, platform_);
      internal_free(values_h_, platform_);
    }

    internal_free(diagonal_, platform_);

    if (conflict_free_apriori_) {
      internal_free(rowptr_sym_, platform_);
      internal_free(rowind_sym_, platform_);
      internal_free(colind_sym_, platform_);
      internal_free(values_sym_, platform_);
      internal_free(color_ptr_, platform_);
    }

    if (local_vectors_indexing_) {
      internal_free(cnfl_map_->cpu);
      internal_free(cnfl_map_->pos);
      delete cnfl_map_;
    }

    if (cmp_symmetry_)
      for (int i = 0; i < nthreads_; ++i)
        delete sym_cmp_data_[i];
    internal_free(sym_cmp_data_, platform_);
  }

  virtual int nrows() const override { return nrows_; }
  virtual int ncols() const override { return ncols_; }
  virtual int nnz() const override { return nnz_; }
  virtual bool symmetric() const override { return symmetric_; }

  virtual size_t size() const override {
    size_t size = 0;
    if (cmp_symmetry_) {
      size += (nrows_ + 1 * nthreads_) * sizeof(IndexT); // rowptr
      size += nnz_lower_ * sizeof(IndexT);               // colind
      size += nnz_lower_ * sizeof(ValueT);               // values
      size += nnz_diag_ * sizeof(ValueT);                // diagonal

      if (local_vectors_indexing_) {
        size += 2 * nthreads_ * sizeof(IndexT);     // map start/end
        size += cnfl_map_->length * sizeof(short);  // map cpu
        size += cnfl_map_->length * sizeof(IndexT); // map pos
      } else if (conflict_free_apriori_) {
        size += (ncolors_ + 1) * sizeof(IndexT); // col_ptr
        size += nrows_ * sizeof(IndexT);         // rowind_sym
      } else if (conflict_free_aposteriori_) {
        size += (ncolors_ + 1) * sizeof(IndexT); // range_ptr
        size += 2 * nranges_ * sizeof(IndexT);   // range_start/end
      }

      if (hybrid_) {
        size += (nrows_left_ + 1) * sizeof(IndexT); // rowptr_high
        size += nnz_h_ * sizeof(IndexT);            // colind_high
        size += nnz_h_ * sizeof(ValueT);            // values_high
      }

      return size;
    }

    size += (nrows_ + 1) * sizeof(IndexT); // rowptr
    size += nnz_ * sizeof(IndexT);         // colind
    size += nnz_ * sizeof(ValueT);         // values
    if (split_nnz_)
      size += (nthreads_ + 1) * sizeof(IndexT); // row_split

    return size;
  }

  virtual inline Platform platform() const override { return platform_; }

  virtual bool tune(Kernel k, Tuning t) override {
    using placeholders::_1;
    using placeholders::_2;

    if (t == Tuning::None) {
      spmv_fn = bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_vanilla, this, _1, _2);
      return false;
    }

    if (symmetric_) {
#ifdef _LOG_INFO
      cout << "[INFO]: converting CSR format to SSS format..." << endl;
#endif
      compress_symmetry();
      if (nthreads_ == 1) {
        spmv_fn =
            bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_serial, this, _1, _2);
      } else {
        if (atomics_)
          spmv_fn = bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_atomics, this,
                         _1, _2);
        else if (effective_ranges_)
          spmv_fn =
              bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_effective_ranges,
                   this, _1, _2);
        else if (local_vectors_indexing_)
          spmv_fn = bind(
              &CSRMatrix<IndexT, ValueT>::cpu_mv_sym_local_vectors_indexing,
              this, _1, _2);
        else if (conflict_free_apriori_)
          spmv_fn =
              bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free_apriori,
                   this, _1, _2);
        else if (conflict_free_aposteriori_ && hybrid_)
          spmv_fn =
              bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free_hyb,
                   this, _1, _2);
        else if (conflict_free_aposteriori_ && !hybrid_)
          spmv_fn = bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free,
                         this, _1, _2);
        else
          assert(false);
      }
    } else {
      spmv_fn =
          bind(&CSRMatrix<IndexT, ValueT>::cpu_mv_split_nnz, this, _1, _2);
    }

    return true;
  }

  virtual void dense_vector_multiply(ValueT *__restrict y,
                                     const ValueT *__restrict x) override {
    spmv_fn(y, x);
  }

  // Export CSR internal representation
  IndexT *rowptr() const { return rowptr_; }
  IndexT *colind() const { return colind_; }
  ValueT *values() const { return values_; }

private:
  Platform platform_;
  int nrows_, ncols_, nnz_, nnz_h_;
  bool symmetric_, hybrid_, owns_data_;
  IndexT *rowptr_;
  IndexT *colind_;
  ValueT *values_;
  // Hybrid
  IndexT *rowptr_h_;
  IndexT *colind_h_;
  ValueT *values_h_;
  // Internal function pointers
  function<void(ValueT *__restrict, const ValueT *__restrict)> spmv_fn;
  // Partitioning
  bool split_nnz_;
  int nthreads_;
  IndexT *row_split_;
  // Symmetry compression
  bool cmp_symmetry_;
  bool atomics_, effective_ranges_, local_vectors_indexing_,
      conflict_free_apriori_, conflict_free_aposteriori_;
  int nnz_lower_, nnz_diag_, nrows_left_, nconflicts_, ncolors_, nranges_;
  IndexT *rowptr_sym_, *rowind_sym_;
  IndexT *colind_sym_;
  ValueT *values_sym_;
  ValueT *diagonal_;
  IndexT *color_ptr_;
  ConflictMap *cnfl_map_;
  ValueT **y_local_;
  SymmetryCompressionData<IndexT, ValueT> **sym_cmp_data_;
  const int BLK_FACTOR = 1;
  const int BLK_BITS = 0;

  /*
  * Preprocessing routines
  */
  void reorder();
  void split_by_bandwidth();
  void split_by_nrows(int nthreads);
  void split_by_nnz(int nthreads);

  /*
   * Symmetry compression
   */
  void compress_symmetry();
  // Method 1: local vector with effective ranges
  void atomics();
  // Method 2: local vector with effective ranges
  void effective_ranges();
  // Method 3: local vectors indexing
  void local_vectors_indexing();
  void count_conflicting_rows();
  // Method 4: conflict-free a priori
  void conflict_free_apriori();
  void count_apriori_conflicts();
  // Method 5: conflict-free a posteriori
  void conflict_free_aposteriori();
  void count_aposteriori_conflicts();
  // Common utilities for methods 4 & 5
  // Graph coloring
  void color(const ColoringGraph &g, vector<int> &color);
  // Graph coloring + load balancing
  void color(const ColoringGraph &g, const vector<WeightedVertex> &v,
             vector<int> &color);
  // Parallel graph coloring
  void parallel_color(const ColoringGraph &g,
                      tbb::concurrent_vector<int> &color);
  // Parallel graph coloring + load balancing
  void parallel_color(const ColoringGraph &g, const vector<WeightedVertex> &v,
                      tbb::concurrent_vector<int> &color);
  void ordering_heuristic(const ColoringGraph &g, vector<int> &order);
  void first_fit_round_robin(const ColoringGraph &g, vector<int> &order);
  void shortest_row(const ColoringGraph &g, vector<int> &order);
  void shortest_row_round_robin(const ColoringGraph &g, vector<int> &order);
  void longest_row(const ColoringGraph &g, vector<int> &order);
  void longest_row_round_robin(const ColoringGraph &g, vector<int> &order);

  /*
   * Sparse Matrix - Dense Vector Multiplication kernels
   */
  void cpu_mv_vanilla(ValueT *__restrict y, const ValueT *__restrict x);
  void cpu_mv_split_nnz(ValueT *__restrict y, const ValueT *__restrict x);

  // Symmetric kernels
  void cpu_mv_sym_serial(ValueT *__restrict y, const ValueT *__restrict x);
  void cpu_mv_sym_atomics(ValueT *__restrict y, const ValueT *__restrict x);
  void cpu_mv_sym_effective_ranges(ValueT *__restrict y,
                                   const ValueT *__restrict x);
  void cpu_mv_sym_local_vectors_indexing(ValueT *__restrict y,
                                         const ValueT *__restrict x);
  void cpu_mv_sym_conflict_free_apriori(ValueT *__restrict y,
                                        const ValueT *__restrict x);
  void cpu_mv_sym_conflict_free(ValueT *__restrict y,
                                const ValueT *__restrict x);
  void cpu_mv_sym_conflict_free_hyb(ValueT *__restrict y,
                                    const ValueT *__restrict x);
};

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::reorder() {
#ifdef _LOG_INFO
  cout << "[INFO]: reordering matrix using RCM..." << endl;
#endif

  // Construct graph
  typedef boost::adjacency_list<
      boost::vecS, boost::vecS, boost::undirectedS,
      boost::property<boost::vertex_color_t, boost::default_color_type,
                      boost::property<boost::vertex_degree_t, int>>>
      ReorderingGraph;
  ReorderingGraph g(nrows_);
  for (int i = 0; i < nrows_; ++i) {
    for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
      IndexT col = colind_[j];
      if (col != i) {
        add_edge(i, col, g);
      }
    }
  }

#ifdef _LOG_INFO
  size_t ob = bandwidth(g);
  cout << "[INFO]: original bandwidth = " << ob << endl;
#endif

  // Reverse Cuthill Mckee Ordering
  using namespace boost;
  typedef graph_traits<ReorderingGraph>::vertex_descriptor Vertex;
  // typedef graph_traits<ReorderingGraph>::vertices_size_type size_type;
  vector<Vertex> inv_perm(num_vertices(g));
  cuthill_mckee_ordering(g, inv_perm.rbegin(), get(vertex_color, g),
                         make_degree_map(g));

  // Find permutation of original to new ordering
  property_map<ReorderingGraph, vertex_index_t>::type idx_map =
      get(vertex_index, g);
  IndexT *perm_;
  // IndexT *inv_perm_;
  perm_ = (IndexT *)internal_alloc(nrows_ * sizeof(IndexT), platform_);
  // inv_perm_ = (IndexT *)internal_alloc(nrows_ * sizeof(IndexT), platform_);
  for (size_t i = 0; i != inv_perm.size(); ++i) {
    perm_[idx_map[inv_perm[i]]] = i;
    // inv_perm_[i] = inv_perm[i];
  }

#ifdef _LOG_INFO
  size_t fb =
      bandwidth(g, make_iterator_property_map(&perm_[0], idx_map, perm_[0]));
  cout << "[INFO]: final bandwidth = " << fb << endl;
#endif

  // Reorder original matrix
  // First reorder rows
  vector<IndexT> row_nnz(nrows_);
  for (int i = 0; i < nrows_; ++i) {
    row_nnz[perm_[i]] = rowptr_[i + 1] - rowptr_[i];
  }

  IndexT *new_rowptr =
      (IndexT *)internal_alloc((nrows_ + 1) * sizeof(IndexT), platform_);
  // Enforce first touch policy
  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int i = 1; i <= nrows_; ++i) {
    new_rowptr[i] = row_nnz[i - 1];
  }

  for (int i = 1; i <= nrows_; ++i) {
    new_rowptr[i] += new_rowptr[i - 1];
  }
  assert(new_rowptr[nrows_] == nnz_);

  // Then reorder nonzeros per row
  map<IndexT, ValueT> sorted_row;
  IndexT *new_colind =
      (IndexT *)internal_alloc(nnz_ * sizeof(IndexT), platform_);
  ValueT *new_values =
      (ValueT *)internal_alloc(nnz_ * sizeof(ValueT), platform_);

  // Enforce first touch policy
  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int i = 0; i < nnz_; i++) {
    new_colind[i] = 0;
    new_values[i] = 0.0;
  }

  for (int i = 0; i < nrows_; ++i) {
    for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
      sorted_row.insert(make_pair(perm_[colind_[j]], values_[j]));
    }

    // Flush row
    auto it = sorted_row.begin();
    for (int j = new_rowptr[perm_[i]]; j < new_rowptr[perm_[i] + 1]; ++j) {
      new_colind[j] = it->first;
      new_values[j] = it->second;
      ++it;
    }

    sorted_row.clear();
  }

  internal_free(perm_, platform_);
  // internal_free(inv_perm_, platform_);
  if (owns_data_) {
    internal_free(rowptr_, platform_);
    internal_free(colind_, platform_);
    internal_free(values_, platform_);
  }
  rowptr_ = new_rowptr;
  colind_ = new_colind;
  values_ = new_values;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::split_by_bandwidth() {
#ifdef _LOG_INFO
  cout << "[INFO]: clustering matrix into low and high bandwidth nonzeros"
       << endl;
#endif

  vector<IndexT> rowptr_low(nrows_ + 1, 0);
  vector<IndexT> rowptr_high(nrows_ + 1, 0);
  vector<IndexT> colind_low, colind_high;
  vector<ValueT> values_low, values_high;
  const int THRESHOLD = 4000;

  rowptr_low[0] = 0;
  rowptr_high[0] = 0;
  for (int i = 0; i < nrows_; ++i) {
    for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
      if (abs(colind_[j] - i) < THRESHOLD) {
        rowptr_low[i + 1]++;
        colind_low.push_back(colind_[j]);
        values_low.push_back(values_[j]);
      } else {
        rowptr_high[i + 1]++;
        colind_high.push_back(colind_[j]);
        values_high.push_back(values_[j]);
      }
    }
  }

  for (int i = 1; i < nrows_ + 1; ++i) {
    rowptr_low[i] += rowptr_low[i - 1];
    rowptr_high[i] += rowptr_high[i - 1];
  }
  assert(rowptr_low[nrows_] == static_cast<int>(values_low.size()));
  assert(rowptr_high[nrows_] == static_cast<int>(values_high.size()));

  nnz_ = values_low.size();
  move(rowptr_low.begin(), rowptr_low.end(), rowptr_);
  move(colind_low.begin(), colind_low.end(), colind_);
  move(values_low.begin(), values_low.end(), values_);

  // #pragma omp parallel num_threads(nthreads_)
  // {
  //   #pragma omp for schedule(static)
  //   for (int i = 0; i <= nrows_; i++) {
  //     rowptr_[i] = rowptr_low[i];
  //   }

  //   #pragma omp for schedule(static)
  //   for (int i = 0; i < nnz_; i++) {
  //     colind_[i] = colind_low[i];
  //     values_[i] = values_low[i];
  //   }
  // }

  nnz_h_ = values_high.size();
  rowptr_h_ =
      (IndexT *)internal_alloc((nrows_ + 1) * sizeof(IndexT), platform_);
  colind_h_ = (IndexT *)internal_alloc(nnz_h_ * sizeof(IndexT), platform_);
  values_h_ = (ValueT *)internal_alloc(nnz_h_ * sizeof(ValueT), platform_);

  move(rowptr_high.begin(), rowptr_high.end(), rowptr_h_);
  move(colind_high.begin(), colind_high.end(), colind_h_);
  move(values_high.begin(), values_high.end(), values_h_);

  // #pragma omp parallel num_threads(nthreads_)
  // {
  //   #pragma omp for schedule(static)
  //   //for (int i = 0; i <= nrows_left_; i++) {
  //   for (int i = 0; i <= nrows_; i++) {
  //     //      rowptr_h_[i] = 0;
  //     rowptr_h_[i] = rowptr_high[i];
  //   }

  //   #pragma omp for schedule(static)
  //   for (int i = 0; i < nnz_h_; i++) {
  //     colind_h_[i] = colind_high[i];
  //     values_h_[i] = values_high[i];
  //   }
  // }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::split_by_nrows(int nthreads) {
#ifdef _LOG_INFO
  cout << "[INFO]: splitting matrix into " << nthreads << " partitions by rows"
       << endl;
#endif

  if (!row_split_) {
    row_split_ =
        (IndexT *)internal_alloc((nthreads + 1) * sizeof(IndexT), platform_);
  }

  // Re-init
  memset(row_split_, 0, (nthreads + 1) * sizeof(IndexT));

  // Compute new matrix splits
  int nrows_per_split = nrows_ / nthreads;
  row_split_[0] = 0;
  for (int i = 0; i < nthreads - 1; i++) {
    row_split_[i + 1] += nrows_per_split;
  }

  row_split_[nthreads] = nrows_;
  // split_nrows_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::split_by_nnz(int nthreads) {
#ifdef _LOG_INFO
  if (symmetric_)
    cout << "[INFO]: splitting lower triangular part of matrix into "
         << nthreads << " partitions" << endl;
  else
    cout << "[INFO]: splitting full matrix into " << nthreads << " partitions"
         << endl;
#endif

  if (!row_split_) {
    row_split_ =
        (IndexT *)internal_alloc((nthreads + 1) * sizeof(IndexT), platform_);
  }

  if (nthreads_ == 1) {
    row_split_[0] = 0;
    row_split_[1] = nrows_;
    split_nnz_ = true;
    return;
  }

  // Compute the matrix splits.
  int nnz_cnt = (symmetric_) ? ((nnz_ - nrows_) / 2) : nnz_;
  int nnz_per_split = nnz_cnt / nthreads_;
  int curr_nnz = 0;
  int row_start = 0;
  int split_cnt = 0;
  int i;

  row_split_[0] = row_start;
  if (hybrid_) {
    nnz_cnt = (nnz_ - nrows_) / 2 + nnz_h_;
    nnz_per_split = nnz_cnt / nthreads_;
    for (i = 0; i < nrows_; i++) {
      int row_nnz = 0;
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          row_nnz++;
        }
      }
      row_nnz += rowptr_h_[i + 1] - rowptr_h_[i];
      curr_nnz += row_nnz;

      if ((curr_nnz >= nnz_per_split) && ((i + 1) % BLK_FACTOR == 0)) {
        row_start = i + 1;
        ++split_cnt;
        if (split_cnt <= nthreads)
          row_split_[split_cnt] = row_start;
        curr_nnz = 0;
      }
    }
  } else if (symmetric_) {
    for (i = 0; i < nrows_; i++) {
      int row_nnz = 0;
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          row_nnz++;
        }
      }
      curr_nnz += row_nnz;

      if ((curr_nnz >= nnz_per_split) && ((i + 1) % BLK_FACTOR == 0)) {
        row_start = i + 1;
        ++split_cnt;
        if (split_cnt <= nthreads)
          row_split_[split_cnt] = row_start;
        curr_nnz = 0;
      }
    }
  } else {
    for (i = 0; i < nrows_; i++) {
      curr_nnz += rowptr_[i + 1] - rowptr_[i];
      if ((curr_nnz >= nnz_per_split) && ((i + 1) % BLK_FACTOR == 0)) {
        row_start = i + 1;
        ++split_cnt;
        if (split_cnt <= nthreads)
          row_split_[split_cnt] = row_start;
        curr_nnz = 0;
      }
    }
  }

  // Fill the last split with remaining elements
  if (curr_nnz < nnz_per_split && split_cnt <= nthreads) {
    row_split_[++split_cnt] = nrows_;
  }

  // If there are any remaining rows merge them in last partition
  if (split_cnt > nthreads_) {
    row_split_[nthreads_] = nrows_;
  }

  // If there are remaining threads create empty partitions
  for (int i = split_cnt + 1; i <= nthreads; i++) {
    row_split_[i] = nrows_;
  }

  split_nnz_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::atomics() {
  // Sanity check
  assert(symmetric_);

#ifdef _LOG_INFO
  cout << "[INFO]: compressing for symmetry using atomics" << endl;
#endif

  diagonal_ = (ValueT *)internal_alloc(nrows_ * sizeof(ValueT));
  memset(diagonal_, 0, nrows_ * sizeof(IndexT));
  sym_cmp_data_ = (SymmetryCompressionData<IndexT, ValueT> **)internal_alloc(
      nthreads_ * sizeof(SymmetryCompressionData<IndexT, ValueT> *));
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    sym_cmp_data_[tid] = new SymmetryCompressionData<IndexT, ValueT>;
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    int nrows = row_split_[tid + 1] - row_split_[tid];
    int row_offset = row_split_[tid];
    data->nrows_ = nrows;
    data->rowptr_ = (IndexT *)internal_alloc((nrows + 1) * sizeof(IndexT));
    memset(data->rowptr_, 0, (nrows + 1) * sizeof(IndexT));
    data->diagonal_ = (ValueT *)internal_alloc(nrows * sizeof(ValueT));

    vector<IndexT> colind_sym;
    vector<ValueT> values_sym;
    int nnz_diag = 0;
    int nnz_estimated =
        (rowptr_[row_split_[tid + 1]] - rowptr_[row_split_[tid]]) / 2;
    colind_sym.reserve(nnz_estimated);
    values_sym.reserve(nnz_estimated);

    data->rowptr_[0] = 0;
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          data->rowptr_[i + 1 - row_offset]++; // FIXME check
          colind_sym.push_back(colind_[j]);
          values_sym.push_back(values_[j]);
        } else if (colind_[j] == i) {
          diagonal_[i] = values_[j];
          data->diagonal_[i - row_offset] = values_[j];
          nnz_diag++;
        }
      }
    }

    for (int i = 1; i <= nrows; ++i) {
      data->rowptr_[i] += data->rowptr_[i - 1];
    }

    assert(data->rowptr_[nrows] == static_cast<int>(values_sym.size()));
    data->nnz_lower_ = values_sym.size();
    data->nnz_diag_ = nnz_diag;
    #pragma omp critical
    {
      nnz_lower_ += data->nnz_lower_;
      nnz_diag_ += data->nnz_diag_;
    }

    data->colind_ =
        (IndexT *)internal_alloc(data->nnz_lower_ * sizeof(IndexT), platform_);
    data->values_ =
        (ValueT *)internal_alloc(data->nnz_lower_ * sizeof(ValueT), platform_);

    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i - row_offset + 1]; ++j) {
        data->colind_[j] = colind_sym[j];
        data->values_[j] = values_sym[j];
      }
    }

    // Cleanup
    colind_sym.clear();
    values_sym.clear();
    colind_sym.shrink_to_fit();
    values_sym.shrink_to_fit();
  }

  cmp_symmetry_ = true;
  atomics_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::effective_ranges() {
  // Sanity check
  assert(symmetric_);

#ifdef _LOG_INFO
  cout << "[INFO]: compressing for symmetry using effective ranges of local "
          "vectors"
       << endl;
#endif

  diagonal_ = (ValueT *)internal_alloc(nrows_ * sizeof(ValueT));
  memset(diagonal_, 0, nrows_ * sizeof(IndexT));
  sym_cmp_data_ = (SymmetryCompressionData<IndexT, ValueT> **)internal_alloc(
      nthreads_ * sizeof(SymmetryCompressionData<IndexT, ValueT> *));
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    sym_cmp_data_[tid] = new SymmetryCompressionData<IndexT, ValueT>;
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    int nrows = row_split_[tid + 1] - row_split_[tid];
    int row_offset = row_split_[tid];
    data->nrows_ = nrows;
    data->rowptr_ = (IndexT *)internal_alloc((nrows + 1) * sizeof(IndexT));
    memset(data->rowptr_, 0, (nrows + 1) * sizeof(IndexT));
    data->diagonal_ = (ValueT *)internal_alloc(nrows * sizeof(ValueT));

    vector<IndexT> colind_sym;
    vector<ValueT> values_sym;
    int nnz_diag = 0;
    int nnz_estimated =
        (rowptr_[row_split_[tid + 1]] - rowptr_[row_split_[tid]]) / 2;
    colind_sym.reserve(nnz_estimated);
    values_sym.reserve(nnz_estimated);

    data->rowptr_[0] = 0;
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          data->rowptr_[i + 1 - row_offset]++; // FIXME check
          colind_sym.push_back(colind_[j]);
          values_sym.push_back(values_[j]);
        } else if (colind_[j] == i) {
          diagonal_[i] = values_[j];
          data->diagonal_[i - row_offset] = values_[j];
          nnz_diag++;
        }
      }
    }

    for (int i = 1; i <= nrows; ++i) {
      data->rowptr_[i] += data->rowptr_[i - 1];
    }

    assert(data->rowptr_[nrows] == static_cast<int>(values_sym.size()));
    data->nnz_lower_ = values_sym.size();
    data->nnz_diag_ = nnz_diag;
    #pragma omp critical
    {
      nnz_lower_ += data->nnz_lower_;
      nnz_diag_ += data->nnz_diag_;
    }

    data->colind_ =
        (IndexT *)internal_alloc(data->nnz_lower_ * sizeof(IndexT), platform_);
    data->values_ =
        (ValueT *)internal_alloc(data->nnz_lower_ * sizeof(ValueT), platform_);

    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i - row_offset + 1]; ++j) {
        data->colind_[j] = colind_sym[j];
        data->values_[j] = values_sym[j];
      }
    }

    if (tid > 0) {
      data->local_vector_ =
          (ValueT *)internal_alloc(row_split_[tid] * sizeof(ValueT), platform_);
      memset(data->local_vector_, 0, row_split_[tid] * sizeof(ValueT));
    }

    // Cleanup
    colind_sym.clear();
    values_sym.clear();
    colind_sym.shrink_to_fit();
    values_sym.shrink_to_fit();
  }

  cmp_symmetry_ = true;
  effective_ranges_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::count_conflicting_rows() {
  // Sanity check
  assert(cmp_symmetry_);

  int cnfl_total = 0;
  set<IndexT> cnfl[nthreads_ - 1];
  for (int tid = 1; tid < nthreads_; ++tid) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT row_offset = row_split_[tid];
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i - row_offset + 1]; ++j) {
        if (data->colind_[j] < row_split_[tid])
          cnfl[tid - 1].insert(data->colind_[j]);
      }
    }

    cnfl_total += cnfl[tid - 1].size();
  }

  double cnfl_mean = (double)cnfl_total / (nthreads_ - 1);
  cout << "[INFO]: detected " << cnfl_mean << " mean direct conflicts" << endl;
  cout << "[INFO]: detected " << cnfl_total << " total direct conflicts"
       << endl;

  for (int tid = 1; tid < nthreads_; ++tid)
    cnfl[tid - 1].clear();
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::local_vectors_indexing() {
  // Sanity check
  assert(symmetric_);

#ifdef _LOG_INFO
  cout << "[INFO]: compressing for symmetry using local vectors indexing"
       << endl;
#endif

  diagonal_ = (ValueT *)internal_alloc(nrows_ * sizeof(ValueT));
  memset(diagonal_, 0, nrows_ * sizeof(IndexT));
  y_local_ = (ValueT **)internal_alloc(nthreads_ * sizeof(ValueT *), platform_);
  sym_cmp_data_ = (SymmetryCompressionData<IndexT, ValueT> **)internal_alloc(
      nthreads_ * sizeof(SymmetryCompressionData<IndexT, ValueT> *));
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    sym_cmp_data_[tid] = new SymmetryCompressionData<IndexT, ValueT>;
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    int nrows = row_split_[tid + 1] - row_split_[tid];
    int row_offset = row_split_[tid];
    data->nrows_ = nrows;
    data->rowptr_ = (IndexT *)internal_alloc((nrows + 1) * sizeof(IndexT));
    memset(data->rowptr_, 0, (nrows + 1) * sizeof(IndexT));
    data->diagonal_ = (ValueT *)internal_alloc(nrows * sizeof(ValueT));

    vector<IndexT> colind_sym;
    vector<ValueT> values_sym;
    int nnz_diag = 0;
    int nnz_estimated =
        (rowptr_[row_split_[tid + 1]] - rowptr_[row_split_[tid]]) / 2;
    colind_sym.reserve(nnz_estimated);
    values_sym.reserve(nnz_estimated);

    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          data->rowptr_[i + 1 - row_offset]++; // FIXME check
          colind_sym.push_back(colind_[j]);
          values_sym.push_back(values_[j]);
        } else if (colind_[j] == i) {
          diagonal_[i] = values_[j];
          data->diagonal_[i - row_offset] = values_[j];
          nnz_diag++;
        }
      }
    }

    for (int i = 1; i <= nrows; ++i) {
      data->rowptr_[i] += data->rowptr_[i - 1];
    }

    assert(data->rowptr_[nrows] == static_cast<int>(values_sym.size()));
    data->nnz_lower_ = values_sym.size();
    data->nnz_diag_ = nnz_diag;
    #pragma omp critical
    {
      nnz_lower_ += data->nnz_lower_;
      nnz_diag_ += data->nnz_diag_;
    }

    data->colind_ =
        (IndexT *)internal_alloc(data->nnz_lower_ * sizeof(IndexT), platform_);
    data->values_ =
        (ValueT *)internal_alloc(data->nnz_lower_ * sizeof(ValueT), platform_);

    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i - row_offset + 1]; ++j) {
        data->colind_[j] = colind_sym[j];
        data->values_[j] = values_sym[j];
      }
    }

    if (tid > 0) {
      data->local_vector_ =
          (ValueT *)internal_alloc(row_split_[tid] * sizeof(ValueT), platform_);
      memset(data->local_vector_, 0, row_split_[tid] * sizeof(ValueT));
      y_local_[tid] = data->local_vector_;
    } else {
      y_local_[tid] = nullptr;
    }

    // Cleanup
    colind_sym.clear();
    values_sym.clear();
    colind_sym.shrink_to_fit();
    values_sym.shrink_to_fit();
  }

  // for (int tid = 0; tid < nthreads_; ++tid) {
  //   SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
  //   nnz_lower_ += data->nnz_lower_;
  //   nnz_diag_ += data->nnz_diag_;
  // }

  cmp_symmetry_ = true;

  if (nthreads_ == 1)
    return;

  // Calculate number of conflicting rows per thread and total
  // count_conflicting_rows();

  // Global map of conflicts
  map<IndexT, set<int>> global_map;
  // Conflicting rows per thread
  set<IndexT> thread_map;
  int ncnfls = 0;
  for (int tid = 1; tid < nthreads_; tid++) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT row_offset = row_split_[tid];
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i + 1 - row_offset]; ++j) {
        IndexT col = data->colind_[j];
        if (col < row_split_[tid]) {
          thread_map.insert(col);
          global_map[col].insert(tid);
        }
      }
    }
    ncnfls += thread_map.size();
    thread_map.clear();
  }

  // Allocate auxiliary map
  cnfl_map_ = new ConflictMap;
  cnfl_map_->length = ncnfls;
  cnfl_map_->cpu = (short *)internal_alloc(ncnfls * sizeof(short), platform_);
  cnfl_map_->pos = (IndexT *)internal_alloc(ncnfls * sizeof(IndexT), platform_);
  int cnt = 0;
  for (auto &elem : global_map) {
    for (auto &cpu : elem.second) {
      cnfl_map_->pos[cnt] = elem.first;
      cnfl_map_->cpu[cnt] = cpu;
      cnt++;
    }
  }
  assert(cnt == ncnfls);

  // Split reduction work among threads so that conflicts to the same row are
  // assigned to the same thread
  int total_count = ncnfls;
  int tid = 0;
  int limit = total_count / nthreads_;
  int tmp_count = 0;
  for (auto &elem : global_map) {
    if (tmp_count <= limit) {
      tmp_count += elem.second.size();
    } else {
      SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
      data->map_end_ = tmp_count;
      // If we have exceeded the number of threads, assigned what is left to
      // last thread
      if (tid == nthreads_ - 1) {
        data->map_end_ = ncnfls;
        break;
      } else {
        total_count -= tmp_count;
        tmp_count = 0;
        limit = total_count / (nthreads_ - tid + 1);
      }
      tid++;
    }
  }

  int start = 0;
  for (int tid = 0; tid < nthreads_; tid++) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    data->map_start_ = start;
    if (tid < nthreads_ - 1)
      data->map_end_ += start;
    start = data->map_end_;
    if (tid == nthreads_ - 1)
      assert(data->map_end_ = ncnfls);
  }

  local_vectors_indexing_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::count_apriori_conflicts() {
  // Sanity check
  assert(cmp_symmetry_);

  set<pair<IndexT, IndexT>> cnfl;
  map<IndexT, set<IndexT>> indirect_cnfl;
  for (int i = 0; i < nrows_; ++i) {
    for (int j = rowptr_sym_[i]; j < rowptr_sym_[i + 1]; ++j) {
      cnfl.insert(make_pair(i, colind_sym_[j]));
      indirect_cnfl[colind_sym_[j]].insert(i);
    }
  }

  int no_direct_cnfl = cnfl.size();
  int no_indirect_cnfl = 0;
  // Add indirect conflicts
  for (auto &col : indirect_cnfl) {
    // N * (N-1) / 2
    no_indirect_cnfl += col.second.size() * (col.second.size() - 1) / 2;
    // Create conflicts for every pair of rows in this set
    for (auto &row1 : col.second) {
      for (auto &row2 : col.second) {
        if (row1 != row2) {
          pair<IndexT, IndexT> i_j = make_pair(row1, row2);
          pair<IndexT, IndexT> j_i = make_pair(row2, row1);
          // If these rows are not already connected
          if (cnfl.count(i_j) == 0 && cnfl.count(j_i) == 0)
            cnfl.insert(i_j);
        }
      }
    }
  }

  cout << "[INFO]: detected " << no_direct_cnfl << " direct conflicts" << endl;
  cout << "[INFO]: detected " << no_indirect_cnfl << " indirect conflicts"
       << endl;
  // The number of edges in the graph will be the union of direct and indirect
  // conflicts
  cout << "[INFO]: the a priori conflict graph will contain " << cnfl.size()
       << " edges" << endl;

  cnfl.clear();
  indirect_cnfl.clear();
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::conflict_free_apriori() {
  // Sanity check
  assert(symmetric_);

#ifdef _LOG_INFO
  cout << "[INFO]: compressing for symmetry using a priori conflict-free SpMV"
       << endl;
#endif

  // FIXME first touch
  rowptr_sym_ = (IndexT *)internal_alloc((nrows_ + 1) * sizeof(IndexT));
  memset(rowptr_sym_, 0, (nrows_ + 1) * sizeof(IndexT));
  diagonal_ = (ValueT *)internal_alloc(nrows_ * sizeof(ValueT));
  memset(diagonal_, 0, nrows_ * sizeof(ValueT));

  vector<IndexT> colind_sym;
  vector<ValueT> values_sym;
  int nnz_estimated = nnz_ / 2;
  colind_sym.reserve(nnz_estimated);
  values_sym.reserve(nnz_estimated);

  nnz_diag_ = 0;
  rowptr_sym_[0] = 0;
  for (int tid = 0; tid < nthreads_; ++tid) {
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        if (colind_[j] < i) {
          rowptr_sym_[i + 1]++;
          colind_sym.push_back(colind_[j]);
          values_sym.push_back(values_[j]);
        } else if (colind_[j] == i) {
          diagonal_[i] = values_[j];
          nnz_diag_++;
        }
      }
    }
  }

  for (int i = 1; i <= nrows_; ++i) {
    rowptr_sym_[i] += rowptr_sym_[i - 1];
  }

  assert(rowptr_sym_[nrows_] == static_cast<int>(values_sym.size()));
  nnz_lower_ = values_sym.size();
  colind_sym_ =
      (IndexT *)internal_alloc(nnz_lower_ * sizeof(IndexT), platform_);
  values_sym_ =
      (ValueT *)internal_alloc(nnz_lower_ * sizeof(ValueT), platform_);

  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int j = 0; j < nnz_lower_; ++j) {
    colind_sym_[j] = colind_sym[j];
    values_sym_[j] = values_sym[j];
  }

  // Cleanup
  colind_sym.clear();
  values_sym.clear();
  colind_sym.shrink_to_fit();
  values_sym.shrink_to_fit();

  cmp_symmetry_ = true;

  if (nthreads_ == 1)
    return;

// Find number of conflicts
// count_apriori_conflicts();

#ifdef _LOG_INFO
  float assembly_start = omp_get_wtime();
#endif
  const int blk_rows = (int)ceil(nrows_ / (double)BLK_FACTOR);
  ColoringGraph g(blk_rows);
  vector<WeightedVertex> vertices(blk_rows);
  tbb::concurrent_vector<tbb::concurrent_vector<int>> indirect(blk_rows);

  // Add direct conflicts
  for (int i = 0; i < nrows_; ++i) {
    IndexT blk_row = i >> BLK_BITS;
    IndexT prev_blk_col = -1;
    for (int j = rowptr_sym_[i]; j < rowptr_sym_[i + 1]; ++j) {
      IndexT blk_col = colind_sym_[j] >> BLK_BITS;
      g[blk_row].push_back(blk_col);
      g[blk_col].push_back(blk_row);
      // Mark potential indirect conflicts
      if (blk_col != prev_blk_col)
        indirect[blk_col].push_back(blk_row);
      prev_blk_col = blk_col;
    }
  }

  // Add indirect conflicts
  // Indirect conflicts occur when two rows have nonzero elements in the same
  // column.
  for (int i = 0; i < blk_rows; i++) {
    for (const auto &row1 : indirect[i]) {
      for (const auto &row2 : indirect[i]) {
        if (row1 < row2) {
          g[row1].push_back(row2);
          g[row2].push_back(row1);
        }
      }
    }
  }

  for (auto &i : indirect)
    i.clear();
  indirect.clear();

#ifdef _LOG_INFO
  float assembly_stop = omp_get_wtime();
  cout << "[INFO]: graph assembly: " << assembly_stop - assembly_start << endl;
  cout << "[INFO]: using a blocking factor of: " << BLK_FACTOR << endl;
#endif

  const int V = g.size();
  vector<int> color_map(V, V - 1);
  color(g, color_map);

  // Find row indices per color
  vector<vector<IndexT>> rowind(ncolors_);
  for (int i = 0; i < nrows_; i++) {
    rowind[color_map[i >> BLK_BITS]].push_back(i);
  }

  // Allocate auxiliary arrays
  color_ptr_ = (IndexT *)internal_alloc((ncolors_ + 1) * sizeof(IndexT));
  memset(color_ptr_, 0, (ncolors_ + 1) * sizeof(IndexT));
  for (int c = 1; c <= ncolors_; ++c) {
    color_ptr_[c] += color_ptr_[c - 1] + rowind[c - 1].size();
  }
  assert(color_ptr_[ncolors_] == nrows_);

  rowind_sym_ = (IndexT *)internal_alloc(nrows_ * sizeof(IndexT));
  int cnt = 0;
  for (int c = 0; c < ncolors_; ++c) {
    sort(rowind[c].begin(), rowind[c].end());
    for (size_t i = 0; i < rowind[c].size(); ++i) {
      rowind_sym_[cnt++] = rowind[c][i];
    }
  }

  conflict_free_apriori_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::count_aposteriori_conflicts() {
  // Sanity check
  assert(cmp_symmetry_);

  set<pair<IndexT, IndexT>> cnfl;
  map<IndexT, vector<pair<IndexT, IndexT>>> indirect_cnfl;
  for (int tid = 0; tid < nthreads_; ++tid) {
    // set<pair<IndexT, IndexT>> cnfl;
    // map<IndexT, vector<pair<IndexT, IndexT>>> indirect_cnfl;
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT row_offset = row_split_[tid];
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i - row_offset + 1]; ++j) {
        if (data->colind_[j] < row_split_[tid])
          cnfl.insert(make_pair(i, data->colind_[j]));

        indirect_cnfl[data->colind_[j]].emplace_back(make_pair(i, tid));
      }
    }
  }

  int no_direct_cnfl = cnfl.size();
  int no_indirect_cnfl = 0;

  for (auto &col : indirect_cnfl) {
    for (auto &row1 : col.second) {
      for (auto &row2 : col.second) {
        if (row1.first != row2.first && row1.second != row2.second) {
          pair<IndexT, IndexT> i_j = make_pair(row1.first, row2.first);
          cnfl.insert(i_j);
          no_indirect_cnfl++;
        }
      }
    }
  }

  cout << "[INFO]: detected " << no_direct_cnfl << " direct conflicts" << endl;
  cout << "[INFO]: detected " << no_indirect_cnfl << " indirect conflicts"
       << endl;
  // The number of edges in the graph will be the union of direct and indirect
  // conflicts
  cout << "[INFO]: the a posteriori conflict graph will contain " << cnfl.size()
       << " edges" << endl;

  cnfl.clear();
  indirect_cnfl.clear();
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::conflict_free_aposteriori() {
  // Sanity check
  assert(symmetric_);

#ifdef _LOG_INFO
  cout << "[INFO]: compressing for symmetry using a posteriori conflict-free "
          "SpMV"
       << endl;
#endif

#ifdef _LOG_INFO
  double tstart, tstop;
  tstart = omp_get_wtime();
#endif
  sym_cmp_data_ = (SymmetryCompressionData<IndexT, ValueT> **)internal_alloc(
      nthreads_ * sizeof(SymmetryCompressionData<IndexT, ValueT> *));
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    sym_cmp_data_[tid] = new SymmetryCompressionData<IndexT, ValueT>;
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    int nrows = row_split_[tid + 1] - row_split_[tid];
    int row_offset = row_split_[tid];
    data->nrows_ = nrows;
    data->rowptr_ = (IndexT *)internal_alloc((nrows + 1) * sizeof(IndexT));
    memset(data->rowptr_, 0, (nrows + 1) * sizeof(IndexT));
    data->diagonal_ = (ValueT *)internal_alloc(nrows * sizeof(ValueT));
    memset(data->diagonal_, 0, nrows * sizeof(IndexT));

    vector<IndexT> colind_sym;
    vector<ValueT> values_sym;
    int nnz_diag = 0;
    int nnz_estimated =
        (rowptr_[row_split_[tid + 1]] - rowptr_[row_split_[tid]]) / 2;
    colind_sym.reserve(nnz_estimated);
    values_sym.reserve(nnz_estimated);

    data->rowptr_[0] = 0;
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      for (int j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
        IndexT col = colind_[j];
        if (col < i) {
          data->rowptr_[i + 1 - row_offset]++;
          colind_sym.push_back(col);
          values_sym.push_back(values_[j]);
        } else if (col == i) {
          data->diagonal_[i - row_offset] = values_[j];
          nnz_diag++;
        }
      }
    }

    for (int i = 1; i <= nrows; ++i) {
      data->rowptr_[i] += data->rowptr_[i - 1];
    }

    assert(data->rowptr_[nrows] == static_cast<int>(values_sym.size()));
    data->nnz_lower_ = values_sym.size();
    data->nnz_diag_ = nnz_diag;
    #pragma omp critical
    {
      nnz_lower_ += data->nnz_lower_;
      nnz_diag_ += data->nnz_diag_;
    }
    data->colind_ =
        (IndexT *)internal_alloc(data->nnz_lower_ * sizeof(IndexT), platform_);
    data->values_ =
        (ValueT *)internal_alloc(data->nnz_lower_ * sizeof(ValueT), platform_);

    std::move(colind_sym.begin(), colind_sym.end(), data->colind_);
    std::move(values_sym.begin(), values_sym.end(), data->values_);

    // Cleanup
    colind_sym.clear();
    values_sym.clear();
    colind_sym.shrink_to_fit();
    values_sym.shrink_to_fit();

    if (hybrid_) {
      data->rowptr_h_ = (IndexT *)internal_alloc((nrows + 1) * sizeof(IndexT));
      memset(data->rowptr_h_, 0, (nrows + 1) * sizeof(IndexT));
      vector<IndexT> colind_high;
      vector<ValueT> values_high;
      data->rowptr_h_[0] = 0;
      for (int i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
        for (int j = rowptr_h_[i]; j < rowptr_h_[i + 1]; ++j) {
          data->rowptr_h_[i + 1 - row_offset]++;
          colind_high.push_back(colind_h_[j]);
          values_high.push_back(values_h_[j]);
        }
      }
      for (int i = 1; i <= nrows; ++i) {
        data->rowptr_h_[i] += data->rowptr_h_[i - 1];
      }
      data->nnz_h_ = values_high.size();
      data->colind_h_ =
          (IndexT *)internal_alloc(data->nnz_h_ * sizeof(IndexT), platform_);
      data->values_h_ =
          (ValueT *)internal_alloc(data->nnz_h_ * sizeof(ValueT), platform_);
      std::move(colind_high.begin(), colind_high.end(), data->colind_h_);
      std::move(values_high.begin(), values_high.end(), data->values_h_);

      // Cleanup
      colind_high.clear();
      values_high.clear();
      colind_high.shrink_to_fit();
      values_high.shrink_to_fit();
    }
  }

  // for (int tid = 0; tid < nthreads_; ++tid) {
  //   SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
  //   nnz_lower_ += data->nnz_lower_;
  //   nnz_diag_ += data->nnz_diag_;
  // }

  cmp_symmetry_ = true;

  if (nthreads_ == 1)
    return;

// Find number of conflicts
// count_aposteriori_conflicts();

#ifdef _LOG_INFO
  float assembly_start = omp_get_wtime();
#endif
  const int blk_rows = (int)ceil(nrows_ / (double)BLK_FACTOR);
  ColoringGraph g(blk_rows);
  vector<WeightedVertex> vertices(blk_rows);
  tbb::concurrent_vector<tbb::concurrent_vector<pair<int, int>>> indirect(
      blk_rows);

  #pragma omp parallel num_threads(nthreads_)
  {
    int t = omp_get_thread_num();
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
    IndexT row_offset = row_split_[t];
    for (int i = row_split_[t]; i < row_split_[t + 1]; i++) {
      IndexT blk_row = i >> BLK_BITS;
      vertices[blk_row].vid = blk_row;
      vertices[blk_row].tid = t;
      vertices[blk_row].nnz +=
          data->rowptr_[i - row_offset + 1] - data->rowptr_[i - row_offset];
      if (hybrid_)
        vertices[blk_row].nnz += data->rowptr_h_[i - row_offset + 1] -
                                 data->rowptr_h_[i - row_offset];
      IndexT prev_blk_col = -1;
      for (int j = data->rowptr_[i - row_offset];
           j < data->rowptr_[i + 1 - row_offset]; ++j) {
        IndexT col = data->colind_[j];
        IndexT blk_col = col >> BLK_BITS;
        // If this nonzero is in the lower triangular part and has a direct
        // conflict with another thread
        if (col < row_offset) {
          g[blk_row].push_back(blk_col);
          g[blk_col].push_back(blk_row);
        }

        // Mark potential indirect conflicts
        if (blk_col != prev_blk_col)
          indirect[blk_col].push_back(make_pair(blk_row, t));
        prev_blk_col = blk_col;
      }
    }

    #pragma omp barrier

    int row_start = row_split_[t] >> BLK_BITS;
    int row_end = row_split_[t + 1] >> BLK_BITS;
    for (int i = row_start; i < row_end; i++) {
      for (const auto &row1 : indirect[i]) {
        for (const auto &row2 : indirect[i]) {
          if ((row1.first < row2.first) && (row1.second != row2.second)) {
            g[row1.first].push_back(row2.first);
            g[row2.first].push_back(row1.first);
          }
        }
      }
    }
  }

  for (auto &i : indirect)
    i.clear();
  indirect.clear();

#ifdef _LOG_INFO
  float assembly_stop = omp_get_wtime();
  cout << "[INFO]: graph assembly: " << assembly_stop - assembly_start << endl;
  cout << "[INFO]: using a blocking factor of: " << BLK_FACTOR << endl;
#endif

  const int V = g.size();
  vector<int> color_map(V, V - 1);
  // color(g, color_map);
  color(g, vertices, color_map);
// tbb::concurrent_vector<int> color_map(V, V - 1);
// parallel_color(g, color_map);
// parallel_color(g, vertices, color_map);

#ifndef _USE_BARRIER
  // Find thread dependency graph between colors.
  // Need to check if a row in color C is touched in color C-1 by others threads
  // It is enough to check if the neighbors of the correpsonding vertex are
  // colored with the previous color and are assigned to different threads
  bool cnfls[ncolors_][nthreads_][nthreads_];
  for (int i = 0; i < ncolors_; i++) {
    for (int t1 = 0; t1 < nthreads_; t1++) {
      for (int t2 = 0; t2 < nthreads_; t2++) {
        cnfls[i][t1][t2] = false;
      }
    }
  }

  // FIXME: optimize this
  for (int i = 0; i < V; i++) {
    int c_i = color_map[i];
    if (c_i > 0) {
      const auto &neighbors = g[i];
      for (size_t j = 0; j < neighbors.size(); j++) {
        int c_j = color_map[neighbors[j]];
        // Mark who I need to wait for before proceeding to current color
        if ((c_j == (c_i - 1)) &&
            (vertices[i].tid != vertices[neighbors[j]].tid))
          cnfls[c_i][vertices[i].tid][vertices[neighbors[j]].tid] = true;
      }
    }
  }

#ifdef _LOG_INFO
  for (int i = 0; i < ncolors_; i++) {
    for (int t1 = 0; t1 < nthreads_; t1++) {
      for (int t2 = 0; t2 < nthreads_; t2++) {
        if (cnfls[i][t1][t2])
          cout << "(C" << i << ", T" << t1 << ", T" << t2 << ")" << endl;
      }
    }
  }
#endif
#endif

  // Find row sets per thread per color
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];

    vector<vector<IndexT>> rowind(ncolors_);

#ifndef _USE_BARRIER
    // Determine dependency graph
    for (int c = 0; c < ncolors_; c++) {
      for (int t = 0; t < nthreads_; t++) {
        if (cnfls[c][tid][t]) {
          data->deps_[c].push_back(t);
        }
      }
    }
#endif

    // Find active row indices per color
    for (int i = row_split_[tid]; i < row_split_[tid + 1]; i++) {
      rowind[color_map[i >> BLK_BITS]].push_back(i);
    }

    // Detect ranges of consecutive rows
    vector<IndexT> row_start[ncolors_];
    vector<IndexT> row_end[ncolors_];
    IndexT row, row_prev;
    int nranges = 0;
    for (int c = 0; c < ncolors_; c++) {
      // assert(static_cast<int>(rowind[c].size()) <=
      //        row_split_[tid + 1] - row_split_[tid] + 1);
      if (rowind[c].size() > 0) {
        row_prev = rowind[c][0];
        row_start[c].push_back(row_prev);
        for (auto it = rowind[c].begin(); it != rowind[c].end(); ++it) {
          row = *it;
          if (row - row_prev > 1) {
            row_end[c].push_back(row_prev);
            row_start[c].push_back(row);
          }

          row_prev = row;
        }

        // Finalize row_end
        row_end[c].push_back(row);
      }

      nranges += row_start[c].size();
    }

    rowind.clear();
    rowind.shrink_to_fit();

    // Allocate auxiliary arrays
    data->ncolors_ = ncolors_;
    data->nranges_ = nranges;
    #pragma omp critical
    { nranges_ += data->nranges_; }
    data->range_ptr_ =
        (IndexT *)internal_alloc((ncolors_ + 1) * sizeof(IndexT));
    data->range_start_ = (IndexT *)internal_alloc(nranges * sizeof(IndexT));
    data->range_end_ = (IndexT *)internal_alloc(nranges * sizeof(IndexT));

    int cnt = 0;
    int row_offset = row_split_[tid];
    data->range_ptr_[0] = 0;
    for (int c = 0; c < ncolors_; c++) {
      data->range_ptr_[c + 1] = data->range_ptr_[c] + row_start[c].size();
      if (row_start[c].size() > 0) {
        for (int i = 0; i < static_cast<int>(row_start[c].size()); ++i) {
          data->range_start_[cnt] = row_start[c][i] - row_offset;
          data->range_end_[cnt] = row_end[c][i] - row_offset;
          cnt++;
        }
      }
    }
    assert(cnt == nranges);
  }

// nranges_ = 0;
// for (int t = 0; t < nthreads_; t++) {
//   //    cout << "T" << t << endl;
//   SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
//   nranges_ += data->nranges_;
//   // for (int c = 0; c < ncolors_; ++c) {
//   //   for (int r = data->range_ptr_[c]; r < data->range_ptr_[c + 1]; ++r) {
//   // 	size_t cnt = 0;
//   //     for (IndexT i = data->range_start_[r]; i <= data->range_end_[r];
//   ++i)
//   //     {
//   // 	  cnt += data->rowptr_[i+1] - data->rowptr_[i];
//   // 	}
//   // 	//cout << "Number of nonzeros in range " << r << ": " << cnt <<
//   endl;
//   //   }
//   // }
// }

#ifdef _LOG_INFO
  tstop = omp_get_wtime();
  cout << "[INFO]: conversion time: " << tstop - tstart << endl;
#endif
  conflict_free_aposteriori_ = true;
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::compress_symmetry() {
  if (!symmetric_) {
    return;
  }

  // atomics();
  // effective_ranges();
  // local_vectors_indexing();
  // conflict_free_apriori();
  conflict_free_aposteriori();

  // Cleanup
  if (owns_data_) {
    internal_free(rowptr_, platform_);
    internal_free(colind_, platform_);
    internal_free(values_, platform_);
    rowptr_ = nullptr;
    colind_ = nullptr;
    values_ = nullptr;
    if (hybrid_) {
      internal_free(rowptr_h_, platform_);
      internal_free(colind_h_, platform_);
      internal_free(values_h_, platform_);
      rowptr_h_ = nullptr;
      colind_h_ = nullptr;
      values_h_ = nullptr;
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::first_fit_round_robin(const ColoringGraph &g,
                                                      std::vector<int> &order) {
#ifdef _LOG_INFO
  cout << "[INFO]: applying FF-RR vertex ordering..." << endl;
#endif

  int cnt = 0, t_cnt = 0;
  while ((unsigned int)cnt < g.size()) {
    for (int t = 0; t < nthreads_; t++) {
      if (row_split_[t] + t_cnt < row_split_[t + 1]) {
        assert(((row_split_[t] + t_cnt) / BLK_FACTOR) < nrows_);
        order.push_back((row_split_[t] + t_cnt) / BLK_FACTOR);
        cnt++;
      }
    }

    t_cnt += BLK_FACTOR;
  }

  assert(order.size() == g.size());
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::shortest_row(const ColoringGraph &g,
                                             std::vector<int> &order) {
#ifdef _LOG_INFO
  cout << "[INFO]: applying SR vertex ordering..." << endl;
#endif

  // Sort rows by increasing number of nonzeros
  std::multimap<size_t, IndexT> row_nnz;
  for (int t = 0; t < nthreads_; ++t) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
    IndexT row_offset = row_split_[t];
    for (int i = 0; i < data->nrows_; ++i) {
      int nnz = data->rowptr_[i + 1] - data->rowptr_[i];
      row_nnz.insert(std::pair<size_t, IndexT>(nnz, i + row_offset));
    }
  }

  for (auto it = row_nnz.begin(); it != row_nnz.end(); ++it) {
    order.push_back(it->second);
  }

  assert(order.size() == g.size());
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::shortest_row_round_robin(
    const ColoringGraph &g, std::vector<int> &order) {
#ifdef _LOG_INFO
  cout << "[INFO]: applying SR-RR vertex ordering..." << endl;
#endif

  // Sort rows by increasing number of nonzeros per thread
  std::multimap<size_t, IndexT> row_nnz[nthreads_];
  for (int t = 0; t < nthreads_; ++t) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
    IndexT row_offset = row_split_[t];
    for (int i = 0; i < data->nrows_; ++i) {
      int nnz = data->rowptr_[i + 1] - data->rowptr_[i];
      row_nnz[t].insert(std::pair<size_t, IndexT>(nnz, i + row_offset));
    }
  }

  typename std::multimap<size_t, IndexT>::iterator it[nthreads_];
  for (int t = 0; t < nthreads_; t++) {
    it[t] = row_nnz[t].begin();
  }

  int cnt = 0;
  while (cnt < nrows_) {
    for (int t = 0; t < nthreads_; t++) {
      if (it[t] != row_nnz[t].end()) {
        order.push_back(it[t]->second);
        it[t]++;
        cnt++;
      }
    }
  }

  assert(static_cast<int>(order.size()) == nrows_);
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::longest_row(const ColoringGraph &g,
                                            std::vector<int> &order) {
#ifdef _LOG_INFO
  cout << "[INFO]: applying LR vertex ordering..." << endl;
#endif

  // Sort rows by decreasing number of nonzeros
  std::multimap<size_t, IndexT> row_nnz;
  for (int t = 0; t < nthreads_; ++t) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
    IndexT row_offset = row_split_[t];
    for (int i = 0; i < data->nrows_; ++i) {
      int nnz = data->rowptr_[i + 1] - data->rowptr_[i];
      row_nnz.insert(std::pair<size_t, IndexT>(nnz, i + row_offset));
    }
  }

  for (auto it = row_nnz.rbegin(); it != row_nnz.rend(); ++it) {
    order.push_back(it->second);
  }

  assert(order.size() == g.size());
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::longest_row_round_robin(
    const ColoringGraph &g, std::vector<int> &order) {
#ifdef _LOG_INFO
  cout << "[INFO]: applying LR-RR vertex ordering..." << endl;
#endif

  // Sort rows by decreasing number of nonzeros per thread
  std::multimap<size_t, IndexT> row_nnz[nthreads_];
  for (int t = 0; t < nthreads_; ++t) {
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[t];
    IndexT row_offset = row_split_[t];
    for (int i = 0; i < data->nrows_; ++i) {
      int nnz = data->rowptr_[i + 1] - data->rowptr_[i];
      row_nnz[t].insert(std::pair<size_t, IndexT>(nnz, i + row_offset));
    }
  }

  typename std::multimap<size_t, IndexT>::reverse_iterator it[nthreads_];
  for (int t = 0; t < nthreads_; t++) {
    it[t] = row_nnz[t].rbegin();
  }

  int cnt = 0;
  while (cnt < nrows_) {
    for (int t = 0; t < nthreads_; t++) {
      if (it[t] != row_nnz[t].rend()) {
        order.push_back(it[t]->second);
        it[t]++;
        cnt++;
      }
    }
  }

  assert(static_cast<int>(order.size()) == nrows_);
}

// FF-RR:        Colors vertices in a round-robin fashion among threads
//               but in the order they appear in the graph representation.
// SR:           Colors vertices in increasing row size.
// SR-RR:        Colors vertices in a round-robin fashion among threads
//               but in increasing row size order.
// LR:           Colors vertices in decreasing row size.
// LR-RR:        Colors vertices in a round-robin fashion among threads
//               but in decreasing row size order.
template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::ordering_heuristic(const ColoringGraph &g,
                                                   vector<int> &order) {
  order.reserve(g.size());

  first_fit_round_robin(g, order);
  // shortest_row(g, order);
  // shortest_row_round_robin(g, order);
  // longest_row(g, order);
  // longest_row_round_robin(g, order);
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::color(const ColoringGraph &g,
                                      vector<int> &color) {
  assert(symmetric_ && cmp_symmetry_);
#ifdef _LOG_INFO
  cout << "[INFO]: applying distance-1 graph coloring to detect conflict-free "
          "submatrices"
       << endl;
  float color_start = omp_get_wtime();
#endif

#ifdef _USE_ORDERING
  // Modify vertex ordering to improve coloring
  vector<int> order;
  ordering_heuristic(g, order);
#endif

  const int V = g.size();
  int max_color = 0;

  // We need to keep track of which colors are used by
  // adjacent vertices. We do this by marking the colors
  // that are used. The mark array contains the mark
  // for each color. The length of mark is the
  // number of vertices since the maximum possible number of colors
  // is the number of vertices.
  vector<int> mark(V, numeric_limits<int>::max());

  // Determine the color for every vertex one by one
  for (int i = 0; i < V; i++) {
#ifdef _USE_ORDERING
    const auto &neighbors = g[order[i]];
#else
    const auto &neighbors = g[i];
#endif

    // Mark the colors of vertices adjacent to current.
    // i can be the value for marking since i increases successively
    for (size_t j = 0; j < neighbors.size(); j++)
      mark[color[neighbors[j]]] = i;

    // Next step is to assign the smallest un-marked color
    // to the current vertex.
    int j = 0;

    // Scan through all useable colors, find the smallest possible
    // color that is not used by neighbors. Note that if mark[j]
    // is equal to i, color j is used by one of the current vertex's
    // neighbors.
    while (j < max_color && mark[j] == i)
      ++j;

    if (j == max_color) // All colors are used up. Add one more color
      ++max_color;

// At this point, j is the smallest possible color
#ifdef _USE_ORDERING
    color[order[i]] = j; // Save the color of vertex current
#else
    color[i] = j; // Save the color of vertex current
#endif
  }

  ncolors_ = max_color;

#ifdef _LOG_INFO
  float color_stop = omp_get_wtime();
  cout << "[INFO]: graph coloring: " << color_stop - color_start << endl;
  cout << "[INFO]: using " << ncolors_ << " colors" << endl;
#endif
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::color(const ColoringGraph &g,
                                      const vector<WeightedVertex> &v,
                                      vector<int> &color) {
  assert(symmetric_ && cmp_symmetry_);
#ifdef _LOG_INFO
  cout << "[INFO]: applying distance-1 balanced graph coloring to detect "
          "conflict-free submatrices"
       << endl;
  float color_start = omp_get_wtime();
#endif

#ifdef _USE_ORDERING
  // Modify vertex ordering to improve coloring
  vector<int> order;
  ordering_heuristic(g, order);
#endif

  const int V = g.size();
  int max_color = 0;

  // We need to keep track of which colors are used by
  // adjacent vertices. We do this by marking the colors
  // that are used. The mark array contains the mark
  // for each color. The length of mark is the
  // number of vertices since the maximum possible number of colors
  // is the number of vertices.
  vector<int> mark(V, numeric_limits<int>::max());

  // Determine the color for every vertex one by one
  for (int i = 0; i < V; i++) {
#ifdef _USE_ORDERING
    const auto &neighbors = g[order[i]];
#else
    const auto &neighbors = g[i];
#endif

    // Mark the colors of vertices adjacent to current.
    // i can be the value for marking since i increases successively
    for (size_t j = 0; j < neighbors.size(); j++)
      mark[color[neighbors[j]]] = i;

    // Next step is to assign the smallest un-marked color
    // to the current vertex.
    int j = 0;

    // Scan through all useable colors, find the smallest possible
    // color that is not used by neighbors. Note that if mark[j]
    // is equal to i, color j is used by one of the current vertex's
    // neighbors.
    while (j < max_color && mark[j] == i)
      ++j;

    if (j == max_color) // All colors are used up. Add one more color
      ++max_color;

// At this point, j is the smallest possible color
#ifdef _USE_ORDERING
    color[order[i]] = j; // Save the color of vertex current
#else
    color[i] = j; // Save the color of vertex current
#endif
  }

  // Balancing phase
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    int total_load = 0;
    int mean_load = 0;
    std::vector<int> load(max_color);
    std::vector<int> balance_deviation(max_color, 0);
    std::vector<std::priority_queue<WeightedVertex, std::vector<WeightedVertex>,
                                    CompareWeightedVertex>>
        bin(max_color);

    // Find total weight and vertices per color, total weight over all colors
    for (int i = 0; i < V; i++) {
      if (v[i].tid == tid) {
        total_load += v[i].nnz;
        load[color[i]] += v[i].nnz;
        bin[color[i]].push(v[i]);
      }
    }
    mean_load = total_load / max_color;

    for (int step = 0; step < BALANCING_STEPS; step++) {
      // Find balance deviation for this processor
      for (int c = 0; c < max_color; c++) {
        balance_deviation[c] = load[c] - mean_load;
      }

#ifdef _LOG_INFO
      #pragma omp critical
      {
        if (step == 0) {
          std::cout << std::fixed;
          std::cout << std::setprecision(2);
          cout << "[INFO]: T" << tid
               << " load distribution before balancing = { ";
          for (int c = 0; c < max_color; c++) {
            cout << ((float)load[c] / total_load) * 100 << "% ";
          }
          cout << "}" << endl;
        }
      }
#endif

      // Minimize balance deviation of each color c
      // The deviance reduction heuristic works by moving vertices from one
      // color with positive deviation to another legal color with a lower
      // deviation when this exchange will reduce the total deviation.
      // This is similar to a bin-packing problem, with the added constraint
      // that a vertex cannot be placed in the same bin as its neighbors.
      // Find color with largest positive deviation
      int max_c = distance(
          balance_deviation.begin(),
          max_element(balance_deviation.begin(), balance_deviation.end()));
      int i = 0;
      const int tol = 0;
      int no_vertices = static_cast<int>(bin[max_c].size());
      while (load[max_c] - mean_load > tol && i < no_vertices) {
        WeightedVertex current = bin[max_c].top();
        // Find eligible colors for this vertex
        std::vector<bool> used(max_color, false);
        used[max_c] = true;
        const auto &neighbors = g[current.vid];
        for (size_t j = 0; j < neighbors.size(); j++) {
          used[color[neighbors[j]]] = true;
        }

        // Re-color with the smallest eligible bin
        int min_c = max_c;
        int min_load = load[max_c];
        for (int c = 0; c < max_color; c++) {
          if (!used[c] && load[c] < min_load) {
            min_c = c;
            min_load = load[c];
          }
        }

        if (min_c != max_c) {
          // Update color bins
          color[current.vid] = min_c;
          load[max_c] -= current.nnz;
          load[min_c] += current.nnz;
          bin[max_c].pop();
          bin[min_c].push(current);
        }

        i++;
      }
    }

#ifdef _LOG_INFO
    #pragma omp critical
    {
      cout << "[INFO]: T" << tid << " load distribution after balancing = { ";
      for (int c = 0; c < max_color; c++) {
        cout << ((float)load[c] / total_load) * 100 << "% ";
      }
      cout << "}" << endl;
    }
#endif
  }

  ncolors_ = max_color;
  for (int i = 0; i < V; i++) {
    assert(color[i] < ncolors_);
  }

#ifdef _LOG_INFO
  float color_stop = omp_get_wtime();
  cout << "[INFO]: graph coloring: " << color_stop - color_start << endl;
  cout << "[INFO]: using " << ncolors_ << " colors" << endl;
#endif
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::parallel_color(
    const ColoringGraph &g, tbb::concurrent_vector<int> &color) {
  assert(symmetric_ && cmp_symmetry_);
#ifdef _LOG_INFO
  cout << "[INFO]: applying distance-1 parallel graph coloring to detect "
          "conflict-free submatrices"
       << endl;
  float color_start = omp_get_wtime();
#endif

#ifdef _USE_ORDERING
  // Modify vertex ordering to improve coloring
  vector<int> order;
  ordering_heuristic(g, order);
#endif

  const int V = g.size();
  vector<int> uncolored(V);

  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int i = 0; i < V; i++) {
#ifdef _USE_ORDERING
    uncolored[i] = order[i];
#else
    uncolored[i] = i;
#endif
  }

  int max_color_global = 0;
  int max_color[nthreads_] = {0};
  int U = V;
  while (U > 0) {
    // Phase 1: tentative coloring (parallel)
    #pragma omp parallel num_threads(nthreads_)
    {
      vector<int> mark(V, numeric_limits<int>::max());
      #pragma omp for schedule(static)
      for (int i = 0; i < U; i++) {
        int tid = omp_get_thread_num();
        int current = uncolored[i];
        const auto &neighbors = g[current];

        // We need to keep track of which colors are used by neightbors.
        // do this by marking the colors that are used.
        // unordered_map<int, int> mark;
        for (size_t j = 0; j < neighbors.size(); j++) {
          mark[color[neighbors[j]]] = i;
        }

        // Next step is to assign the smallest un-marked color to the current
        // vertex.
        int j = 0;

        // Find the smallest possible color that is not used by neighbors.
        while (j < max_color[tid] && mark[j] == i)
          ++j;

        // All colors are used up. Add one more color.
        if (j == max_color[tid])
          ++max_color[tid];

        // At this point, j is the smallest possible color. Save the color of
        // vertex current.
        color[current] = j; // Save the color of vertex current
      }

      #pragma omp barrier
      #pragma omp single
      for (int i = 0; i < nthreads_; i++) {
        if (max_color[i] > max_color_global) {
          max_color_global = max_color[i];
        }
        max_color[i] = max_color_global;
      }

      // Phase 2: conflict detection (parallel)
      #pragma omp for schedule(static)
      for (int i = 0; i < U; i++) {
        int current = uncolored[i];
        const auto &neighbors = g[current];
        for (size_t j = 0; j < neighbors.size(); j++) {
          // Add higher numbered vertex to uncolored set
          if ((color[neighbors[j]] == color[current])) {
            color[current] = V - 1;
          }
        }
      }
    }

    int tail = 0;
    for (int i = 0; i < U; i++) {
      if (color[uncolored[i]] == V - 1) {
        uncolored[tail++] = uncolored[i];
      }
    }

    U = tail;
  }

  ncolors_ = max_color_global;

#ifdef _LOG_INFO
  float color_stop = omp_get_wtime();
  cout << "[INFO]: graph coloring: " << color_stop - color_start << endl;
  cout << "[INFO]: using " << ncolors_ << " colors" << endl;
#endif
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::parallel_color(
    const ColoringGraph &g, const vector<WeightedVertex> &v,
    tbb::concurrent_vector<int> &color) {
  assert(symmetric_ && cmp_symmetry_);
#ifdef _LOG_INFO
  cout << "[INFO]: applying distance-1 parallel balanced graph coloring to "
          "detect conflict-free submatrices"
       << endl;
  float color_start = omp_get_wtime();
#endif

  // Modify vertex ordering to improve coloring
  vector<int> order;
  ordering_heuristic(g, order);

  const int V = g.size();
  vector<int> uncolored(V);

  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int i = 0; i < V; i++) {
#ifdef _USE_ORDERING
    uncolored[i] = order[i];
#else
    uncolored[i] = i;
#endif
  }

  int max_color_global = 0;
  int max_color[nthreads_] = {0};
  int U = V;
  while (U > 0) {
    // Phase 1: tentative coloring (parallel)
    #pragma omp parallel num_threads(nthreads_)
    {
      vector<int> mark(V, numeric_limits<int>::max());
      #pragma omp for schedule(static)
      for (int i = 0; i < U; i++) {
        int tid = omp_get_thread_num();
        int current = uncolored[i];
        const auto &neighbors = g[current];

        // We need to keep track of which colors are used by neightbors.
        // do this by marking the colors that are used.
        // unordered_map<int, int> mark;
        for (size_t j = 0; j < neighbors.size(); j++) {
          mark[color[neighbors[j]]] = i;
        }

        // Next step is to assign the smallest un-marked color to the current
        // vertex.
        int j = 0;

        // Find the smallest possible color that is not used by neighbors.
        while (j < max_color[tid] && mark[j] == i)
          ++j;

        // All colors are used up. Add one more color.
        if (j == max_color[tid])
          ++max_color[tid];

        // At this point, j is the smallest possible color. Save the color of
        // vertex current.
        color[current] = j; // Save the color of vertex current
      }
    }

    for (int i = 0; i < nthreads_; i++) {
      if (max_color[i] > max_color_global) {
        max_color_global = max_color[i];
      }
      max_color[i] = max_color_global;
    }

    // Phase 2: conflict detection (parallel)
    #pragma omp parallel for schedule(static) num_threads(nthreads_)
    for (int i = 0; i < U; i++) {
      int current = uncolored[i];
      const auto &neighbors = g[current];
      for (size_t j = 0; j < neighbors.size(); j++) {
        // Add higher numbered vertex to uncolored set
        if ((color[neighbors[j]] == color[current])) {
          color[current] = V - 1;
        }
      }
    }

    int tail = 0;
    for (int i = 0; i < U; i++) {
      if (color[uncolored[i]] == V - 1) {
        uncolored[tail++] = uncolored[i];
      }
    }

    U = tail;
  }

  ncolors_ = max_color_global;

  // Phase 3: color balancing
  for (int t = 0; t < nthreads_; t++) {
    int total_load = 0;
    int mean_load = 0;
    std::vector<int> load(ncolors_);
    int balance_deviation[ncolors_] = {0};
    std::vector<std::vector<WeightedVertex>> bin(ncolors_);

    // Find total weight and vertices per color, total weight over all colors
    // and balance deviation for this processor
    for (int i = 0; i < V; i++) {
      if (v[i].tid == t) {
        total_load += v[i].nnz;
        load[color[i]] += v[i].nnz;
        bin[color[i]].push_back(v[i]);
      }
    }
    mean_load = total_load / ncolors_;
    for (int c = 0; c < ncolors_; c++) {
      balance_deviation[c] = load[c] - mean_load;
    }

// Sort vertices per color bin in descending order of nonzeros
// for (int c = 0; c < max_color; c++) {
//   std::sort(bin[c].begin(), bin[c].end());
// }

#ifdef _LOG_INFO
    std::cout << std::fixed;
    std::cout << std::setprecision(2);
    cout << "[INFO]: T" << t << " load distribution before balancing = { ";
    for (int c = 0; c < ncolors_; c++) {
      cout << ((float)load[c] / total_load) * 100 << "% ";
    }
    cout << "}" << endl;
#endif

    // Minimize balance deviation of each color c
    // The deviance reduction heuristic works by moving vertices from one color
    // with positive deviation to another legal color with a lower deviation
    // when this exchange with reduce the total deviation.
    // This is similar to a bin-packing problem, with the added constraint that
    // a vertex cannot be placed in the same bin as its neighbors.
    // Find color with largest positive deviation
    // unsigned int max_deviation = *max_element(balance_deviation,
    // balance_deviation + ncolors_);
    int max_c =
        distance(balance_deviation,
                 max_element(balance_deviation, balance_deviation + ncolors_));
    int i = 0;
    // FIXME: tune tolerance
    const int tol = 0;
    int no_vertices = static_cast<int>(bin[max_c].size());
    while (load[max_c] - mean_load > tol && i < no_vertices) {
      int current = bin[max_c][i].vid;
      // Find eligible colors for this vertex
      bool used[ncolors_] = {false};
      used[max_c] = true;
      const auto &neighbors = g[current];
      for (size_t j = 0; j < neighbors.size(); j++) {
        assert(color[neighbors[j]] < ncolors_);
        used[color[neighbors[j]]] = true;
      }

      // Re-color with the smallest eligible bin
      int min_c = max_c;
      int min_load = load[max_c];
      for (int c = 0; c < ncolors_; c++) {
        if (!used[c] && load[c] < min_load) {
          min_c = c;
          min_load = load[c];
        }
      }
      color[current] = min_c;
      load[max_c] -= v[current].nnz;
      load[min_c] += v[current].nnz;

      i++;
    }

#ifdef _LOG_INFO
    cout << "[INFO]: T" << t << " load distribution after balancing = { ";
    for (int c = 0; c < ncolors_; c++) {
      cout << ((float)load[c] / total_load) * 100 << "% ";
    }
    cout << "}" << endl;
#endif
  }

#ifdef _LOG_INFO
  float color_stop = omp_get_wtime();
  cout << "[INFO]: graph coloring: " << color_stop - color_start << endl;
  cout << "[INFO]: using " << ncolors_ << " colors" << endl;
#endif
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_vanilla(ValueT *__restrict y,
                                               const ValueT *__restrict x) {
  #pragma omp parallel for schedule(static) num_threads(nthreads_)
  for (int i = 0; i < nrows_; ++i) {
    ValueT y_tmp = 0.0;

    #pragma omp simd reduction(+: y_tmp)
    for (IndexT j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
      y_tmp += values_[j] * x[colind_[j]];
    }

    y[i] = y_tmp;
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_split_nnz(ValueT *__restrict y,
                                                 const ValueT *__restrict x) {
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    for (IndexT i = row_split_[tid]; i < row_split_[tid + 1]; ++i) {
      register ValueT y_tmp = 0;

      #pragma omp simd reduction(+: y_tmp)
      for (IndexT j = rowptr_[i]; j < rowptr_[i + 1]; ++j) {
#if defined(_PREFETCH) && defined(_INTEL_COMPILER)
        /* T0: prefetch into L1, temporal with respect to all level caches */
        _mm_prefetch((const char *)&x[colind_[j + 16]], _MM_HINT_T2);
#endif
        y_tmp += values_[j] * x[colind_[j]];
      }

      /* Reduction on y */
      y[i] = y_tmp;
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_serial(ValueT *__restrict y,
                                                  const ValueT *__restrict x) {

  SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[0];
  IndexT *rowptr = data->rowptr_;
  IndexT *colind = data->colind_;
  ValueT *values = data->values_;
  ValueT *diagonal = data->diagonal_;

  for (int i = 0; i < nrows_; ++i) {
    ValueT y_tmp = diagonal[i] * x[i];

    #pragma omp simd reduction(+: y_tmp)
    for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
      IndexT col = colind[j];
      ValueT val = values[j];
      y_tmp += val * x[col];
      y[col] += val * x[i];
    }

    /* Reduction on y */
    y[i] = y_tmp;
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_atomics(ValueT *__restrict y,
                                                   const ValueT *__restrict x) {

  // Local vectors phase
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    IndexT row_offset = row_split_[tid];
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT *rowptr = data->rowptr_;
    IndexT *colind = data->colind_;
    ValueT *values = data->values_;
    ValueT *diagonal = data->diagonal_;

    for (int i = 0; i < data->nrows_; ++i) {
      y[i + row_offset] = diagonal[i] * x[i + row_offset];
    }
    #pragma omp barrier

    for (int i = 0; i < data->nrows_; ++i) {
      ValueT y_tmp = 0;

      for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
        IndexT col = colind[j];
        ValueT val = values[j];
        y_tmp += val * x[col];
        #pragma omp atomic
        y[col] += val * x[i + row_offset];
      }

      /* Reduction on y */
      y[i + row_offset] += y_tmp;
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_effective_ranges(
    ValueT *__restrict y, const ValueT *__restrict x) {

  // Local vectors phase
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    IndexT row_offset = row_split_[tid];
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT *rowptr = data->rowptr_;
    IndexT *colind = data->colind_;
    ValueT *values = data->values_;
    ValueT *diagonal = data->diagonal_;
    ValueT *y_local = data->local_vector_;
    if (tid == 0)
      y_local = y;

    for (int i = 0; i < data->nrows_; ++i) {
      y[i + row_offset] = diagonal[i] * x[i + row_offset];
    }
    #pragma omp barrier

    for (int i = 0; i < data->nrows_; ++i) {
      register ValueT y_tmp = 0;

      #pragma omp simd reduction(+: y_tmp)
      for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
        IndexT col = colind[j];
        ValueT val = values[j];
        y_tmp += val * x[col];
        if (col < row_split_[tid])
          y_local[col] += val * x[i + row_offset];
        else
          y[col] += val * x[i + row_offset];
      }

      /* Reduction on y */
      y[i + row_offset] += y_tmp;
    }

    #pragma omp barrier

    // Reduction of conflicts phase
    for (int tid = 1; tid < nthreads_; ++tid) {
      SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
      ValueT *y_local = data->local_vector_;
      #pragma omp for schedule(static)
      for (IndexT i = 0; i < row_split_[tid]; ++i) {
        y[i] += y_local[i];
        y_local[i] = 0;
      }
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_local_vectors_indexing(
    ValueT *__restrict y, const ValueT *__restrict x) {

  // Local vectors phase
  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    IndexT row_offset = row_split_[tid];
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT *rowptr = data->rowptr_;
    IndexT *colind = data->colind_;
    ValueT *values = data->values_;
    ValueT *diagonal = data->diagonal_;
    ValueT *y_local = data->local_vector_;
    if (tid == 0) {
      y_local = y;
    } else {
      memset(y_local, 0.0, row_split_[tid] * sizeof(ValueT));
    }

    for (int i = 0; i < data->nrows_; ++i) {
      y[i + row_offset] = diagonal[i] * x[i + row_offset];
    }
    #pragma omp barrier

    for (int i = 0; i < data->nrows_; ++i) {
      register ValueT y_tmp = 0;

      #pragma omp simd reduction(+: y_tmp)
      for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
        IndexT col = colind[j];
        ValueT val = values[j];
        y_tmp += val * x[col];
        if (col < row_split_[tid])
          y_local[col] += val * x[i + row_offset];
        else
          y[col] += val * x[i + row_offset];
      }

      /* Reduction on y */
      y[i + row_offset] += y_tmp;
    }

    #pragma omp barrier
    for (int i = data->map_start_; i < data->map_end_; ++i) {
      y[cnfl_map_->pos[i]] += y_local_[cnfl_map_->cpu[i]][cnfl_map_->pos[i]];
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free_apriori(
    ValueT *__restrict y, const ValueT *__restrict x) {

  #pragma omp parallel num_threads(nthreads_)
  {
    for (int c = 0; c < ncolors_; ++c) {
      #pragma omp for schedule(static, BLK_FACTOR * 64)
      for (int i = color_ptr_[c]; i < color_ptr_[c + 1]; ++i) {
        register IndexT row = rowind_sym_[i];
        register ValueT y_tmp = diagonal_[row] * x[row];

        #pragma omp simd reduction(+: y_tmp)
        for (IndexT j = rowptr_sym_[row]; j < rowptr_sym_[row + 1]; ++j) {
          IndexT col = colind_sym_[j];
          ValueT val = values_sym_[j];
          y_tmp += val * x[col];
          y[col] += val * x[row];
        }

        /* Reduction on y */
        y[row] += y_tmp;
      }
    }
  }
}

#ifndef _USE_BARRIER
// False indicates that the thread is still computing
extern std::atomic<bool> done[MAX_THREADS][MAX_COLORS];
#endif

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free(
    ValueT *__restrict y, const ValueT *__restrict x) {

  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    IndexT row_offset = row_split_[tid];
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT *rowptr = data->rowptr_;
    IndexT *colind = data->colind_;
    ValueT *values = data->values_;
    ValueT *diagonal = data->diagonal_;
    IndexT *range_ptr = data->range_ptr_;
    IndexT *range_start = data->range_start_;
    IndexT *range_end = data->range_end_;

#ifndef _USE_BARRIER
    for (int c = 0; c < ncolors_; ++c) {
      done[tid][c].store(false);
    }
#endif

    for (int i = 0; i < data->nrows_; ++i) {
      y[i + row_offset] = diagonal[i] * x[i + row_offset];
    }
    #pragma omp barrier

    for (int c = 0; c < ncolors_; ++c) {
#ifndef _USE_BARRIER
      // Wait until my dependencies have finished the previous phase
      for (size_t i = 0; i < data->deps_[c].size(); i++)
        while (!done[data->deps_[c][i]][c - 1])
          ;
#endif
      for (int r = range_ptr[c]; r < range_ptr[c + 1]; ++r) {
        for (IndexT i = range_start[r]; i <= range_end[r]; ++i) {
          register ValueT y_tmp = 0;

          #pragma omp simd reduction(+: y_tmp)
          for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
            IndexT col = colind[j];
            ValueT val = values[j];
            y_tmp += val * x[col];
            y[col] += val * x[i + row_offset];
          }

          /* Reduction on y */
          y[i + row_offset] += y_tmp;
        }
      }

#ifdef _USE_BARRIER
#pragma omp barrier
#else
      // Inform threads that depend on me that I have completed this phase
      done[tid][c] = true;
#endif
    }
  }
}

template <typename IndexT, typename ValueT>
void CSRMatrix<IndexT, ValueT>::cpu_mv_sym_conflict_free_hyb(
    ValueT *__restrict y, const ValueT *__restrict x) {

  #pragma omp parallel num_threads(nthreads_)
  {
    int tid = omp_get_thread_num();
    IndexT row_offset = row_split_[tid];
    SymmetryCompressionData<IndexT, ValueT> *data = sym_cmp_data_[tid];
    IndexT *rowptr = data->rowptr_;
    IndexT *colind = data->colind_;
    ValueT *values = data->values_;
    ValueT *diagonal = data->diagonal_;
    IndexT *range_ptr = data->range_ptr_;
    IndexT *range_start = data->range_start_;
    IndexT *range_end = data->range_end_;
    IndexT *rowptr_h = data->rowptr_h_;
    IndexT *colind_h = data->colind_h_;
    ValueT *values_h = data->values_h_;

    for (int i = 0; i < data->nrows_; ++i) {
      y[i + row_offset] = diagonal[i] * x[i + row_offset];
    }
    #pragma omp barrier

    for (int c = 0; c < ncolors_; ++c) {
      for (int r = range_ptr[c]; r < range_ptr[c + 1]; ++r) {
        for (IndexT i = range_start[r]; i <= range_end[r]; ++i) {
          register ValueT y_tmp = 0;

          #pragma omp simd reduction(+: y_tmp)
          for (IndexT j = rowptr[i]; j < rowptr[i + 1]; ++j) {
            IndexT col = colind[j];
            ValueT val = values[j];
            y_tmp += val * x[col];
            y[col] += val * x[i + row_offset];
          }

          #pragma omp simd reduction(+: y_tmp)
          for (IndexT j = rowptr_h[i]; j < rowptr_h[i + 1]; ++j) {
            y_tmp += values_h[j] * x[colind_h[j]];
          }

          /* Reduction on y */
          y[i + row_offset] += y_tmp;
        }
      }

      #pragma omp barrier
    }
  }
}

} // end of namespace sparse
} // end of namespace matrix
