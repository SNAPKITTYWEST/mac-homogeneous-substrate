/* ============================================================
 * SpGEMM: Gustavson hash + Two-phase + ESC + CUDA device kernels
 * Ahmad Ali Parr — hand-rolled C/CUDA, no AI generation
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define HASH_EMPTY (-1)
#define HASH_SCALE 2

/* --- Open-addressing hash accumulator --- */
typedef struct { int32_t *key; float *val; int32_t capacity; int32_t size; } HashAcc;

static HashAcc hash_create(int32_t cap) {
    HashAcc h = {0}; h.capacity = cap;
    h.key = malloc(cap * sizeof(int32_t)); h.val = malloc(cap * sizeof(float));
    for (int32_t i = 0; i < cap; i++) h.key[i] = HASH_EMPTY;
    return h;
}
static void hash_destroy(HashAcc *h) { free(h->key); free(h->val); memset(h,0,sizeof(*h)); }
static void hash_clear(HashAcc *h) { for(int32_t i=0;i<h->capacity;i++) h->key[i]=HASH_EMPTY; h->size=0; }

static inline uint32_t hash_func(int32_t col, int32_t cap) {
    return ((uint32_t)col * 2654435761u) % (uint32_t)cap;
}

static void hash_insert(HashAcc *h, int32_t col, float v) {
    uint32_t idx = hash_func(col, h->capacity);
    while (h->key[idx] != HASH_EMPTY && h->key[idx] != col) idx = (idx+1) % h->capacity;
    if (h->key[idx] == HASH_EMPTY) { h->key[idx] = col; h->val[idx] = v; h->size++; }
    else h->val[idx] += v;
}

/* (csr_matrix and buffer helpers assumed from csr.cu) */
typedef struct csr_matrix csr_matrix; /* forward */

/* --- Gustavson + hash (host) --- */
csr_matrix spgemm_gustavson_hash(const csr_matrix *A, const csr_matrix *B, int target_host) {
    /* host-only implementation */
    const int32_t *Ap = (const int32_t*)A->row_ptr.ptr;
    const int32_t *Aj = (const int32_t*)A->col_idx.ptr;
    const float   *Av = (const float*)  A->values.ptr;
    const int32_t *Bp = (const int32_t*)B->row_ptr.ptr;
    const int32_t *Bj = (const int32_t*)B->col_idx.ptr;
    const float   *Bv = (const float*)  B->values.ptr;

    int32_t total_est = 0;
    for (int32_t i = 0; i < A->rows; i++)
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++)
            total_est += Bp[Aj[p]+1] - Bp[Aj[p]];

    /* Allocate result */
    extern csr_matrix csr_create(int32_t, int32_t, int32_t, int);
    csr_matrix C = csr_create(A->rows, B->cols, total_est, 0 /* MEM_HOST */);
    int32_t *Cp = (int32_t*)C.row_ptr.ptr;
    int32_t *Cj = (int32_t*)C.col_idx.ptr;
    float   *Cv = (float*)  C.values.ptr;

    HashAcc acc = hash_create(HASH_SCALE * (total_est / A->rows + 16));
    int32_t nz = 0; Cp[0] = 0;

    for (int32_t i = 0; i < A->rows; i++) {
        hash_clear(&acc);
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++) {
            int32_t j = Aj[p]; float a = Av[p];
            for (int32_t q = Bp[j]; q < Bp[j+1]; q++) hash_insert(&acc, Bj[q], a*Bv[q]);
        }
        int32_t *tmp_col = malloc(acc.size * sizeof(int32_t));
        float   *tmp_val = malloc(acc.size * sizeof(float));
        int32_t cnt = 0;
        for (int32_t k = 0; k < acc.capacity; k++)
            if (acc.key[k] != HASH_EMPTY) { tmp_col[cnt] = acc.key[k]; tmp_val[cnt] = acc.val[k]; cnt++; }

        /* insertion sort */
        for (int32_t a2 = 1; a2 < cnt; a2++) {
            int32_t kc = tmp_col[a2]; float kv = tmp_val[a2]; int32_t b2 = a2-1;
            while (b2 >= 0 && tmp_col[b2] > kc) { tmp_col[b2+1]=tmp_col[b2]; tmp_val[b2+1]=tmp_val[b2]; b2--; }
            tmp_col[b2+1]=kc; tmp_val[b2+1]=kv;
        }
        for (int32_t k = 0; k < cnt; k++) { Cj[nz]=tmp_col[k]; Cv[nz]=tmp_val[k]; nz++; }
        Cp[i+1] = nz;
        free(tmp_col); free(tmp_val);
    }
    C.nnz = nz;
    hash_destroy(&acc);
    return C;
}

/* --- Two-phase SpGEMM (host) --- */
csr_matrix spgemm_two_phase(const csr_matrix *A, const csr_matrix *B) {
    const int32_t *Ap = (const int32_t*)A->row_ptr.ptr;
    const int32_t *Aj = (const int32_t*)A->col_idx.ptr;
    const float   *Av = (const float*)  A->values.ptr;
    const int32_t *Bp = (const int32_t*)B->row_ptr.ptr;
    const int32_t *Bj = (const int32_t*)B->col_idx.ptr;
    const float   *Bv = (const float*)  B->values.ptr;

    int32_t *c_row_ptr = calloc(A->rows+1, sizeof(int32_t));
    int32_t *marker    = malloc(B->cols * sizeof(int32_t));
    for (int32_t i = 0; i < B->cols; i++) marker[i] = -1;

    /* Symbolic phase */
    for (int32_t i = 0; i < A->rows; i++) {
        int32_t nnz = 0;
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++) {
            int32_t j = Aj[p];
            for (int32_t q = Bp[j]; q < Bp[j+1]; q++) {
                int32_t col = Bj[q];
                if (marker[col] != i) { marker[col] = i; nnz++; }
            }
        }
        c_row_ptr[i+1] = c_row_ptr[i] + nnz;
    }

    int32_t c_nnz = c_row_ptr[A->rows];
    extern csr_matrix csr_create(int32_t, int32_t, int32_t, int);
    csr_matrix C = csr_create(A->rows, B->cols, c_nnz, 0);
    C.nnz = c_nnz;
    memcpy(C.row_ptr.ptr, c_row_ptr, (A->rows+1)*sizeof(int32_t));

    /* Numeric phase */
    int32_t *Cj   = (int32_t*)C.col_idx.ptr;
    float   *Cv   = (float*)  C.values.ptr;
    int32_t *next = malloc(B->cols * sizeof(int32_t));
    float   *spa  = calloc(B->cols, sizeof(float));
    for (int32_t i = 0; i < B->cols; i++) marker[i] = -1;

    for (int32_t i = 0; i < A->rows; i++) {
        int32_t head = -2, nnz = 0;
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++) {
            int32_t j = Aj[p]; float a = Av[p];
            for (int32_t q = Bp[j]; q < Bp[j+1]; q++) {
                int32_t col = Bj[q];
                if (marker[col] != i) { marker[col]=i; next[col]=head; head=col; spa[col]=a*Bv[q]; nnz++; }
                else spa[col] += a*Bv[q];
            }
        }
        int32_t pos = c_row_ptr[i];
        for (int32_t col = head; col != -2; col = next[col]) {
            Cj[pos] = col; Cv[pos] = spa[col]; spa[col] = 0.0f; pos++;
        }
    }

    free(c_row_ptr); free(marker); free(next); free(spa);
    return C;
}

/* --- Expand-Sort-Compress SpGEMM (host) --- */
typedef struct { int32_t row, col; float val; } Triple;
static int triple_cmp(const void *a, const void *b) {
    const Triple *x = (const Triple*)a, *y = (const Triple*)b;
    if (x->row != y->row) return x->row - y->row;
    return x->col - y->col;
}

csr_matrix spgemm_esc(const csr_matrix *A, const csr_matrix *B) {
    const int32_t *Ap = (const int32_t*)A->row_ptr.ptr;
    const int32_t *Aj = (const int32_t*)A->col_idx.ptr;
    const float   *Av = (const float*)  A->values.ptr;
    const int32_t *Bp = (const int32_t*)B->row_ptr.ptr;
    const int32_t *Bj = (const int32_t*)B->col_idx.ptr;
    const float   *Bv = (const float*)  B->values.ptr;

    int64_t nprod = 0;
    for (int32_t i = 0; i < A->rows; i++)
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++)
            nprod += Bp[Aj[p]+1] - Bp[Aj[p]];

    Triple *prod = malloc(nprod * sizeof(Triple));
    int64_t pos = 0;
    for (int32_t i = 0; i < A->rows; i++)
        for (int32_t p = Ap[i]; p < Ap[i+1]; p++) {
            int32_t j = Aj[p]; float a = Av[p];
            for (int32_t q = Bp[j]; q < Bp[j+1]; q++) {
                prod[pos].row = i; prod[pos].col = Bj[q]; prod[pos].val = a*Bv[q]; pos++;
            }
        }

    qsort(prod, nprod, sizeof(Triple), triple_cmp);

    int32_t c_nnz = 0;
    for (int64_t i = 0; i < nprod; ) {
        int32_t r = prod[i].row, c = prod[i].col; float sum = 0.0f;
        while (i < nprod && prod[i].row == r && prod[i].col == c) { sum += prod[i].val; i++; }
        prod[c_nnz].row = r; prod[c_nnz].col = c; prod[c_nnz].val = sum; c_nnz++;
    }

    extern csr_matrix csr_create(int32_t, int32_t, int32_t, int);
    csr_matrix C = csr_create(A->rows, B->cols, c_nnz, 0);
    C.nnz = c_nnz;
    int32_t *Cp = (int32_t*)C.row_ptr.ptr;
    int32_t *Cj = (int32_t*)C.col_idx.ptr;
    float   *Cv = (float*)  C.values.ptr;

    int32_t cur = 0; Cp[0] = 0;
    for (int32_t r = 0; r < A->rows; r++) {
        while (cur < c_nnz && prod[cur].row == r) { Cj[cur]=prod[cur].col; Cv[cur]=prod[cur].val; cur++; }
        Cp[r+1] = cur;
    }
    free(prod);
    return C;
}

/* ------------------------------------------------------------------ */
/* CUDA SpGEMM — two-phase global-memory hash */
/* ------------------------------------------------------------------ */
#ifdef __CUDACC__

__global__ void spgemm_hash_symbolic_kernel(
    int32_t rows, int32_t hash_cap,
    const int32_t *Ap, const int32_t *Aj,
    const int32_t *Bp, const int32_t *Bj,
    int32_t *c_row_nnz, int32_t *d_hash_keys)
{
    int32_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) return;
    int32_t *my_keys = d_hash_keys + row * hash_cap;
    for (int32_t i = 0; i < hash_cap; i++) my_keys[i] = HASH_EMPTY;

    int32_t nnz = 0;
    for (int32_t p = Ap[row]; p < Ap[row+1]; p++) {
        int32_t b_row = Aj[p];
        for (int32_t q = Bp[b_row]; q < Bp[b_row+1]; q++) {
            int32_t col = Bj[q];
            uint32_t idx = ((uint32_t)col * 2654435761u) % (uint32_t)hash_cap;
            while (my_keys[idx] != HASH_EMPTY && my_keys[idx] != col) idx = (idx+1) % hash_cap;
            if (my_keys[idx] == HASH_EMPTY) { my_keys[idx] = col; nnz++; }
        }
    }
    c_row_nnz[row] = nnz;
}

__global__ void spgemm_hash_numeric_kernel(
    int32_t rows, int32_t hash_cap,
    const int32_t *Ap, const int32_t *Aj, const float *Av,
    const int32_t *Bp, const int32_t *Bj, const float *Bv,
    const int32_t *Cp, int32_t *Cj, float *Cv,
    int32_t *d_hash_keys, float *d_hash_vals)
{
    int32_t row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) return;
    int32_t *my_keys = d_hash_keys + row * hash_cap;
    float   *my_vals = d_hash_vals + row * hash_cap;
    for (int32_t i = 0; i < hash_cap; i++) my_keys[i] = HASH_EMPTY;

    for (int32_t p = Ap[row]; p < Ap[row+1]; p++) {
        int32_t b_row = Aj[p]; float a_val = Av[p];
        for (int32_t q = Bp[b_row]; q < Bp[b_row+1]; q++) {
            int32_t col = Bj[q]; float b_val = Bv[q];
            uint32_t idx = ((uint32_t)col * 2654435761u) % (uint32_t)hash_cap;
            while (my_keys[idx] != HASH_EMPTY && my_keys[idx] != col) idx = (idx+1) % hash_cap;
            if (my_keys[idx] == HASH_EMPTY) { my_keys[idx] = col; my_vals[idx] = a_val*b_val; }
            else my_vals[idx] += a_val*b_val;
        }
    }

    int32_t write_offset = Cp[row], extracted = 0;
    for (int32_t i = 0; i < hash_cap; i++)
        if (my_keys[i] != HASH_EMPTY) {
            Cj[write_offset + extracted] = my_keys[i];
            Cv[write_offset + extracted] = my_vals[i];
            extracted++;
        }

    /* in-place insertion sort by column */
    for (int32_t i = 1; i < extracted; i++) {
        int32_t k_col = Cj[write_offset+i]; float k_val = Cv[write_offset+i]; int32_t j = i-1;
        while (j >= 0 && Cj[write_offset+j] > k_col) {
            Cj[write_offset+j+1] = Cj[write_offset+j];
            Cv[write_offset+j+1] = Cv[write_offset+j];
            j--;
        }
        Cj[write_offset+j+1] = k_col; Cv[write_offset+j+1] = k_val;
    }
}

extern "C" csr_matrix spgemm_gustavson_hash_cuda(
    const csr_matrix *A, const csr_matrix *B, int32_t est_max_row_nnz)
{
    int32_t hash_cap = est_max_row_nnz * 2;
    int32_t threads = 256, blocks = (A->rows + threads - 1) / threads;

    int32_t *d_hash_keys; float *d_hash_vals;
    CUDA_CHECK(cudaMalloc(&d_hash_keys, (size_t)A->rows * hash_cap * sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&d_hash_vals, (size_t)A->rows * hash_cap * sizeof(float)));

    int32_t *d_c_row_nnz;
    CUDA_CHECK(cudaMalloc(&d_c_row_nnz, A->rows * sizeof(int32_t)));

    spgemm_hash_symbolic_kernel<<<blocks, threads>>>(
        A->rows, hash_cap,
        (int32_t*)A->row_ptr.ptr, (int32_t*)A->col_idx.ptr,
        (int32_t*)B->row_ptr.ptr, (int32_t*)B->col_idx.ptr,
        d_c_row_nnz, d_hash_keys);
    CUDA_CHECK(cudaDeviceSynchronize());

    int32_t *h_c_row_nnz = (int32_t*)malloc(A->rows * sizeof(int32_t));
    CUDA_CHECK(cudaMemcpy(h_c_row_nnz, d_c_row_nnz, A->rows*sizeof(int32_t), cudaMemcpyDeviceToHost));

    int32_t *h_Cp = (int32_t*)malloc((A->rows+1)*sizeof(int32_t));
    h_Cp[0] = 0;
    for (int32_t i = 0; i < A->rows; i++) h_Cp[i+1] = h_Cp[i] + h_c_row_nnz[i];
    int32_t total_nnz = h_Cp[A->rows];

    extern csr_matrix csr_create(int32_t, int32_t, int32_t, int);
    csr_matrix C = csr_create(A->rows, B->cols, total_nnz, 1 /* MEM_DEVICE */);
    CUDA_CHECK(cudaMemcpy(C.row_ptr.ptr, h_Cp, (A->rows+1)*sizeof(int32_t), cudaMemcpyHostToDevice));

    spgemm_hash_numeric_kernel<<<blocks, threads>>>(
        A->rows, hash_cap,
        (int32_t*)A->row_ptr.ptr, (int32_t*)A->col_idx.ptr, (float*)A->values.ptr,
        (int32_t*)B->row_ptr.ptr, (int32_t*)B->col_idx.ptr, (float*)B->values.ptr,
        (int32_t*)C.row_ptr.ptr, (int32_t*)C.col_idx.ptr, (float*)C.values.ptr,
        d_hash_keys, d_hash_vals);
    CUDA_CHECK(cudaDeviceSynchronize());

    CUDA_CHECK(cudaFree(d_hash_keys));
    CUDA_CHECK(cudaFree(d_hash_vals));
    CUDA_CHECK(cudaFree(d_c_row_nnz));
    free(h_c_row_nnz); free(h_Cp);
    return C;
}

#endif /* __CUDACC__ */
