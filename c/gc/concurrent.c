/* ============================================================
 * CARD-MARKING REMEMBERED SET + CONCURRENT MARKING
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define OLD_SIZE      (2 * 1024 * 1024)
#define CARD_SIZE     512
#define CARDS_PER_OLD (OLD_SIZE / CARD_SIZE)
#define MARK_STACK_SIZE 8192

typedef enum { SPACE_NURSERY, SPACE_SURVIVOR, SPACE_OLD } SpaceTag;
typedef struct { uint8_t *start; uint8_t *end; uint8_t *free; SpaceTag tag; } Space;
typedef struct Object Object;

extern Space old_space, nursery, survivor_from, survivor_to;
extern size_t object_size(Object *o);
extern Object *copy_object(Object *old, Space *to);

/* ------------------------------------------------------------------ */
/* Card table */
/* ------------------------------------------------------------------ */
uint8_t card_table[CARDS_PER_OLD];

static inline uint32_t addr_to_card(void *p) {
    uintptr_t off = (uintptr_t)p - (uintptr_t)old_space.start;
    return (uint32_t)(off / CARD_SIZE);
}

void card_mark(void *old_obj_addr) {
    if ((uint8_t*)old_obj_addr >= old_space.start &&
        (uint8_t*)old_obj_addr < old_space.end) {
        uint32_t c = addr_to_card(old_obj_addr);
        card_table[c] = 1;
    }
}

void card_table_clear(void) { memset(card_table, 0, sizeof(card_table)); }

void scan_dirty_cards(Space *to) {
    for (uint32_t c = 0; c < CARDS_PER_OLD; c++) {
        if (!card_table[c]) continue;

        uint8_t *card_start = old_space.start + c * CARD_SIZE;
        uint8_t *card_end = card_start + CARD_SIZE;
        if (card_end > old_space.free) card_end = old_space.free;

        uint8_t *p = card_start;
        while (p < card_end) {
            Object *o = (Object *)p;
            /* examine fields for young pointers */
            extern void scan_object_for_young(Object *o, Space *to);
            scan_object_for_young(o, to);
            p += object_size(o);
        }
        card_table[c] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Concurrent Marking for Old Space */
/* ------------------------------------------------------------------ */
typedef enum { WHITE = 0, GREY = 1, BLACK = 2 } Colour;

Object *mark_stack[MARK_STACK_SIZE];
uint32_t mark_sp = 0;

volatile bool marking_active = false;
volatile bool mutator_handshake = false;

struct Object {
    struct Object *isa_or_map;
    uint32_t flags;
    uint32_t identityHash;
    uint32_t slotCount;
    void *slots;
    struct Object *forwarding;
    SpaceTag space;
    uint32_t age;
};

#define FLAG_MARKED (1u << 1)

static inline Object *follow_fwd(Object *o) {
    while (o && (o->flags & (1u<<2)) && o->forwarding) o = o->forwarding;
    return o;
}

void mark_push(Object *o) {
    if (!o || (o->flags & FLAG_MARKED)) return;
    o->flags |= FLAG_MARKED;
    if (mark_sp < MARK_STACK_SIZE) mark_stack[mark_sp++] = o;
}

Object *mark_pop(void) {
    return mark_sp ? mark_stack[--mark_sp] : NULL;
}

void mark_object_fields(Object *o) {
    if (o->isa_or_map) mark_push(follow_fwd(o->isa_or_map));
    if (o->slots) {
        Object **s = (Object **)o->slots;
        for (uint32_t i = 0; i < o->slotCount; i++)
            if (s[i]) mark_push(follow_fwd(s[i]));
    }
}

void concurrent_mark_step(uint32_t quantum) {
    uint32_t work = 0;
    while (work < quantum && mark_sp > 0) {
        Object *o = mark_pop();
        if (!o) break;
        mark_object_fields(o);
        work++;
    }
}

extern Object **root_set[];
extern uint32_t root_count;

void concurrent_mark_begin(void) {
    mutator_handshake = true;
    mark_sp = 0;
    for (uint32_t i = 0; i < root_count; i++)
        mark_push(*root_set[i]);
    marking_active = true;
    mutator_handshake = false;
    printf("--- concurrent old-space mark begin ---\n");
}

void concurrent_mark_end(void) {
    while (mark_sp > 0) concurrent_mark_step(1024);
    mutator_handshake = true;
    marking_active = false;
    mutator_handshake = false;
    printf("--- concurrent old-space mark end ---\n");
}
