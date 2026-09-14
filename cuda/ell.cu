/* ============================================================
 * ELLPACK (ELL) FORMAT + HIGH-BANDWIDTH SpMV
 * Column-major layout for perfect coalescing
 * Ahmad Ali Parr — hand-rolled C/CUDA, no AI generation
 * ============================================================ */

/* (requires types from csr.cu to be visible: Buffer, MemSpace, csr_matrix) */

typedef struct {
    int32_t rows, cols, max_nnz_per_row, nnz;
    Buffer col_idx;
    Buffer values;
    Buffer row_len;
    MemSpace space;
} ell_matrix;

static ell_matrix ell_create(int32_t rows, int32_t cols, int32_t max_nnz, MemSpace space) {
    ell_matrix m = {0};
    m.rows = rows; m.cols = cols; m.max_nnz_per_row = max_nnz; m.space = space;
    size_t ell_size = (size_t)rows * max_nnz;
    m.col_idx = buffer_alloc(ell_size * sizeof(int32_t), space);
    m.values  = buffer_alloc(ell_size * sizeof(float),   space);
    m.row_len = buffer_alloc(rows     * sizeof(int32_t), space);
    return m;
}

static void ell_destroy(ell_matrix *m) {
    if (!m) return;
    buffer_free(&m->col_idx);
    buffer_free(&m->values);
    buffer_free(&m->row_len);
    memset(m, 0, sizeof(*m));
}

static ell_matrix csr_to_ell(const csr_matrix *csr, MemSpace target) {
    int32_t max_nnz = 0;
    const int32_t *rp = (const int32_t*)csr->row_ptr.ptr;
    for (int32_t r = 0; r < csr->rows; r++) {
        int32_t len = rp[r+1]-rp[r]; if (len > max_nnz) max_nnz = len;
    }
    if (max_nnz == 0) max_nnz = 1;

    ell_matrix ell = ell_create(csr->rows, csr->cols, max_nnz, MEM_HOST);
    ell.nnz = csr->nnz;

    int32_t *ell_col = (int32_t*)ell.col_idx.ptr;
    float   *ell_val = (float*)  ell.values.ptr;
    int32_t *ell_len = (int32_t*)ell.row_len.ptr;
    const int32_t *ci = (const int32_t*)csr->col_idx.ptr;
    const float   *v  = (const float*)  csr->values.ptr;

    for (int32_t r = 0; r < csr->rows; r++) {
        int32_t len = rp[r+1]-rp[r];
        ell_len[r] = len;
        for (int32_t j = 0; j < max_nnz; j++) {
            size_t dst = (size_t)j * csr->rows + r; /* column-major */
            if (j < len) { ell_col[dst] = ci[rp[r]+j]; ell_val[dst] = v[rp[r]+j]; }
            else         { ell_col[dst] = 0;             ell_val[dst] = 0.0f; }
        }
    }

    if (target != MEM_HOST) {
        ell_matrix dev = ell_create(ell.rows, ell.cols, ell.max_nnz_per_row, target);
        dev.nnz = ell.nnz;
        buffer_copy(&dev.col_idx, &ell.col_idx);
        buffer_copy(&dev.values,  &ell.values);
        buffer_copy(&dev.row_len, &ell.row_len);
        ell_destroy(&ell);
        return dev;
    }
    return ell;
}

#ifdef __CUDACC__
__global__ void __launch_bounds__(256, 8)
ell_spmv_kernel(int32_t rows, int32_t max_nnz,
                const int32_t* __restrict__ col_idx,
                const float*   __restrict__ values,
                const int32_t* __restrict__ row_len,
                const float*   __restrict__ x,
                float*         __restrict__ y,
                float alpha, float beta)
{
    const int row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) return;
    const int32_t len = row_len ? __ldg(&row_len[row]) : max_nnz;
    float sum = 0.0f;
    #pragma unroll 4
    for (int j = 0; j < max_nnz; j++) {
        if (j >= len) break;
        size_t idx = (size_t)j * rows + row;
        sum += __ldg(&values[idx]) * __ldg(&x[__ldg(&col_idx[idx])]);
    }
    float yi = (beta == 0.0f) ? 0.0f : beta * y[row];
    y[row] = alpha * sum + yi;
}
#endif

static void ell_spmv(const ell_matrix *A,
                     const float *x, float *y,
                     float alpha, float beta)
{
    if (A->space != MEM_DEVICE) {
        const int32_t *col = (const int32_t*)A->col_idx.ptr;
        const float   *val = (const float*)  A->values.ptr;
        const int32_t *len = (const int32_t*)A->row_len.ptr;
        for (int32_t r = 0; r < A->rows; r++) {
            float sum = 0.0f;
            int32_t rowlen = len ? len[r] : A->max_nnz_per_row;
            for (int32_t j = 0; j < rowlen; j++) {
                size_t idx = (size_t)j * A->rows + r;
                sum += val[idx] * x[col[idx]];
            }
            y[r] = alpha * sum + beta * y[r];
        }
        return;
    }
#ifdef __CUDACC__
    const int THREADS = 256;
    const int BLOCKS = (A->rows + THREADS - 1) / THREADS;
    ell_spmv_kernel<<<BLOCKS, THREADS>>>(
        A->rows, A->max_nnz_per_row,
        (const int32_t*)A->col_idx.ptr,
        (const float*)  A->values.ptr,
        (const int32_t*)A->row_len.ptr,
        x, y, alpha, beta);
    CUDA_CHECK_LAST();
    CUDA_CHECK(cudaDeviceSynchronize());
#else
    fprintf(stderr, "ell_spmv: device matrix requires CUDA\n");
    exit(EXIT_FAILURE);
#endif
}
