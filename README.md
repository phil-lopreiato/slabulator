# slab-allocator
A basic implementation of a slab allocator, as described in [Bonwick '94](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.a).

## Interface
This is exported in `slab.h`

```
struct kmem_cache *
kmem_cache_create(char *name, size_t size, size_t align);

void *
kmem_cache_alloc(struct kmem_cache *cp, int flags);

void
kmem_cache_free(struct kmem_cache *cp, void *buf);

void
kmem_cache_destroy(struct kmem_cache *cp);
```

## Building
```
make
```

## Testing
```
make test
```
