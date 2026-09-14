/* ============================================================
 * CSR FORMAT + WARP-CENTRIC SpMV
 * Ahmad Ali Parr — hand-rolled C/CUDA, no AI generation
 * y = alpha * A * x + beta * y
 * ============================================================ */

#ifdef __CUDACC__
#include <cuda_runtime.h>
#include <cooperative_groups.h>
namespace cg = cooperative_groups;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef WARP_SIZE
#define WARP_SIZE 32
#endif

#define CUDA_CHECK(call) do { \
    cudaError_t e = (call); \
    if (e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e)); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define CUDA_CHECK_LAST() CUDA_CHECK(cudaGetLastError())

typedef enum { MEM_HOST, MEM_DEVICE, MEM_MANAGED } MemSpace;

typedef struct {
    void *ptr;
    size_t size;
    MemSpace space;
} Buffer;

static Buffer buffer_alloc(size_t size, MemSpace space) {
    Buffer b = {0}; b.size = size; b.space = space;
    if (space == MEM_HOST) {
        b.ptr = calloc(1, size);
    }
#ifdef __CUDACC__
    else if (space == MEM_DEVICE) {
        CUDA_CHECK(cudaMalloc(&b.ptr, size));
        CUDA_CHECK(cudaMemset(b.ptr, 0, size));
    } else if (space == MEM_MANAGED) {
        CUDA_CHECK(cudaMallocManaged(&b.ptr, size));
        memset(b.ptr, 0, size);
    }
#endif
    return b;
}

static void buffer_free(Buffer *b) {
    if (!b || !b->ptr) return;
    if (b->space == MEM_HOST) free(b->ptr);
#ifdef __CUDACC__
    else cudaFree(b->ptr);
#endif
    b->ptr = NULL;
}

static void buffer_copy(Buffer *dst, const Buffer *src) {
    if (!dst || !src || !src->ptr) return;
    size_t sz = src->size < dst->size ? src->size : dst->size;
#ifdef __CUDACC__
    cudaMemcpyKind kind =
        (src->space == MEM_HOST && dst->space == MEM_HOST) ? cudaMemcpyHostToHost :
        (src->space == MEM_HOST) ? cudaMemcpyHostToDevice :
        (dst->space == MEM_HOST) ? cudaMemcpyDeviceToHost :
        cudaMemcpyDeviceToDevice;
    CUDA_CHECK(cudaMemcpy(dst->ptr, src->ptr, sz, kind));
#else
    memcpy(dst->ptr, src->ptr, sz);
#endif
}

/* ------------------------------------------------------------------ */
/* CSR matrix */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t rows, cols, nnz;
    Buffer row_ptr;
    Buffer col_idx;
    Buffer values;
    MemSpace space;
} csr_matrix;

static csr_matrix csr_create(int32_t rows, int32_t cols, int32_t nnz_cap, MemSpace space) {
    csr_matrix m = {0};
    m.rows = rows; m.cols = cols; m.nnz = 0; m.space = space;
    m.row_ptr = buffer_alloc((size_t)(rows+1)*sizeof(int32_t), space);
    m.col_idx = buffer_alloc((size_t)nnz_cap*sizeof(int32_t), space);
    m.values  = buffer_alloc((size_t)nnz_cap*sizeof(float), space);
    if (space == MEM_HOST) {
        ((int32_t*)m.row_ptr.ptr)[0] = 0;
    }
    return m;
}

static void csr_destroy(csr_matrix *m) {
    if (!m) return;
    buffer_free(&m->row_ptr);
    buffer_free(&m->col_idx);
    buffer_free(&m->values);
    memset(m, 0, sizeof(*m));
}

static csr_matrix csr_clone(const csr_matrix *src, MemSpace target) {
    csr_matrix dst = csr_create(src->rows, src->cols, src->nnz, target);
    dst.nnz = src->nnz;
    buffer_copy(&dst.row_ptr, &src->row_ptr);
    buffer_copy(&dst.col_idx, &src->col_idx);
    buffer_copy(&dst.values,  &src->values);
    return dst;
}

static csr_matrix dense_to_csr(const float *dense, int32_t rows, int32_t cols,
                               float threshold, MemSpace target) {
    int32_t nnz = 0;
    for (int32_t i = 0; i < rows * cols; i++)
        if (fabsf(dense[i]) > threshold) nnz++;

    csr_matrix m = csr_create(rows, cols, nnz, MEM_HOST);
    m.nnz = nnz;
    int32_t *rp = (int32_t*)m.row_ptr.ptr;
    int32_t *ci = (int32_t*)m.col_idx.ptr;
    float   *v  = (float*)  m.values.ptr;

    int32_t pos = 0;
    for (int32_t r = 0; r < rows; r++) {
        rp[r] = pos;
        for (int32_t c = 0; c < cols; c++) {
            float val = dense[r*cols+c];
            if (fabsf(val) > threshold) { ci[pos] = c; v[pos] = val; pos++; }
        }
    }
    rp[rows] = pos;

    if (target != MEM_HOST) {
        csr_matrix dev = csr_clone(&m, target);
        csr_destroy(&m);
        return dev;
    }
    return m;
}

static int32_t csr_row_nnz(const csr_matrix *A, int32_t row) {
    if (A->space != MEM_HOST) return -1;
    const int32_t *rp = (const int32_t*)A->row_ptr.ptr;
    return rp[row+1] - rp[row];
}

static void csr_row_stats(const csr_matrix *A, float *avg_nnz, int32_t *max_nnz) {
    if (A->space != MEM_HOST) return;
    const int32_t *rp = (const int32_t*)A->row_ptr.ptr;
    int32_t mx = 0; int64_t sum = 0;
    for (int32_t r = 0; r < A->rows; r++) {
        int32_t len = rp[r+1]-rp[r]; sum += len; if (len > mx) mx = len;
    }
    *avg_nnz = A->rows ? (float)sum/(float)A->rows : 0.0f;
    *max_nnz = mx;
}

static void csr_print_info(const csr_matrix *A, const char *name) {
    printf("CSR \"%s\": %d x %d, nnz=%d, space=%s\n",
           name ? name : "matrix", A->rows, A->cols, A->nnz,
           A->space == MEM_HOST ? "HOST" : A->space == MEM_DEVICE ? "DEVICE" : "MANAGED");
}

/* ------------------------------------------------------------------ */
/* Warp-centric SpMV kernel */
/* ------------------------------------------------------------------ */
#ifdef __CUDACC__

__device__ __forceinline__ float warp_reduce_sum(float val) {
    #pragma unroll
    for (int offset = WARP_SIZE/2; offset > 0; offset >>= 1)
        val += __shfl_down_sync(0xffffffff, val, offset);
    return val;
}

__global__ void __launch_bounds__(256, 4)
csr_spmv_warp_kernel(int32_t rows,
                     const int32_t* __restrict__ row_ptr,
                     const int32_t* __restrict__ col_idx,
                     const float*   __restrict__ values,
                     const float*   __restrict__ x,
                     float*         __restrict__ y,
                     float alpha, float beta)
{
    cg::thread_block block = cg::this_thread_block();
    cg::thread_block_tile<WARP_SIZE> warp = cg::tiled_partition<WARP_SIZE>(block);

    const int warp_id = (blockIdx.x * blockDim.x + threadIdx.x) / WARP_SIZE;
    const int lane_id = threadIdx.x % WARP_SIZE;
    if (warp_id >= rows) return;

    const int32_t row_start = __ldg(&row_ptr[warp_id]);
    const int32_t row_end   = __ldg(&row_ptr[warp_id+1]);

    float sum = 0.0f;
    for (int32_t i = row_start + lane_id; i < row_end; i += WARP_SIZE) {
        const int32_t c = __ldg(&col_idx[i]);
        const float   v = __ldg(&values[i]);
        sum += v * __ldg(&x[c]);
    }
    sum = warp_reduce_sum(sum);
    if (lane_id == 0) {
        float yi = (beta == 0.0f) ? 0.0f : beta * y[warp_id];
        y[warp_id] = alpha * sum + yi;
    }
}

#endif /* __CUDACC__ */

static void csr_spmv(const csr_matrix *A,
                     const float *x, float *y,
                     float alpha, float beta)
{
    if (A->space == MEM_DEVICE) {
#ifdef __CUDACC__
        const int WARPS = 8, THREADS = WARPS * WARP_SIZE;
        const int BLOCKS = (A->rows + WARPS - 1) / WARPS;
        csr_spmv_warp_kernel<<<BLOCKS, THREADS>>>(
            A->rows,
            (const int32_t*)A->row_ptr.ptr,
            (const int32_t*)A->col_idx.ptr,
            (const float*)  A->values.ptr,
            x, y, alpha, beta);
        CUDA_CHECK_LAST();
        CUDA_CHECK(cudaDeviceSynchronize());
#else
        fprintf(stderr, "csr_spmv: device matrix requires CUDA\n");
        exit(EXIT_FAILURE);
#endif
    } else {
        const int32_t *rp = (const int32_t*)A->row_ptr.ptr;
        const int32_t *ci = (const int32_t*)A->col_idx.ptr;
        const float   *v  = (const float*)  A->values.ptr;
        for (int32_t row = 0; row < A->rows; row++) {
            float sum = 0.0f;
            for (int32_t i = rp[row]; i < rp[row+1]; i++) sum += v[i]*x[ci[i]];
            y[row] = alpha*sum + beta*y[row];
        }
    }
}
