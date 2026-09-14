/* ============================================================
 * REAL become: + SCAVENGING GENERATIONAL GC
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define NURSERY_SIZE  (256 * 1024)
#define SURVIVOR_SIZE (128 * 1024)
#define OLD_SIZE      (2 * 1024 * 1024)

typedef enum { SPACE_NURSERY, SPACE_SURVIVOR, SPACE_OLD } SpaceTag;

typedef struct {
    uint8_t *start;
    uint8_t *end;
    uint8_t *free;
    SpaceTag tag;
} Space;

Space nursery, survivor_from, survivor_to, old_space;

void space_init(Space *s, size_t size, SpaceTag tag) {
    s->start = aligned_alloc(16, size);
    s->end   = s->start + size;
    s->free  = s->start;
    s->tag   = tag;
    memset(s->start, 0, size);
}

void spaces_bootstrap(void) {
    space_init(&nursery,       NURSERY_SIZE,  SPACE_NURSERY);
    space_init(&survivor_from, SURVIVOR_SIZE, SPACE_SURVIVOR);
    space_init(&survivor_to,   SURVIVOR_SIZE, SPACE_SURVIVOR);
    space_init(&old_space,     OLD_SIZE,      SPACE_OLD);
    printf("Scavenger spaces ready: nursery %zu KB, survivors %zu KB, old %zu KB\n",
           NURSERY_SIZE/1024, SURVIVOR_SIZE/1024, OLD_SIZE/1024);
}

/* Object header (must match runtime.c) */
typedef struct Object {
    struct Object *isa_or_map;
    uint32_t flags;
    uint32_t identityHash;
    uint32_t slotCount;
    void *slots;
    struct Object *forwarding;
    SpaceTag space;
    uint32_t age;
} Object;

#define FLAG_FORWARDED (1u << 2)
#define FLAG_PINNED    (1u << 3)

extern void cache_flush(void);
extern void card_mark(void *addr);
extern size_t object_size(Object *o);

static inline Object *follow(Object *o) {
    while (o && (o->flags & FLAG_FORWARDED) && o->forwarding)
        o = o->forwarding;
    return o;
}

/* ------------------------------------------------------------------ */
/* Allocation (bump in nursery) */
/* ------------------------------------------------------------------ */
extern void scavenge(void); /* forward */

Object *gc_alloc(size_t size, SpaceTag preferred) {
    size = (size + 15) & ~15;
    Space *target = &nursery;
    if (preferred == SPACE_OLD || size > NURSERY_SIZE/4) target = &old_space;

    if (target->free + size > target->end) {
        if (target == &nursery) {
            scavenge();
            if (nursery.free + size > nursery.end) target = &old_space;
        }
        if (target->free + size > target->end) {
            fprintf(stderr, "Out of memory in space %d\n", target->tag);
            abort();
        }
    }

    Object *o = (Object *)target->free;
    target->free += size;
    memset(o, 0, size);
    o->space = target->tag;
    o->forwarding = NULL;
    o->age = 0;
    return o;
}

/* ------------------------------------------------------------------ */
/* become: (two-way) */
/* ------------------------------------------------------------------ */
void become_update_all_pointers(Object *old, Object *newObj);

void become(Object *a, Object *b) {
    if (a == b) return;
    a = follow(a); b = follow(b);
    if (a == b) return;

    uint32_t tmpHash = a->identityHash;
    a->identityHash = b->identityHash;
    b->identityHash = tmpHash;

    a->forwarding = b; b->forwarding = a;
    a->flags |= FLAG_FORWARDED; b->flags |= FLAG_FORWARDED;

    become_update_all_pointers(a, b);
    printf("become: %p <-> %p completed\n", (void*)a, (void*)b);
}

void becomeForward(Object *a, Object *b) {
    a = follow(a); b = follow(b);
    if (a == b) return;
    a->forwarding = b;
    a->flags |= FLAG_FORWARDED;
    become_update_all_pointers(a, b);
    printf("becomeForward: %p -> %p\n", (void*)a, (void*)b);
}

void become_update_all_pointers(Object *old, Object *newObj) {
    extern Object *lobby;
    if (lobby == old) lobby = newObj;

    Space *spaces[] = {&nursery, &survivor_from, &survivor_to, &old_space};
    for (int s = 0; s < 4; s++) {
        uint8_t *p = spaces[s]->start;
        while (p < spaces[s]->free) {
            Object *o = (Object *)p;
            size_t sz = object_size(o);
            if (!(o->flags & FLAG_FORWARDED)) {
                if (o->isa_or_map == old) o->isa_or_map = newObj;
                if (o->slots) {
                    Object **slotptrs = (Object **)o->slots;
                    for (uint32_t i = 0; i < o->slotCount; i++)
                        if (slotptrs[i] == old) slotptrs[i] = newObj;
                }
            }
            p += sz;
        }
    }
    cache_flush();
}

/* ------------------------------------------------------------------ */
/* Scavenging GC (Cheney-style) */
/* ------------------------------------------------------------------ */
Object **root_set[1024];
uint32_t root_count = 0;

void add_root(Object **root) {
    if (root_count < 1024) root_set[root_count++] = root;
}

static Object *copy_object(Object *old, Space *to) {
    if (old->flags & FLAG_FORWARDED) return old->forwarding;

    size_t sz = object_size(old);
    if (to->free + sz > to->end) {
        to = &old_space;
        if (to->free + sz > to->end) abort();
    }

    Object *newObj = (Object *)to->free;
    memcpy(newObj, old, sz);
    to->free += sz;
    newObj->space = to->tag;
    newObj->forwarding = NULL;
    newObj->flags &= ~FLAG_FORWARDED;
    if (to->tag == SPACE_SURVIVOR) newObj->age++;
    else if (to->tag == SPACE_OLD) newObj->age = 0;

    old->forwarding = newObj;
    old->flags |= FLAG_FORWARDED;
    return newObj;
}

extern void scan_dirty_cards(Space *to);

static void scavenge_object_fields(Object *o, Space *to) {
    o->isa_or_map = follow(o->isa_or_map);
    if (o->isa_or_map && o->isa_or_map->space == SPACE_NURSERY)
        o->isa_or_map = copy_object(o->isa_or_map, to);

    if (o->slots) {
        Object **slots = (Object **)o->slots;
        for (uint32_t i = 0; i < o->slotCount; i++) {
            slots[i] = follow(slots[i]);
            if (slots[i] && slots[i]->space == SPACE_NURSERY)
                slots[i] = copy_object(slots[i], to);
        }
    }
}

void scavenge(void) {
    printf("--- scavenge begin ---\n");

    Space tmp = survivor_from;
    survivor_from = survivor_to;
    survivor_to = tmp;
    survivor_to.free = survivor_to.start;

    uint8_t *scan = survivor_to.start;

    for (uint32_t i = 0; i < root_count; i++) {
        Object *r = *root_set[i];
        if (r && r->space == SPACE_NURSERY)
            *root_set[i] = copy_object(r, &survivor_to);
    }

    while (scan < survivor_to.free) {
        Object *o = (Object *)scan;
        scavenge_object_fields(o, &survivor_to);
        scan += object_size(o);
    }

    scan = survivor_to.start;
    while (scan < survivor_to.free) {
        Object *o = (Object *)scan;
        if (o->age >= 3 && !(o->flags & FLAG_PINNED))
            copy_object(o, &old_space);
        scan += object_size(o);
    }

    nursery.free = nursery.start;
    cache_flush();
    printf("--- scavenge end --- (survivor used %zu bytes)\n",
           (size_t)(survivor_to.free - survivor_to.start));
}

size_t object_size(Object *o) {
    return sizeof(Object) + o->slotCount * sizeof(Object *);
}
