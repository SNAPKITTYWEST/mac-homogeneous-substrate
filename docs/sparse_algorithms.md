# Sparse Matrix Multiplication Algorithms

Survey relevant to the sparse 2D neural-network workload.

---

## 1. Sparse Matrix–Vector Multiplication (SpMV)

**Goal:** y = A x, A sparse, x and y dense.

| Algorithm | Core Idea | Strengths | Weaknesses |
|-----------|-----------|-----------|------------|
| CSR warp-centric | One warp per row, shuffle reduction | Good balance, high occupancy | Sensitive to very long rows |
| ELLPACK / SELL-C-σ | Pad rows to fixed length, column-major | Excellent coalescing & bandwidth | Padding overhead |
| Hybrid ELL + COO | ELL for short rows, COO for long | Combines regularity + flexibility | More complex |
| BCSR | Small dense blocks | Better cache reuse, SIMD | Only good when blocks are dense |

**Key performance limiters:**
- Memory bandwidth (usually the bottleneck)
- Irregular gather from the dense vector x
- Load imbalance across rows

---

## 2. Sparse Matrix–Matrix Multiplication (SpGEMM)

**Goal:** C = A × B, all sparse.

The sparsity pattern of C is not known in advance — this is the hard part.

### Sequential / CPU Algorithms

| Algorithm | Description | Notes |
|-----------|-------------|-------|
| Gustavson (row-wise) | Scatter-add rows of B into accumulator | Foundation of most high-performance codes |
| Hash-based | Hash table as row accumulator | Good for high-nnz rows |
| Two-phase | Symbolic (pattern) + numeric | Exact allocation, zero waste |

### Parallel / GPU Algorithms

| Method | Core Technique | Pros | Cons |
|--------|---------------|------|------|
| Expand-Sort-Compress (ESC) | Expand all products, sort, compress | Simple, highly parallel | High memory traffic |
| Hash SpGEMM | Per-row hash table in global VRAM | Fast for moderate density | Hash conflicts |
| Gustavson + SPA | Classic with dense/sparse SPA | Excellent arithmetic intensity | SPA size can be large |
| Two-phase | Symbolic + numeric | Perfect allocation | Extra pass |

### Algorithmic Structure

1. **Symbolic phase** — determine exact nnz per row of C
2. **Numeric phase** — accumulate products using hash/SPA
3. **Load balancing** — group rows by estimated intermediate product count

---

## 3. Relevance to This Stack

| Operation | Recommended Algorithm |
|-----------|----------------------|
| SpMV, moderate irregularity | Warp-centric CSR (implemented) |
| SpMV, regular / bandwidth-bound | ELLPACK (implemented) |
| SpGEMM, host, unknown pattern | Gustavson hash (implemented) |
| SpGEMM, need exact memory | Two-phase symbolic+numeric (implemented) |
| SpGEMM, GPU, max parallelism | Hash SpGEMM CUDA kernels (implemented) |
| Structured sparse conv | Custom sparse kernel |

---

## 4. Algorithm Selection Guide

| Situation | Algorithm |
|-----------|-----------|
| SpMV, irregular | Warp-centric CSR |
| SpMV, regular | ELLPACK |
| SpGEMM, small-medium, CPU | Gustavson + SPA |
| SpGEMM, GPU | Hash SpGEMM or ESC |
| SpGEMM, need exact memory | Two-phase |
| Automatic | Hybrid (max/avg ratio threshold) |

---

## 5. Implementation Notes

**CSR layout:**
- `row_ptr[0..rows]` — `row_ptr[i]` = index of first nnz in row i
- `col_idx[0..nnz)` — column index of each nnz
- `values[0..nnz)` — value of each nnz

**ELL layout (column-major for coalescing):**
- Threads in a warp read consecutive addresses
- Padding with zero values and column 0 for short rows

**CUDA SpGEMM hash tables:**
- One hash table per row, stored in global memory
- Phase 1: symbolic — find nnz per row
- Phase 2: numeric — accumulate + sort + write CSR

## References

- Gustavson, F.G. (1978): Two fast algorithms for sparse matrices
- Bell & Garland (2008): Efficient sparse matrix-vector multiplication on CUDA
- Merrill & Garland (2016): Merge-based sparse matrix-vector multiplication
- nsparse, bhSPARSE, KokkosKernels: state-of-the-art SpGEMM libraries
