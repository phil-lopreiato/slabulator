#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "slab.h"
#include "hash.h"

/**
 * These are caches we allocate to store 'kmem_bufctl' and 'kmem_slab'
 * They get lazy initalized as caches are created
 * IMPORTANT: sizeof(kmem_bufctl) and sizeof(kmem_slab) MUST BE SMALLER
 * than 1/8th the system page size so that /these/ caches do not
 * require bufctls and then recurse infinitely.
 * Ahem... https://xkcd.com/754/
 */
static struct kmem_cache *money_cache = NULL; // Cache for kmem_cache
static struct kmem_cache *bufctl_cache = NULL;
static struct kmem_cache *slab_cache = NULL;
static struct kmem_cache *hash_cache = NULL;
static struct kmem_cache *hash_node_cache = NULL;

/* Size of a page on the system */
static size_t system_pagesize = 0;

/**
 * Add a new slab into the slab linkedlist and to the freelist
 * Since the new slab is complete (refcount == 0), we want to add it
 * to the end of the list
 */
static inline void
_kmem_cache_add_slab(struct kmem_cache *cp, struct kmem_slab *slab)
{
        struct kmem_slab *head;
        struct kmem_slab *tail;

        if (!cp->slabs) {
                // There are no slabs in the cache currently,
                // add the new one at the beginning and update freelist
                DEBUG_PRINT("Adding new (first) slab to top of list\n");
                cp->slabs = slab;
                cp->freelist = slab;

                // Since the list is circular & doubly linked...
                slab->next = slab;
                slab->last = slab;
        } else {
                // Add the new slab to the end of the list & pointer shit
                DEBUG_PRINT("Adding new slab %p to tail\n", (void*)slab);
                head = cp->slabs;
                tail = head->last;
                tail->next = slab;
                slab->last = tail;
                slab->next = head;
                head->last = slab;
        }

        DEBUG_PRINT("Cache %s got new slab %p, next: %p, last: %p\n", cp->name, (void*)slab, (void*)slab->next, (void*)slab->last);

        // Update the freelist pointer
        head = cp->freelist;
        while (head->size == head->refcount) {
                head = head->last;
        }
        if (head != cp->freelist) {
                DEBUG_PRINT("Setting %s freelist to %p\n", cp->name, (void*)head);
                cp->freelist = head;
        }
        DEBUG_PRINT("Slab freelist is now %p\n", (void*)cp->freelist);
        DEBUG_PRINT("Freelist size %lu, total %lu\n", cp->freelist->refcount, cp->freelist->size);

        cp->slab_count++;
        DEBUG_PRINT("Cache %s now has %u slabs\n", cp->name, cp->slab_count);
}

/**
 * Moves a slab to the tail of the list
 * These slabs should be empty (e.g. refcount 0)
 */
static inline void
_kmem_cache_empty_slab(struct kmem_cache *cp, struct kmem_slab *slab)
{
        DEBUG_PRINT("Moving slab %p to HEAD of freelist of cache %s\n", (void*)(slab->last), cp->name);
        if (cp->freelist == slab) {
                // If this slab was first on the freelist...
                // However, don't update the freelist pointer if it would
                // go to a slab without space
                cp->freelist = slab->next->refcount < slab->next->size
                        ? slab->next
                        : NULL;
                DEBUG_PRINT("Updating freelist pointer to %p\n", (void*)cp->freelist);
        }
        if (cp->slabs == slab) {
                // If there's only one slab, it's already at the end
                return;
        }

        slab->last->next = slab->next;
        slab->next->last = slab->last;

        slab->last = cp->slabs->last;
        cp->slabs->last->next = slab;

        slab->next = cp->slabs;
        slab->last = cp->slabs->last;

        cp->slabs = slab;

        DEBUG_PRINT("Slab %p is now the HEAD of cache %s\n", (void*)(cp->slabs->last), cp->name);
}

/**
 * Remove this slab from the list
 */
static inline void
_kmem_cache_remove_slab(struct kmem_cache *cp, struct kmem_slab *slab)
{
        DEBUG_PRINT("Removing slab %p from cache %s freelist\n", (void*)slab, cp->name);
        cp->slab_count--;
        if (cp->slabs == slab->next && cp->slabs == slab->last) {
                cp->slabs = NULL;
                cp->freelist = NULL;
        }

        slab->last->next = slab->next;
        slab->next->last = slab->last;

        if (cp->slabs == slab) {
                cp->slabs = slab->next;
        }

        if (cp->freelist == slab) {
                cp->freelist = slab->next->refcount < slab->next->size
                        ? slab->next
                        : NULL;
        }
}

/**
 * Initialize a newly allocated slab
 * This is used for slabs with object size < 1/8th of a page
 * In thise case, we don't use separate bufctls, but keep the data
 * directly on the page and put the slab data at the end
 * Set offset param to skip initializing the first n items in the cache
 */
static inline struct kmem_slab *
_kmem_slab_init_small(struct kmem_cache *cp, void *page, size_t offset)
{
        struct kmem_slab *slab;
        size_t available;
        void *i;

        DEBUG_PRINT("Setting up new (small object) slab for cache %s...\n", cp->name);

        // We put the slab at the end of the page
        slab = (struct kmem_slab *)((uintptr_t)page + system_pagesize - sizeof(struct kmem_slab));

        // Zero out the new slab metadata (this set all things to 0/NULL)
        memset(slab, 0, sizeof(struct kmem_slab));

        available = system_pagesize - sizeof(struct kmem_slab);
        slab->size = (available / cp->object_size) - offset - 1;

        slab->firstbuf.buf = (void**)((uintptr_t)page + (offset * cp->object_size));
        slab->lastbuf.buf = (void**)((uintptr_t)page + ((slab->size) * cp->object_size));
        DEBUG_PRINT("One page (%lu bytes) can hold %lu x %lu byte bufs, "
               "plus %lu bytes for slab metadata\n",
                system_pagesize, slab->size, cp->object_size,
                sizeof(struct kmem_slab));

        // Initialize the freelist pointers to be the next
        // To avoid overhead, the first byte of each buf is the pointer
        int count;
        count = 0;
        for (i = slab->firstbuf.buf;
             i <= slab->lastbuf.buf;
             i = (void*)((uintptr_t)i + cp->object_size)) {
                count++;
                *((void**)i) = (void*)((uintptr_t)i + cp->object_size);
        }

        return slab;
}

static inline struct kmem_slab *
_kmem_slab_init_large(struct kmem_cache *cp, void *page, int flags)
{
        struct kmem_slab *slab;
        struct kmem_bufctl *bufctl;
        struct kmem_bufctl *last;
        unsigned int i;

        DEBUG_PRINT("Setting up new (large object) slab for cache %s...\n", cp->name);

        // Allocate and zero out a new slab
        slab = kmem_cache_alloc(slab_cache, flags);
        memset(slab, 0, sizeof(struct kmem_slab));

        slab->size = system_pagesize / cp->object_size;
        DEBUG_PRINT("One page (%lu bytes) can hold %lu x %lu byte bufs\n",
               system_pagesize, slab->size, cp->object_size);

        // Allocate bufctls that point to our new data
        // Do the first and last separately to minimize in-loop branching
        // Totally not premature optimization... https://xkcd.com/1691/
        slab->firstbuf.bufctl = kmem_cache_alloc(bufctl_cache, flags);
        last = slab->firstbuf.bufctl;
        last->slab = slab;
        last->buf = page;
        last->next = NULL;
        kmem_hash_insert(cp->hash, last->buf, last);
        for (i = 1; i < slab->size-1; i++) {
                bufctl = kmem_cache_alloc(bufctl_cache, flags);
                bufctl->slab = slab;
                bufctl->buf = (void*)((uintptr_t)page + (i * cp->object_size));
                bufctl->next = NULL;
                last->next = bufctl;

                // Insert this bufctl -> buf into the hashtable
                kmem_hash_insert(cp->hash, bufctl->buf, bufctl);
                last = bufctl;
        }
        slab->lastbuf.bufctl = kmem_cache_alloc(bufctl_cache, flags);
        last->next = slab->lastbuf.bufctl;
        last = slab->lastbuf.bufctl;
        last->slab = slab;
        last->buf = (void*)((uintptr_t)page + (i * cp->object_size));
        last->next = NULL;
        kmem_hash_insert(cp->hash, last->buf, last);

        return slab;
}

/**
 * Add a new slab to the given cache
 * Returns a pointer to the new slab, or 0 on error
 */
static struct kmem_slab *
_kmem_cache_grow(struct kmem_cache *cp, int flags)
{
        void *page;
        struct kmem_slab *slab;

        DEBUG_PRINT("Allocating new slab for cache %s...\n", cp->name);

        // Allocate page-aligned memory
        if (0 != posix_memalign(&page, system_pagesize, system_pagesize))
                return NULL;

        slab = cp->type == KM_SMALL_CACHE
                ? _kmem_slab_init_small(cp, page, 0 /* No offset */)
                : _kmem_slab_init_large(cp, page, flags);
        slab->start = page;

        // Add the slab into the cache's freelist
        _kmem_cache_add_slab(cp, slab);

        return slab;
}

/**
 * Return all bufctls from a slab to their cache
 * ASSUMES: cache type is KM_REGULAR_CACHE
 */
static inline void
_kmem_slab_reap_large(struct kmem_slab *slab)
{
        struct kmem_bufctl *bufctl;
        unsigned count;

        bufctl = slab->firstbuf.bufctl;
        for (count = 0; bufctl && count < slab->size; count++) {
                kmem_cache_free(bufctl_cache, bufctl);
                bufctl = bufctl->next;
        }
}

/**
 * Reclaims all empty slabs in the cache
 *   ___o .--.
 *  /___| |OO|
 *      |_|  |_
 *      (_    _)
 *      | |   \
 *      | |___/
 */
static void
_kmem_cache_reap(struct kmem_cache *cp, unsigned force)
{
        struct kmem_slab *slab;
        struct kmem_slab *next;
        void *buf;

        if (!cp->slabs) return;
        DEBUG_PRINT("Reaping slabs from cache %s (starts with %u, at %p)\n", cp->name, cp->slab_count, (void*)cp->slabs);
        slab = cp->slabs;
        while (force || (slab->refcount == 0 && cp->slab_count > 1)) {
                // For every slab that must meet their maker...
                // (but don't free the last slab)
                // https://xkcd.com/393/
                _kmem_cache_remove_slab(cp, slab);
                buf = (void*)((unsigned long)slab->start>> 12 << 12);
                if (cp->type == KM_REGULAR_CACHE) {
                        _kmem_slab_reap_large(slab);
                }

                next = slab->next;
                kmem_cache_free(slab_cache, slab);
                DEBUG_PRINT("Freeing %p, from slab\n", buf);
                free(buf);
                if (slab == next) break;
                slab = next;
        }
        DEBUG_PRINT("Cache %s now has %u slabs\n", cp->name, cp->slab_count);
}

/**
 * Called on a newly-full slab
 * This moves the given slab to the HEAD postion in the freelist
 */
static inline void
_kmem_slab_complete(struct kmem_cache *cp, struct kmem_slab *slab)
{
        struct kmem_slab *old_last;
        struct kmem_slab *old_next;

        old_last = slab->last;
        old_next = slab->next;

        slab->last = cp->slabs->last;
        slab->next = cp->slabs->next;
        cp->slabs = slab;

        old_last->next = old_next;
        old_next->last = old_last;

        if (cp->freelist == slab) {
                DEBUG_PRINT("Updating freelist pointer\n");
                cp->freelist = old_next;
        }
}

/**
 * Allocate a buf out of the given slab
 * Remember, these are formatted (link)(buf)
 * So take the first one, and then update the freelist
 * ASSUMED: that the cache type == KM_SMALL_CACHE
 * ASSUMED: that the slab has free bufs available
 */
static inline void *
_kmem_cache_alloc_small(struct kmem_cache *cp, struct kmem_slab *slab)
{
        void **buf;

        buf = slab->firstbuf.buf;
        if (!buf) {
                DEBUG_PRINT("Unable to obtain buf, slab is full...\n");
                DEBUG_PRINT("Slab size %lu, refcount %lu\n", slab->size, slab->refcount);
                slab = _kmem_cache_grow(cp, KM_SLEEP);
                buf = slab->firstbuf.buf;
                if (!buf) return NULL;
        }
        DEBUG_PRINT("Allocating item from small cache at %p\n", (void*)buf);
        slab->firstbuf.buf = *buf;
        slab->refcount++;

        DEBUG_PRINT("Slab refcount is now %lu\n", slab->refcount);

        return buf;
}

/**
 * Allocate a buf out of the given slab
 * We need to take a bufctl from the slab's freelist
 * ASSUMED: that the cache type == KM_REGULAR_CACHE
 * ASSUMED: that the slab has free bufs available
 */
static inline void *
_kmem_cache_alloc_large(struct kmem_cache *cp, struct kmem_slab *slab)
{
        struct kmem_bufctl *bufctl;

        bufctl = slab->firstbuf.bufctl;
        if (!bufctl) {
                DEBUG_PRINT("Unable to obtain bufctl, slab is full...\n");
                slab = _kmem_cache_grow(cp, KM_SLEEP);
                bufctl = slab->firstbuf.bufctl;
                if (!bufctl) return NULL;
        }
        slab->refcount++;
        slab->firstbuf.bufctl = bufctl->next;

        DEBUG_PRINT("Slab refcount is now %lu\n", slab->refcount);

        return bufctl->buf;
}

/**
 * Free an item from the cache
 * ASSUMED: the cache type == KM_SMALL_CACHE
 */
static inline void
_kmem_cache_free_small(struct kmem_cache *cp, void *buf)
{
        void *page;
        struct kmem_slab *slab;

        // Find the start of the page
        DEBUG_PRINT("Freeing item %p from small cache %s\n", buf, cp->name);
        page = (void*)((unsigned long)buf >> 12 << 12);
        DEBUG_PRINT("Found start of page at %p\n", page);

        slab = (struct kmem_slab *)((uintptr_t)page + system_pagesize - sizeof(struct kmem_slab));

        // Update the last buffer pointer to this one
        // And make this buf point to NULL (end of list)
        *((void**)slab->lastbuf.buf) = buf;

        if((--slab->refcount) == 0 && cp->slab_count > 1) {
                // Don't reap the last slab in the cache
                DEBUG_PRINT("Slab is no longer referenced. Reaping...\n");
                _kmem_cache_empty_slab(cp, slab);
                _kmem_cache_reap(cp, 0);
        } else {
                DEBUG_PRINT("Slab refcount is now %lu\n", slab->refcount);
        }
}


/**
 * Free an item from the cache
 * Here, we need to obtain the bufctl from the cache's hash
 * That buf gets added back to the slab's freelist
 * ASSUMED: the cache type == KM_REGULAR_CACHE
 */
static inline void
_kmem_cache_free_large(struct kmem_cache *cp, void *buf)
{
        struct kmem_slab *slab;
        struct kmem_bufctl *bufctl;

        DEBUG_PRINT("Freeing item %p from large cache %s\n", buf, cp->name);
        bufctl = kmem_hash_get(cp->hash, buf);
        if (!bufctl) {
                DEBUG_PRINT("Unable to find bufctl for item %p\n", buf);
                return;
        }
        slab = bufctl->slab;
        assert(slab);

        // Insert this bufctl back into the freelist
        slab->lastbuf.bufctl->next = bufctl;
        slab->lastbuf.bufctl = bufctl;

        if ((--slab->refcount) == 0 && cp->slab_count > 1) {
                // Don't reap the slab slab in the cache
                DEBUG_PRINT("Slab is no longer referenced. Reaping...\n");
                _kmem_cache_empty_slab(cp, slab);

                // Reclaim the slab
                _kmem_cache_reap(cp, 0);
        } else {
                DEBUG_PRINT("Slab refcount is now %lu\n", slab->refcount);
        }
}
