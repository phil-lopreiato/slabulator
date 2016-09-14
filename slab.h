#ifndef PLOPREIATO_SLAB_H
#define PLOPREIATO_SLAB_H

#include <stddef.h>

/* Keep gcc happy */
#define UNUSED(x) UNUSED_ ## x __attribute__((unused))

#if DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) do {} while (0)
#endif

/**
 * An implementation of a Slab Allocator
 * As outlined in Bonwick (94)
 *
 * The basic idea is to keep caches of pre-initialized
 * objects that the allocator can quickly give out.
 */

#define KM_SLEEP 0
#define KM_NOSLEEP 1

#define KM_REGULAR_CACHE 0
#define KM_SMALL_CACHE 1

union buf_ish {
        struct kmem_bufctl *bufctl;
        void *buf;
};

/**
 * A slab is a grouping of objects ready to be allocated
 * It is continguous virtual memory, with a reference count
 */
struct kmem_slab {
        struct kmem_slab *next;
        struct kmem_slab *last;
        union buf_ish firstbuf;
        union buf_ish lastbuf;  /* For small objects (1/8 pagesize)
                                 * don't use bufctls, but keep the
                                 * bufs directly on the page. In
                                 * that case, these point directly
                                 * to the next item in the slab's freelist.
                                 * Otherwise, they'll be bufctls.
                                 */
        size_t size;            /* Number of bufs total on slab */
        size_t refcount;        /* How many bufs are in use */
        void *start;            /* Address of the allocated memory for this slab */
};

/**
 * When dealing with larger objects, store metadata
 */
struct kmem_bufctl {
        struct kmem_bufctl *next; /* Next free buffer in the slab */
        struct kmem_slab *slab;   /* A pointer back to the slab */
        void *buf;                /* This is a pointer to the real data */
};

/**
 * The basic container for an object cache
 */
struct kmem_cache {
        char *name;                 /* Used for debug purposes */
        unsigned slab_count;        /* Number of slabs in this cache */
        size_t object_size;         /* The size of one object in the cache
                                     * including alignment
                                     */
        struct kmem_slab *slabs;    /* Circular, doubly linked list of slabs
                                     * Sorted as empty (all allocated), then
                                     * partial slabs, (some allocated), and
                                     * complete (all free, refcount = 0)
                                     */
        struct kmem_slab *freelist; /* Pointer to first nonempty slab */
        unsigned char type;     /* Either KM_REGULAR_CACHE or
                                 * KM_SMALL_CACHE, depending if the small
                                 * object optimizations are in play
                                 */
        struct kmem_hash *hash; /* Hash table for mapping buf -> bufctl */
};


/**
 * Create a new cache for objects of a given size
 */
struct kmem_cache *
kmem_cache_create(
        char *name,
        size_t size,
        size_t align
        // TODO: not implementing constructors/destructors here
        //void (*constructor)(void *, size_t),
        //void (*destructor)(void *, size_t)
);

/**
 * Allocate an item from the given cache
 * flags is one of KM_SLEEP or KM_NOSLEEP,
 * depending if we should block until memory
 * is available to allocate
 */
void *
kmem_cache_alloc(
        struct kmem_cache *cp,
        int flags
);

/**
 * Return an element to the cache
 */
void
kmem_cache_free(
        struct kmem_cache *cp,
        void *buf
);

/**
 * Destroy the given cache
 */
void
kmem_cache_destroy(
        struct kmem_cache *cp
);

#endif
