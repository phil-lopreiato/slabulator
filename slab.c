#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "slab.h"
#include "hash.h"
#include "slab_internal.c"

/**
 * Bootstrapping function, create all the internal caches we'll need
 * Includes a fun, hacky variable to tell kmem_cache_create to not init
 * the hash tables at create time, because that wouldn't create recursion
 * (bad), since those caches won't be initialized until this function
 * completes
 */
static uint8_t _create_hash_on_create = 1;
static void
__init_global_caches()
{
        void *firstpage;

        /* First, we solve the bootstrapping problem
         * Malloc a page and manually set up a "small object" cache
         * This basically a special cased/more generic kmem_cache_grow
         * Then, we can use that cache for all the other caches we init
         */
        firstpage = malloc(system_pagesize);
        _create_hash_on_create = 0;
        money_cache = firstpage;
        money_cache->name = "cash_money_cache";
        money_cache->object_size = sizeof(struct kmem_cache);
        money_cache->slabs = NULL;
        money_cache->freelist = NULL;
        money_cache->type = KM_SMALL_CACHE;
        money_cache->hash = NULL;

        __slab_init_small(money_cache, money_cache, 1);

        hash_node_cache = kmem_cache_create("hash_node_cache",
                                            sizeof(struct kmem_hash_node),
                                            0 /* No align */);
        hash_cache = kmem_cache_create("hash_cache",
                                       sizeof(struct kmem_hash),
                                       0 /* No align */);
        slab_cache = kmem_cache_create("kmem_slab cache",
                                       sizeof(struct kmem_slab),
                                       0 /* No align */);
        bufctl_cache = kmem_cache_create("kmem_bufctl cache",
                                         sizeof(struct kmem_bufctl),
                                         0 /* No align */);
        _create_hash_on_create = 1;

        // Now, init the hash tables for these caches
        money_cache->hash = kmem_hash_init(hash_cache, hash_node_cache);
        hash_node_cache->hash = kmem_hash_init(hash_cache, hash_node_cache);
        hash_cache->hash = kmem_hash_init(hash_cache, hash_node_cache);
        slab_cache->hash = kmem_hash_init(hash_cache, hash_node_cache);
        bufctl_cache->hash =kmem_hash_init(hash_cache, hash_node_cache);
}

/**
 * Create a new cache for objects of a given size
 * Returns a pointer to an initialized cache,
 * or NULL on error
 */
struct kmem_cache *
kmem_cache_create(char *name, size_t size, size_t align)
{
        struct kmem_cache *cp;
        size_t align_diff;

        DEBUG_PRINT("Creating new slab: %s. Object size %lu, aligned at %lu\n", name, size, align);

        // Preconditions: size > 0 and align is 0 or a power of 2
        assert(size > 0);
        assert(align == 0 || !(align & (align - 1)));


        // TODO this should support multi-page slabs for large objects
        if (!system_pagesize) {
                system_pagesize = sysconf(_SC_PAGESIZE);
                DEBUG_PRINT("System page size is %lu bytes\n", system_pagesize);
        }
        if (!money_cache) {
                __init_global_caches();
        }

        cp = kmem_cache_alloc(money_cache, KM_SLEEP);
        if (!cp) return NULL;

        // Initialize the new cache
        cp->name = name;
        cp->slabs = NULL;
        cp->freelist = NULL;

        // Alignment "magic"
        align_diff = align /* Be safe, mod by 0 is bad */
                ? size % align
                : 0;
        cp->object_size = size + align_diff;

        cp->type = cp->object_size < (system_pagesize / 8)
                ? KM_SMALL_CACHE
                : KM_REGULAR_CACHE;
        DEBUG_PRINT("Cache type is: %d\n", cp->type);

        if (_create_hash_on_create) {
                cp->hash = kmem_hash_init(hash_cache, hash_node_cache);
                DEBUG_PRINT("Adding hash %p to cache %s\n", (void*)cp->hash, name);
        }

        // Add the first slab, so we're ready to go at first allocation
        if (!__cache_grow(cp, KM_SLEEP)) {
        DEBUG_PRINT("Failed adding initial slab to cache %s\n", name);
        }

        return cp;
}

/**
 * Allocate an item from the given cache
 * flags is one of KM_SLEEP or KM_NOSLEEP,
 * depending if we should block until memory
 * is available to allocate. If unable to allocate (and KM_NOSLEP),
 * then return NULL
 */
void *
kmem_cache_alloc(struct kmem_cache *cp, int flags)
{
        struct kmem_slab *slab;
        void *data;

        DEBUG_PRINT("Allocating new item from cache %s\n", cp->name);

        // Get the first slab with free bufs
        // This is the first item in the freelist, except for when that
        // is the HEAD of the list (e.g. the only slab is full)
        slab = cp->freelist;
        while (!slab || slab->refcount >= slab->size) {
                // No slabs are available, get a new one
                DEBUG_PRINT("Growing the cache...\n");
                slab = __cache_grow(cp, flags);
                if (!slab && flags == KM_NOSLEEP) break;
        }

        if (!slab) {
                DEBUG_PRINT("Unable to allocate new slab for cache %s\n", cp->name);
                return NULL;
        }

        data = cp->type == KM_REGULAR_CACHE
                ? __cache_alloc_large(cp, slab)
                : __cache_alloc_small(cp, slab);

        if (slab->size == slab->refcount) {
                // Slab is full, move it off the cache's freelist
                DEBUG_PRINT("Slab is now complete, moving...\n");
                __slab_complete(cp, slab);
        }

        return data;
}

/**
 * Return an element to the cache
 */
void
kmem_cache_free(struct kmem_cache *cp, void *buf)
{
        if (cp->type == KM_SMALL_CACHE) {
                __cache_free_small(cp, buf);
        } else {
                __cache_free_large(cp, buf);
        }
}

/**
 * Destroy the given cache
 */
void
kmem_cache_destroy(struct kmem_cache *cp)
{
        kmem_hash_free(hash_cache, cp->hash);
        cp->freelist = NULL;
        __cache_reap(cp, 1);
}
