/* ============================================================
 * WRITE BARRIER + BULK become: + INCREMENTAL SCAVENGE
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward decls from gc.c */
typedef enum { SPACE_NURSERY, SPACE_SURVIVOR, SPACE_OLD } SpaceTag;
typedef struct Object Object;
extern void card_mark(void *addr);
extern void cache_flush(void);
extern size_t object_size(Object *o);
typedef struct Selector Selector;
typedef struct Slot { Selector *name; Object *value; bool isParent; } Slot;

/* ------------------------------------------------------------------ */
/* Write barrier on Self slot stores */
/* ------------------------------------------------------------------ */
void self_slot_at_put(Object *obj, uint32_t index, Object *value) {
    Object **slots = (Object **)obj->slots;
    slots[index] = value;

    /* generational write barrier */
    extern Space old_space;
    extern Space nursery;
    typedef struct { uint8_t *start; uint8_t *end; uint8_t *free; SpaceTag tag; } Space;
    /* simplified check */
    (void)obj; (void)value;
    card_mark(obj);
}

/* ------------------------------------------------------------------ */
/* Bulk array become: */
/* ------------------------------------------------------------------ */
extern void become_update_all_pointers(Object *old, Object *newObj);

void become_update_all_pointers_bulk(Object **A, Object **B, uint32_t n);

void become_arrays(Object **arrayA, Object **arrayB, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        Object *a = arrayA[i];
        Object *b = arrayB[i];
        if (a == b) continue;

        uint32_t h = a->identityHash;
        a->identityHash = b->identityHash;
        b->identityHash = h;

        a->forwarding = b; b->forwarding = a;
        a->flags |= 4; /* FLAG_FORWARDED */
        b->flags |= 4;
    }

    become_update_all_pointers_bulk(arrayA, arrayB, count);
    cache_flush();
    printf("bulk become: %u pairs exchanged\n", count);
}

/* ------------------------------------------------------------------ */
/* Incremental Scavenge */
/* ------------------------------------------------------------------ */
typedef enum {
    SC_IDLE, SC_ROOTS, SC_CHENEY, SC_CARDS, SC_PROMOTE, SC_DONE
} ScavengePhase;

ScavengePhase sc_phase = SC_IDLE;
uint8_t *sc_scan = NULL;
uint32_t sc_work_quantum = 64;

bool scavenge_is_running(void) { return sc_phase != SC_IDLE && sc_phase != SC_DONE; }

/* forward decls */
extern Object **root_set[];
extern uint32_t root_count;
typedef struct { uint8_t *start; uint8_t *end; uint8_t *free; SpaceTag tag; } Space;
extern Space nursery, survivor_from, survivor_to, old_space;
extern Object *copy_object(Object *old, Space *to);
extern void scavenge_object_fields(Object *o, Space *to);
extern void scan_dirty_cards(Space *to);

void scavenge_begin(void) {
    if (sc_phase != SC_IDLE) return;
    Space tmp = survivor_from; survivor_from = survivor_to; survivor_to = tmp;
    survivor_to.free = survivor_to.start;
    sc_scan = survivor_to.start;
    sc_phase = SC_ROOTS;
    printf("--- incremental scavenge begin ---\n");
}

void scavenge_step(void) {
    if (sc_phase == SC_IDLE || sc_phase == SC_DONE) return;
    uint32_t work = 0;

    switch (sc_phase) {
    case SC_ROOTS:
        for (uint32_t i = 0; i < root_count && work < sc_work_quantum; i++, work++) {
            Object *r = *root_set[i];
            if (r && r->space == SPACE_NURSERY)
                *root_set[i] = copy_object(r, &survivor_to);
        }
        if (work < sc_work_quantum) { sc_phase = SC_CHENEY; sc_scan = survivor_to.start; }
        break;

    case SC_CHENEY:
        while (sc_scan < survivor_to.free && work < sc_work_quantum) {
            Object *o = (Object *)sc_scan;
            scavenge_object_fields(o, &survivor_to);
            sc_scan += object_size(o);
            work++;
        }
        if (sc_scan >= survivor_to.free) sc_phase = SC_CARDS;
        break;

    case SC_CARDS:
        scan_dirty_cards(&survivor_to);
        sc_phase = SC_PROMOTE;
        break;

    case SC_PROMOTE: {
        uint8_t *p = survivor_to.start;
        while (p < survivor_to.free && work < sc_work_quantum) {
            Object *o = (Object *)p;
            if (o->age >= 3 && !(o->flags & 8)) copy_object(o, &old_space);
            p += object_size(o);
            work++;
        }
        if (p >= survivor_to.free) {
            nursery.free = nursery.start;
            sc_phase = SC_DONE;
            cache_flush();
            printf("--- incremental scavenge done ---\n");
        }
        break;
    }
    default: break;
    }
}

void scavenge_to_completion(void) {
    scavenge_begin();
    while (sc_phase != SC_DONE) scavenge_step();
    sc_phase = SC_IDLE;
}
