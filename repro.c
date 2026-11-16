/*
 * Standalone reproduction of the heap overflow in reallocate_aligned() from libPZPathFind64.so.
 *
 *   ./repro /path/to/ProjectZomboid/linux64/libPZPathFind64.so
 *
 * Exit status: 0 the copy stayed inside the new block, 1 the copy overflowed, 2 setup failed.
 * With the shipped library glibc usually aborts the process before the exit status is reached.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARK 0xA5

/* Whatever the heap holds right after the block the list grows into. */
static void *volatile neighbour;

typedef void *(*reallocate_aligned_fn)(void *old, size_t size, size_t align);

/*
 * Returns a 4-byte allocation that glibc served from a 0x30 chunk (40 usable bytes).
 *
 * glibc does this whenever no free 0x20 chunk exists and the smallest free chunk is 0x30.
 * Splitting would leave a 0x10 remainder, which is below the minimum chunk size, so malloc
 * hands out the whole chunk. A long-running server reaches this state on its own.
 */
__attribute__((noinline)) static unsigned char *four_bytes_in_a_big_chunk(void) {
    enum { N = 64 };
    void *p[N];

    /*
     * Some unrelated 8-byte object that is alive for now, and the object allocated right behind it.
     * Volatile keeps the compiler from dropping the pair.
     */
    void *volatile other = NULL;
    for (int i = 0; i < 10000; i++) {
        other = malloc(8);
        neighbour = malloc(1500);
        if ((char *)neighbour == (char *)other + 32) break;
    }

    for (int i = 0; i < N; i++) p[i] = malloc(40);

    /* Every other chunk goes free. The first 7 fill the tcache, the rest land in the fastbin. */
    for (int i = 0; i < N; i += 2) free(p[i]);

    /* A large request consolidates the fastbin, which moves those chunks to the 0x30 smallbin. */
    free(malloc(0x10000));

    /* Use up every free 0x20 chunk. The next 4-byte request then takes a 0x30 chunk whole. */
    for (int i = 0; i < 100000; i++) {
        unsigned char *b = aligned_alloc(1, 4);
        if (malloc_usable_size(b) >= 40) {
            /* The unrelated object dies, so one 0x20 chunk is free again for the list to grow into. */
            free(other);
            return b;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: %s /path/to/libPZPathFind64.so\n", argv[0]);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) {
        printf("dlopen failed: %s\n", dlerror());
        return 2;
    }
    reallocate_aligned_fn reallocate_aligned = (reallocate_aligned_fn)dlsym(lib, "_Z18reallocate_alignedPvmm");
    if (!reallocate_aligned) {
        printf("symbol reallocate_aligned(void*, unsigned long, unsigned long) not found\n");
        return 2;
    }
    printf("glibc %s, library %s\n\n", gnu_get_libc_version(), argv[1]);

    /* Step 1. An ArrayList<bool> with capacity 4, as HLAStar::findPath allocates it. */
    unsigned char *list = four_bytes_in_a_big_chunk();
    if (list) {
        printf("step 1: aligned_alloc(1, 4) returned a block with %zu usable bytes (glibc gave out a whole 0x30 chunk)\n",
               malloc_usable_size(list));
    } else {
        list = malloc(40);
        printf("step 1: could not steer this glibc into the state, so the old block comes from malloc(40) instead\n");
    }
    size_t usable_old = malloc_usable_size(list);
    memset(list, 1, 4);
    memset(list + 4, MARK, usable_old - 4);

    /* Step 2. The list grows from 4 to 8 elements, as it does on a path with 5 level transitions. */
    unsigned char *grown = reallocate_aligned(list, 8, 1);
    size_t usable_new = malloc_usable_size(grown);
    printf("step 2: reallocate_aligned(old, 8, 1) returned a block with %zu usable bytes\n", usable_new);

    size_t block = usable_new ? usable_new : 24;
    size_t past = 0;
    while (block + past < usable_old && grown[block + past] == MARK) past++;

    if (!past) {
        printf("\nOK: nothing was written past the new block. This library copies min(usable(old), size).\n");
        return 0;
    }
    unsigned long next_size;
    memcpy(&next_size, grown + block, sizeof next_size);
    printf("\nOVERFLOW: memcpy copied %zu bytes into a %zu-byte block.\n", usable_old, block);
    printf("%zu bytes of the old block landed on the next chunk. Its size field now reads 0x%lx.\n", past, next_size);

    /* Step 3. glibc aborts when it next reads the damaged chunk header. */
    printf("\n");
    printf("step 3: the owner of the damaged chunk frees it\n");
    free(neighbour);
    void *q[512];
    for (int round = 0; round < 64; round++) {
        for (int i = 0; i < 512; i++) q[i] = malloc(16 + (i * 37) % 2000);
        for (int i = 0; i < 512; i++) free(q[i]);
    }
    free(grown);
    printf("glibc did not abort in this run, but the heap is corrupt.\n");
    return 1;
}
