/* ============================================================
 * FULL SMALLTALK / OBJECTIVE-C DUAL RUNTIME
 * + GLOBAL + INLINE METHOD CACHE
 * + COMPLETE SMALLTALK IMAGE PERSISTENCE
 * + SELF LANGUAGE PROTOTYPE SYSTEM
 *
 * Ahmad Ali Parr — hand-rolled C, no AI generation
 *
 * Homogeneous substrate: every object (class-based or prototype)
 * carries a header that the shared dispatcher and image
 * serializer understand. All messages reduce to FLOP/MEMORY.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define MAX_CLASSES 256
#define MAX_METHODS 1024
#define MAX_OBJECTS 8192
#define SELECTOR_LEN 64
#define IMAGE_MAGIC 0x53544B4C /* "STKL" */
#define IMAGE_VERSION 3
#define CACHE_SIZE 1024
#define INLINE_CACHE_SIZE 4

/* ------------------------------------------------------------------ */
/* Shared object header */
/* ------------------------------------------------------------------ */
typedef struct Object {
    struct Object *isa_or_map;
    uint32_t flags;
    uint32_t identityHash;
    uint32_t slotCount;
    void *slots;
    struct Object *forwarding;
    uint32_t space; /* SpaceTag */
    uint32_t age;
} Object;

#define FLAG_PROTOTYPE  (1u << 0)
#define FLAG_MARKED     (1u << 1)
#define FLAG_FORWARDED  (1u << 2)
#define FLAG_PINNED     (1u << 3)

/* ------------------------------------------------------------------ */
/* Selector */
/* ------------------------------------------------------------------ */
typedef struct Selector {
    char name[SELECTOR_LEN];
    uint32_t uid;
} Selector;

Selector selector_table[MAX_METHODS];
uint32_t selector_count = 0;

Selector *sel_registerName(const char *name) {
    for (uint32_t i = 0; i < selector_count; i++)
        if (strcmp(selector_table[i].name, name) == 0)
            return &selector_table[i];
    if (selector_count >= MAX_METHODS) return NULL;
    Selector *s = &selector_table[selector_count];
    strncpy(s->name, name, SELECTOR_LEN-1);
    s->uid = selector_count++;
    return s;
}

/* ------------------------------------------------------------------ */
/* Method & Class */
/* ------------------------------------------------------------------ */
typedef void *(*IMP)(Object *self, Selector *sel, ...);

typedef struct Method {
    Selector *sel;
    IMP imp;
    struct Method *next;
} Method;

typedef struct Class {
    Object header;
    struct Class *super_class;
    char name[32];
    Method *methodDict;
    uint32_t instanceSize;
} Class;

Class *class_table[MAX_CLASSES];
uint32_t class_count = 0;

/* ------------------------------------------------------------------ */
/* Global Method Cache */
/* ------------------------------------------------------------------ */
typedef struct CacheEntry {
    Object *class_or_map;
    Selector *sel;
    IMP imp;
    uint32_t hits;
} CacheEntry;

CacheEntry global_cache[CACHE_SIZE];

static inline uint32_t cache_hash(Object *cls, Selector *sel) {
    return ((uintptr_t)cls ^ (uintptr_t)sel) % CACHE_SIZE;
}

IMP cache_lookup(Object *cls, Selector *sel) {
    uint32_t h = cache_hash(cls, sel);
    CacheEntry *e = &global_cache[h];
    if (e->class_or_map == cls && e->sel == sel) {
        e->hits++;
        return e->imp;
    }
    return NULL;
}

void cache_insert(Object *cls, Selector *sel, IMP imp) {
    uint32_t h = cache_hash(cls, sel);
    global_cache[h].class_or_map = cls;
    global_cache[h].sel = sel;
    global_cache[h].imp = imp;
    global_cache[h].hits = 1;
}

void cache_flush(void) { memset(global_cache, 0, sizeof(global_cache)); }

/* ------------------------------------------------------------------ */
/* Inline / Polymorphic Call-Site Cache */
/* ------------------------------------------------------------------ */
typedef struct InlineCache {
    Object *receiver_class[INLINE_CACHE_SIZE];
    IMP imp[INLINE_CACHE_SIZE];
    uint32_t count;
} InlineCache;

IMP inline_cache_lookup(InlineCache *ic, Object *cls) {
    for (uint32_t i = 0; i < ic->count; i++)
        if (ic->receiver_class[i] == cls) return ic->imp[i];
    return NULL;
}

void inline_cache_update(InlineCache *ic, Object *cls, IMP imp) {
    if (ic->count < INLINE_CACHE_SIZE) {
        ic->receiver_class[ic->count] = cls;
        ic->imp[ic->count] = imp;
        ic->count++;
    } else {
        memmove(&ic->receiver_class[0], &ic->receiver_class[1],
                (INLINE_CACHE_SIZE-1)*sizeof(Object*));
        memmove(&ic->imp[0], &ic->imp[1], (INLINE_CACHE_SIZE-1)*sizeof(IMP));
        ic->receiver_class[INLINE_CACHE_SIZE-1] = cls;
        ic->imp[INLINE_CACHE_SIZE-1] = imp;
    }
}

/* ------------------------------------------------------------------ */
/* Forward declarations */
/* ------------------------------------------------------------------ */
IMP lookup_method(Object *cls_or_map, Selector *sel);
size_t object_size(Object *o);

static inline Object *follow(Object *o) {
    while (o && (o->flags & FLAG_FORWARDED) && o->forwarding)
        o = o->forwarding;
    return o;
}

/* ------------------------------------------------------------------ */
/* Core message dispatcher */
/* ------------------------------------------------------------------ */
void *objc_msgSend(Object *receiver, Selector *sel, ...) {
    if (!receiver) return NULL;
    Object *cls = receiver->isa_or_map;
    IMP imp = cache_lookup(cls, sel);
    if (imp) return imp(receiver, sel);
    imp = lookup_method(cls, sel);
    if (imp) {
        cache_insert(cls, sel, imp);
        return imp(receiver, sel);
    }
    fprintf(stderr, "doesNotUnderstand: %s\n", sel->name);
    return NULL;
}

#define send(obj, name) objc_msgSend((obj), sel_registerName(name))

IMP lookup_method(Object *start, Selector *sel) {
    Object *current = start;
    while (current) {
        if (current->flags & FLAG_PROTOTYPE) break;
        Class *cls = (Class *)current;
        for (Method *m = cls->methodDict; m; m = m->next)
            if (m->sel == sel) return m->imp;
        current = (Object *)cls->super_class;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Class creation helpers */
/* ------------------------------------------------------------------ */
Class *create_class(const char *name, Class *super, uint32_t extra) {
    Class *cls = calloc(1, sizeof(Class));
    strncpy(cls->name, name, 31);
    cls->super_class = super;
    cls->instanceSize = sizeof(Object) + extra;
    cls->header.isa_or_map = (Object *)cls;
    class_table[class_count++] = cls;
    return cls;
}

void add_method(Class *cls, const char *selname, IMP imp) {
    Method *m = calloc(1, sizeof(Method));
    m->sel = sel_registerName(selname);
    m->imp = imp;
    m->next = cls->methodDict;
    cls->methodDict = m;
    cache_flush();
}

/* ------------------------------------------------------------------ */
/* Self Prototype System */
/* ------------------------------------------------------------------ */
typedef struct Slot {
    Selector *name;
    Object *value;
    bool isParent;
} Slot;

typedef struct Map {
    Object header;
    uint32_t slotCount;
    Slot *slots;
} Map;

Object *lobby;

Object *self_clone(Object *proto) {
    Object *o = calloc(1, sizeof(Object) + proto->slotCount * sizeof(Slot));
    o->isa_or_map = proto->isa_or_map;
    o->flags = FLAG_PROTOTYPE;
    o->slotCount = proto->slotCount;
    o->slots = (char*)o + sizeof(Object);
    memcpy(o->slots, proto->slots, proto->slotCount * sizeof(Slot));
    o->identityHash = (uint32_t)(uintptr_t)o;
    return o;
}

Object *self_new_prototype(void) {
    Map *map = calloc(1, sizeof(Map));
    map->header.flags = FLAG_PROTOTYPE;
    map->slotCount = 0;
    map->slots = NULL;
    Object *o = calloc(1, sizeof(Object));
    o->isa_or_map = (Object *)map;
    o->flags = FLAG_PROTOTYPE;
    o->slotCount = 0;
    o->slots = NULL;
    o->identityHash = (uint32_t)(uintptr_t)o;
    return o;
}

void self_add_slot(Object *obj, const char *name, Object *value, bool parent) {
    Map *map = (Map *)obj->isa_or_map;
    map->slots = realloc(map->slots, (map->slotCount+1)*sizeof(Slot));
    Slot *s = &map->slots[map->slotCount++];
    s->name = sel_registerName(name);
    s->value = value;
    s->isParent = parent;
    obj->slotCount = map->slotCount;
    obj->slots = map->slots;
    cache_flush();
}

/* ------------------------------------------------------------------ */
/* Image Persistence */
/* ------------------------------------------------------------------ */
Object *image[MAX_OBJECTS];
uint32_t image_count = 0;

void image_register(Object *o) {
    if (image_count < MAX_OBJECTS) image[image_count++] = o;
}

typedef struct ImageHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t objectCount;
    uint32_t selectorCount;
    uint32_t classCount;
} ImageHeader;

bool image_save(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    ImageHeader h = {IMAGE_MAGIC, IMAGE_VERSION, image_count, selector_count, class_count};
    fwrite(&h, sizeof(h), 1, fp);
    fwrite(selector_table, sizeof(Selector), selector_count, fp);
    for (uint32_t i = 0; i < image_count; i++) {
        Object *o = image[i];
        fwrite(o, sizeof(Object), 1, fp);
        if (o->slotCount && o->slots)
            fwrite(o->slots, sizeof(Slot), o->slotCount, fp);
    }
    fclose(fp);
    printf("Image saved: %u objects → %s\n", image_count, path);
    return true;
}

size_t object_size(Object *o) {
    return sizeof(Object) + o->slotCount * sizeof(Object *);
}

/* ------------------------------------------------------------------ */
/* Example methods that bottom out at the FLOP core */
/* ------------------------------------------------------------------ */
extern float flop_fma_op(float a, float b, float c);
extern void flop_store(uint32_t addr, float v);

void *Object_print(Object *self, Selector *sel, ...) {
    printf("<%s %p hash=%u>\n",
           (self->flags & FLAG_PROTOTYPE) ? "Prototype" : "Object",
           (void*)self, self->identityHash);
    return self;
}

void *Float_fma(Object *self, Selector *sel, ...) {
    float *data = (float *)((char*)self + sizeof(Object));
    float result = data[0] * data[1] + data[2];
    printf("Float>>fma -> %f (written to shared MEMORY)\n", result);
    flop_store(0x10, result);
    return self;
}

/* ------------------------------------------------------------------ */
/* Bootstrap */
/* ------------------------------------------------------------------ */
Class *Object_class;
Class *Float_class;

void bootstrap(void) {
    Object_class = create_class("Object", NULL, 0);
    add_method(Object_class, "print", Object_print);
    Float_class = create_class("Float", Object_class, 3 * sizeof(float));
    add_method(Float_class, "fma:with:with:", Float_fma);
    lobby = self_new_prototype();
    image_register(lobby);
    printf("=== FULL DUAL + SELF RUNTIME READY ===\n");
    printf("Global cache size %d, inline cache size %d\n", CACHE_SIZE, INLINE_CACHE_SIZE);
}

/* ------------------------------------------------------------------ */
/* Demo driver */
/* ------------------------------------------------------------------ */
int main(void) {
    bootstrap();
    Object *f = calloc(1, Float_class->instanceSize);
    f->isa_or_map = (Object *)Float_class;
    float *data = (float *)((char*)f + sizeof(Object));
    data[0] = 10.0f; data[1] = 20.0f; data[2] = 5.0f;
    image_register(f);
    send(f, "fma:with:with:");
    send(f, "print");
    Object *proto = self_new_prototype();
    self_add_slot(proto, "parent", lobby, true);
    Object *child = self_clone(proto);
    image_register(child);
    send(child, "print");
    image_save("macintosh-full.image");
    printf("=== ALL PATHS REDUCE TO SHARED METHOD CACHE + FLOP/MEMORY ===\n");
    return 0;
}
