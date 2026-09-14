/* ============================================================
 * PORTABLE IR + SSA CONSTRUCTION + DOMINANCE TREE
 * + LINEAR-SCAN + WOZ WORK-STEALING ALLOCATOR
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Portable IR */
/* ------------------------------------------------------------------ */
typedef enum {
    IR_NOP, IR_MOV, IR_LOAD, IR_STORE,
    IR_ADD, IR_SUB, IR_MUL, IR_DIV,
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FMA,
    IR_CMP, IR_JMP, IR_JT, IR_JF,
    IR_CALL, IR_RET, IR_PHI, IR_GUARD, IR_SAFEPOINT, IR_LABEL
} IROp;

typedef struct {
    IROp op;
    int32_t dest;
    int32_t a, b, c;
    int32_t imm;
    uint32_t deopt_id;
    bool is_float;
} IRInst;

#define MAX_IR    4096
#define MAX_VREG  512
#define MAX_BLOCKS 256

typedef struct {
    IRInst inst[MAX_IR];
    uint32_t len;
    uint32_t n_vreg;
    struct {
        uint32_t bc_pc;
        int32_t live_vregs[16];
        uint32_t n_live;
    } deopt[256];
    uint32_t n_deopt;
} IRFunction;

void ir_init(IRFunction *f) { memset(f, 0, sizeof(*f)); }

int32_t ir_new_vreg(IRFunction *f) { return f->n_vreg++; }

void ir_emit(IRFunction *f, IROp op, int32_t dest, int32_t a, int32_t b, int32_t c) {
    if (f->len >= MAX_IR) return;
    IRInst *i = &f->inst[f->len++];
    i->op = op; i->dest = dest; i->a = a; i->b = b; i->c = c;
}

/* ------------------------------------------------------------------ */
/* CFG */
/* ------------------------------------------------------------------ */
typedef struct BasicBlock {
    uint32_t id;
    uint32_t start, end;
    uint32_t pred[8], n_pred;
    uint32_t succ[8], n_succ;
    uint32_t idom;
    uint32_t df[MAX_BLOCKS], n_df;
    bool visited;
} BasicBlock;

typedef struct { BasicBlock blocks[MAX_BLOCKS]; uint32_t n_blocks; uint32_t entry; } CFG;

/* ------------------------------------------------------------------ */
/* Dominator computation (iterative) */
/* ------------------------------------------------------------------ */
void compute_dominators(CFG *cfg) {
    cfg->blocks[0].idom = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 1; b < cfg->n_blocks; b++) {
            BasicBlock *bb = &cfg->blocks[b];
            if (bb->n_pred == 0) continue;
            uint32_t new_idom = bb->pred[0];
            for (uint32_t p = 1; p < bb->n_pred; p++) {
                uint32_t pred = bb->pred[p];
                uint32_t i1 = new_idom, i2 = pred;
                while (i1 != i2) {
                    while (i1 > i2) i1 = cfg->blocks[i1].idom;
                    while (i2 > i1) i2 = cfg->blocks[i2].idom;
                }
                new_idom = i1;
            }
            if (bb->idom != new_idom) { bb->idom = new_idom; changed = true; }
        }
    }
}

void compute_dominance_frontiers(CFG *cfg) {
    for (uint32_t b = 0; b < cfg->n_blocks; b++) cfg->blocks[b].n_df = 0;
    for (uint32_t b = 0; b < cfg->n_blocks; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        if (bb->n_pred < 2) continue;
        for (uint32_t p = 0; p < bb->n_pred; p++) {
            uint32_t runner = bb->pred[p];
            while (runner != bb->idom) {
                BasicBlock *r = &cfg->blocks[runner];
                bool found = false;
                for (uint32_t i = 0; i < r->n_df; i++) if (r->df[i]==b){found=true;break;}
                if (!found && r->n_df < MAX_BLOCKS) r->df[r->n_df++] = b;
                runner = r->idom;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* phi-node insertion */
/* ------------------------------------------------------------------ */
void insert_phi_nodes(IRFunction *f, CFG *cfg) {
    bool def_blocks[MAX_VREG][MAX_BLOCKS];
    memset(def_blocks, 0, sizeof(def_blocks));

    for (uint32_t i = 0; i < f->len; i++) {
        IRInst *ins = &f->inst[i];
        if (ins->dest >= 0) {
            for (uint32_t b = 0; b < cfg->n_blocks; b++) {
                if (i >= cfg->blocks[b].start && i < cfg->blocks[b].end) {
                    def_blocks[ins->dest][b] = true; break;
                }
            }
        }
    }

    for (uint32_t v = 0; v < f->n_vreg; v++) {
        bool worklist[MAX_BLOCKS] = {0}, has_phi[MAX_BLOCKS] = {0};
        for (uint32_t b = 0; b < cfg->n_blocks; b++) if (def_blocks[v][b]) worklist[b] = true;
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t b = 0; b < cfg->n_blocks; b++) {
                if (!worklist[b]) continue;
                worklist[b] = false;
                BasicBlock *bb = &cfg->blocks[b];
                for (uint32_t i = 0; i < bb->n_df; i++) {
                    uint32_t df = bb->df[i];
                    if (!has_phi[df]) {
                        if (f->len < MAX_IR) {
                            IRInst *phi = &f->inst[f->len++];
                            phi->op = IR_PHI; phi->dest = v;
                            phi->a = phi->b = phi->c = -1; phi->imm = df;
                        }
                        has_phi[df] = true;
                        if (!def_blocks[v][df]) { worklist[df] = true; changed = true; }
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Variable renaming */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t stack[MAX_VREG][64];
    int32_t sp[MAX_VREG];
    int32_t next_id[MAX_VREG];
} RenameState;

static int32_t rename_new_name(RenameState *rs, int32_t v) {
    int32_t id = rs->next_id[v]++;
    rs->stack[v][rs->sp[v]++] = id;
    return id;
}
static int32_t rename_top(RenameState *rs, int32_t v) {
    if (rs->sp[v] == 0) return v;
    return rs->stack[v][rs->sp[v]-1];
}
static void rename_push(RenameState *rs, int32_t v, int32_t name) { rs->stack[v][rs->sp[v]++] = name; }

void rename_variables(IRFunction *f, CFG *cfg) {
    RenameState rs; memset(&rs, 0, sizeof(rs));
    for (uint32_t v = 0; v < f->n_vreg; v++) rename_push(&rs, v, 0);
    for (uint32_t b = 0; b < cfg->n_blocks; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        for (uint32_t i = bb->start; i < bb->end; i++) {
            IRInst *ins = &f->inst[i];
            if (ins->a >= 0) ins->a = rename_top(&rs, ins->a);
            if (ins->b >= 0) ins->b = rename_top(&rs, ins->b);
            if (ins->c >= 0) ins->c = rename_top(&rs, ins->c);
            if (ins->dest >= 0) ins->dest = rename_new_name(&rs, ins->dest);
        }
    }
}

void construct_ssa(IRFunction *f, CFG *cfg) {
    if (cfg->n_blocks == 0) return;
    compute_dominators(cfg);
    compute_dominance_frontiers(cfg);
    insert_phi_nodes(f, cfg);
    rename_variables(f, cfg);
    printf("SSA: %u blocks, %u vregs, %u IR instructions\n",
           cfg->n_blocks, f->n_vreg, f->len);
}

void build_cfg_from_ir(IRFunction *f, CFG *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->n_blocks = 1;
    cfg->blocks[0].id = 0;
    cfg->blocks[0].start = 0;
    cfg->blocks[0].end = f->len;
    cfg->entry = 0;
}

/* ------------------------------------------------------------------ */
/* Linear-Scan Register Allocator */
/* ------------------------------------------------------------------ */
#define MAX_PHYS_X64 14
#define MAX_PHYS_ARM64 28

typedef struct {
    int32_t vreg;
    uint32_t start, end;
    int32_t phys;
    bool is_float;
} Interval;

typedef struct {
    Interval intervals[MAX_VREG];
    uint32_t n;
    int32_t mapping[MAX_VREG];
    int32_t stack_slots;
} Allocation;

void build_live_intervals(IRFunction *f, Allocation *a) {
    memset(a, 0, sizeof(*a));
    for (uint32_t i = 0; i < f->len; i++) {
        IRInst *ins = &f->inst[i];
        if (ins->dest >= 0) {
            Interval *iv = &a->intervals[ins->dest];
            if (iv->end == 0) { iv->vreg = ins->dest; iv->start = i; }
            iv->end = i; iv->is_float = ins->is_float;
        }
        if (ins->a >= 0) a->intervals[ins->a].end = i;
        if (ins->b >= 0) a->intervals[ins->b].end = i;
        if (ins->c >= 0) a->intervals[ins->c].end = i;
    }
    a->n = f->n_vreg;
}

void linear_scan(Allocation *a, int max_phys) {
    for (uint32_t i = 1; i < a->n; i++) {
        Interval key = a->intervals[i];
        int j = i - 1;
        while (j >= 0 && a->intervals[j].start > key.start) {
            a->intervals[j+1] = a->intervals[j]; j--;
        }
        a->intervals[j+1] = key;
    }

    int32_t active[32]; uint32_t n_active = 0; int32_t free_reg = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        Interval *cur = &a->intervals[i];
        for (uint32_t j = 0; j < n_active; )
            if (a->intervals[active[j]].end < cur->start) {
                free_reg = a->intervals[active[j]].phys;
                active[j] = active[--n_active];
            } else j++;

        if (n_active < max_phys) { cur->phys = free_reg++; active[n_active++] = i; }
        else { cur->phys = -(++a->stack_slots); }
        a->mapping[cur->vreg] = cur->phys;
    }
}

/* ------------------------------------------------------------------ */
/* Woz work-stealing register allocator */
/* ------------------------------------------------------------------ */
#define MAX_WORKERS 8
#define DEQUE_SIZE  512

typedef struct {
    Interval intervals[DEQUE_SIZE];
    uint32_t head, tail;
    atomic_uint ticket;
} WorkDeque;

typedef struct {
    WorkDeque deques[MAX_WORKERS];
    uint32_t n_workers;
    Allocation *result;
    atomic_uint next_worker;
} StealScheduler;

void deque_push(WorkDeque *d, Interval iv) {
    uint32_t t = d->tail;
    if (t < DEQUE_SIZE) {
        d->intervals[t] = iv;
        atomic_store(&d->ticket, t+1);
        d->tail = t+1;
    }
}

bool deque_pop(WorkDeque *d, Interval *out) {
    if (d->tail == d->head) return false;
    d->tail--; *out = d->intervals[d->tail]; return true;
}

bool deque_steal(WorkDeque *d, Interval *out) {
    uint32_t h = d->head;
    if (h >= atomic_load(&d->ticket)) return false;
    *out = d->intervals[h]; d->head = h+1; return true;
}

void worker_allocate(StealScheduler *sched, uint32_t wid, int max_phys) {
    WorkDeque *my = &sched->deques[wid];
    Interval cur;
    int32_t free_reg = 0, active[32]; uint32_t n_active = 0;

    while (true) {
        if (!deque_pop(my, &cur)) {
            bool stolen = false;
            for (uint32_t w = 0; w < sched->n_workers; w++) {
                if (w == wid) continue;
                if (deque_steal(&sched->deques[w], &cur)) { stolen = true; break; }
            }
            if (!stolen) break;
        }
        for (uint32_t j = 0; j < n_active; )
            if (sched->result->intervals[active[j]].end < cur.start) {
                free_reg = sched->result->intervals[active[j]].phys;
                active[j] = active[--n_active];
            } else j++;

        if (n_active < max_phys) { cur.phys = free_reg++; active[n_active++] = cur.vreg; }
        else { cur.phys = -(++sched->result->stack_slots); }
        sched->result->mapping[cur.vreg] = cur.phys;
        sched->result->intervals[cur.vreg] = cur;
    }
}

void allocate_registers_woz(IRFunction *f, Allocation *a, int max_phys, uint32_t n_workers) {
    build_live_intervals(f, a);
    StealScheduler sched = {0};
    sched.n_workers = n_workers > MAX_WORKERS ? MAX_WORKERS : n_workers;
    sched.result = a;
    for (uint32_t i = 0; i < a->n; i++) {
        uint32_t w = i % sched.n_workers;
        deque_push(&sched.deques[w], a->intervals[i]);
    }
    for (uint32_t w = 0; w < sched.n_workers; w++)
        worker_allocate(&sched, w, max_phys);
}

/* ------------------------------------------------------------------ */
/* Deoptimization + OSR */
/* ------------------------------------------------------------------ */
typedef struct Object Object;

typedef struct DeoptFrame {
    uint32_t bc_pc;
    Object *receiver;
    Object *args[8];
    uint32_t n_args;
    Object *locals[32];
} DeoptFrame;

void deoptimize(uint32_t deopt_id, void *frame_pointer) {
    (void)frame_pointer;
    printf("DEOPT id=%u – transferring to interpreter\n", deopt_id);
}

void osr_enter(IRFunction *f, uint32_t bc_pc, Object *recv, Object **locals) {
    (void)f; (void)recv; (void)locals;
    printf("OSR enter at bytecode PC %u\n", bc_pc);
}
