/* ============================================================
 * x86-64 JIT BACK-END + IC STUBS + ESCAPE ANALYSIS
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 * ============================================================ */

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ---------- Executable memory allocator ---------- */
typedef struct ExecPage {
    uint8_t *base; size_t size; size_t used; struct ExecPage *next;
} ExecPage;
ExecPage *exec_pages = NULL;

uint8_t *exec_alloc(size_t sz) {
    sz = (sz + 15) & ~15;
    if (!exec_pages || exec_pages->used + sz > exec_pages->size) {
        size_t page = 16 * 1024;
        void *p = mmap(NULL, page, PROT_READ|PROT_WRITE|PROT_EXEC,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        ExecPage *ep = calloc(1, sizeof(ExecPage));
        ep->base = p; ep->size = page; ep->used = 0; ep->next = exec_pages;
        exec_pages = ep;
    }
    uint8_t *r = exec_pages->base + exec_pages->used;
    exec_pages->used += sz;
    return r;
}

/* ---------- Tiny x86-64 assembler ---------- */
typedef struct { uint8_t *code; size_t cap; size_t len; } Asm;

void asm_init(Asm *a, size_t cap) { a->code = exec_alloc(cap); a->cap = cap; a->len = 0; }

static void emit(Asm *a, uint8_t b) { if (a->len < a->cap) a->code[a->len++] = b; }
static void emit32(Asm *a, uint32_t v) { emit(a,v); emit(a,v>>8); emit(a,v>>16); emit(a,v>>24); }
static void emit64(Asm *a, uint64_t v) { emit32(a,(uint32_t)v); emit32(a,(uint32_t)(v>>32)); }

#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
#define R8  8
#define R9  9
#define R10 10
#define R11 11

static void rex(Asm *a, int w, int r, int x, int b) { emit(a,0x40|(w<<3)|(r<<2)|(x<<1)|b); }

static void mov_reg_imm64(Asm *a, int reg, uint64_t imm) {
    rex(a,1,0,0,reg>=8); emit(a,0xB8+(reg&7)); emit64(a,imm);
}

static void mov_reg_mem(Asm *a, int dst, int base, int32_t disp) {
    rex(a,1,dst>=8,0,base>=8); emit(a,0x8B);
    if (disp==0 && (base&7)!=RBP) { emit(a,((dst&7)<<3)|(base&7)); }
    else { emit(a,0x80|((dst&7)<<3)|(base&7)); emit32(a,disp); }
}

static void cmp_reg_reg(Asm *a, int left, int right) {
    rex(a,1,right>=8,0,left>=8); emit(a,0x39); emit(a,0xC0|((right&7)<<3)|(left&7));
}

static void je_rel32(Asm *a, int32_t rel) { emit(a,0x0F); emit(a,0x84); emit32(a,rel); }

static void call_reg(Asm *a, int reg) { if(reg>=8)emit(a,0x41); emit(a,0xFF); emit(a,0xD0|(reg&7)); }
static void ret_ins(Asm *a) { emit(a,0xC3); }
static void push_reg(Asm *a, int reg) { if(reg>=8)emit(a,0x41); emit(a,0x50+(reg&7)); }
static void pop_reg(Asm *a, int reg)  { if(reg>=8)emit(a,0x41); emit(a,0x58+(reg&7)); }

/* ---------- IC Stub types ---------- */
typedef void *(*IMP)(void *self, void *sel, ...);

typedef struct ICStub {
    void *cached_map;
    IMP cached_imp;
    uint8_t *code;
    uint32_t code_size;
    struct ICStub *next;
} ICStub;

ICStub *ic_list = NULL;

typedef struct X64ICStub {
    ICStub base;
    uint8_t *code;
    size_t code_len;
    uint32_t patch_cached_map;
    uint32_t patch_cached_imp;
} X64ICStub;

void *ic_miss_handler(void *recv, void *sel, X64ICStub *stub);

X64ICStub *jit_emit_ic_stub_x64(void *sel) {
    Asm a; asm_init(&a, 128);
    X64ICStub *stub = calloc(1, sizeof(X64ICStub));
    stub->base.cached_map = NULL; stub->base.cached_imp = NULL;

    mov_reg_mem(&a, RAX, RDI, 0); /* load isa_or_map */
    stub->patch_cached_map = a.len + 2;
    mov_reg_imm64(&a, R10, 0);
    cmp_reg_reg(&a, RAX, R10);
    size_t je_pos = a.len;
    je_rel32(&a, 0);

    /* miss path */
    mov_reg_imm64(&a, RAX, (uint64_t)(uintptr_t)ic_miss_handler);
    mov_reg_imm64(&a, RCX, (uint64_t)(uintptr_t)stub);
    call_reg(&a, RAX);
    ret_ins(&a);

    /* hit path */
    size_t hit_pos = a.len;
    int32_t rel = (int32_t)(hit_pos - (je_pos + 6));
    a.code[je_pos+2] = rel; a.code[je_pos+3] = rel>>8;
    a.code[je_pos+4] = rel>>16; a.code[je_pos+5] = rel>>24;

    stub->patch_cached_imp = a.len + 2;
    mov_reg_imm64(&a, RAX, 0);
    emit(&a, 0xFF); emit(&a, 0xE0); /* jmp rax */

    stub->code = a.code; stub->code_len = a.len;
    stub->base.code = a.code;
    stub->base.next = ic_list;
    ic_list = &stub->base;
    return stub;
}

void *ic_miss_handler(void *recv, void *sel, X64ICStub *stub) {
    /* simplified: no actual method lookup in this standalone file */
    (void)recv; (void)sel; (void)stub;
    return NULL;
}

void ic_stubs_after_scavenge(void) {
    for (ICStub *s = ic_list; s; s = s->next) {
        X64ICStub *xs = (X64ICStub *)s;
        if (s->cached_map) {
            /* follow forwarding pointer */
            uint64_t *p = (uint64_t *)(xs->code + xs->patch_cached_map);
            (void)p;
        }
    }
}

/* ---------- Float FMA method compiler ---------- */
typedef void *(*NativeEntry)(void *, void *, ...);

NativeEntry jit_compile_float_fma(void) {
    Asm a; asm_init(&a, 256);

    push_reg(&a, RBP);
    emit(&a,0x48); emit(&a,0x89); emit(&a,0xE5); /* mov rbp, rsp */

    /* movsd xmm0, [rdi+16] */
    emit(&a,0xF2); emit(&a,0x0F); emit(&a,0x10); emit(&a,0x47); emit(&a,16);
    /* movsd xmm1, [rdi+24] */
    emit(&a,0xF2); emit(&a,0x0F); emit(&a,0x10); emit(&a,0x4F); emit(&a,24);
    /* movsd xmm2, [rdi+32] */
    emit(&a,0xF2); emit(&a,0x0F); emit(&a,0x10); emit(&a,0x57); emit(&a,32);
    /* mulsd xmm0, xmm1 */
    emit(&a,0xF2); emit(&a,0x0F); emit(&a,0x59); emit(&a,0xC1);
    /* addsd xmm0, xmm2 */
    emit(&a,0xF2); emit(&a,0x0F); emit(&a,0x58); emit(&a,0xC2);
    /* mov rax, rdi */
    emit(&a,0x48); emit(&a,0x89); emit(&a,0xF8);
    emit(&a,0x5D); /* pop rbp */
    ret_ins(&a);

    return (NativeEntry)a.code;
}

void jit_init_x64(void) {
    printf("x86-64 JIT back-end ready\n");
}

/* ---------- Stack frame / escape analysis ---------- */
#define FRAME_STACK_SIZE 4096
uint8_t frame_stack[FRAME_STACK_SIZE];
uint8_t *frame_sp = frame_stack;

typedef struct { uint8_t *base; uint8_t *top; } StackFrame;
StackFrame current_frame;

void frame_enter(void) { current_frame.base = frame_sp; current_frame.top = frame_sp; }
void frame_leave(void) { frame_sp = current_frame.base; }
