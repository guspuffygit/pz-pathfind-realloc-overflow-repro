#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#define FN_OFFSET 0x5aff0
#define POISON 0xA5

static atomic_long calls, risky, overflows, logged, injected, small4;
static int poison = -1, inject_every = -1;
static int fd = -2;
static int mode = -1; /* 0 count (safe copy here), 1 verify (call the library's own function) */

static char *base;
static const char *libname = "?";
static int cb(struct dl_phdr_info *i, size_t sz, void *d) {
    if (i->dlpi_name && strstr(i->dlpi_name, "libPZPathFind64")) { base = (char *)i->dlpi_addr; libname = i->dlpi_name; return 1; }
    return 0;
}
typedef void *(*real_fn)(void *, size_t, size_t);
static real_fn real(void) { if (!base) dl_iterate_phdr(cb, 0); return (real_fn)(base + FN_OFFSET); }

/* Both builds of the function start with push r12; push rbp; mov rbp,rdi. */
static int known_build(void) {
    const unsigned char *f = (const unsigned char *)real();
    return base && f[0] == 0x41 && f[1] == 0x54 && f[2] == 0x55 && f[3] == 0x48;
}

static void out(const char *s, int n) {
    if (fd == -2) {
        const char *p = getenv("PF_LOG");
        fd = p ? open(p, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    }
    if (fd >= 0) (void)!write(fd, s, n);
}

#define NH 96
static struct { size_t size, align, uold, unew; long n; } H[NH];
static atomic_flag hl = ATOMIC_FLAG_INIT;
static void hist(size_t size, size_t align, size_t uold, size_t unew) {
    while (atomic_flag_test_and_set(&hl)) ;
    for (int i = 0; i < NH; i++) {
        if (!H[i].n) { H[i].size = size; H[i].align = align; H[i].uold = uold; H[i].unew = unew; }
        if (H[i].size == size && H[i].align == align && H[i].uold == uold && H[i].unew == unew) { H[i].n++; break; }
    }
    atomic_flag_clear(&hl);
}
static void out(const char *s, int n);
static void dump(void) {
    char b[160];
    for (int i = 0; i < NH && H[i].n; i++) {
        int n = snprintf(b, sizeof b, "[pfhist] size=%zu align=%zu usable(old)=%zu usable(new)=%zu n=%ld\n", H[i].size, H[i].align, H[i].uold, H[i].unew, H[i].n);
        out(b, n);
    }
}

static void stats(const char *why) {
    char b[256];
    int n = snprintf(b, sizeof b, "[pfprobe pid=%d mode=%s] %s calls=%ld risky=%ld overflows=%ld injected=%ld of %ld 4-byte allocs\n", getpid(),
                     mode ? "verify" : "count", why, (long)calls, (long)risky, (long)overflows, (long)injected, (long)small4);
    out(b, n);
}

void *_Z18reallocate_alignedPvmm(void *old, size_t size, size_t align) {
    void *ra = __builtin_return_address(0);
    if (mode < 0) {
        const char *m = getenv("PF_MODE");
        mode = (m && !strcmp(m, "verify")) ? 1 : 0;
        if (mode && !known_build()) {
            mode = 0;
            static const char w[] = "[pfprobe] reallocate_aligned is not at +0x5aff0 in this build, falling back to count mode\n";
            out(w, sizeof w - 1);
        }
        char b[512];
        int n = 0;
        (void)ra;
        {
            const unsigned char *f = (const unsigned char *)real();
            n = snprintf(b, sizeof b, "[pfprobe pid=%d] first call from %s, bytes at +0x5afff: %02x %02x %02x %02x (%s)\n", getpid(),
                         libname, f[15], f[16], f[17], f[18], f[15] == 0x48 && f[16] == 0x89 && f[17] == 0x34 ? "PATCHED" : "ORIGINAL");
        }
        out(b, n);
    }
    long c = ++calls;
    if ((c & (c - 1)) == 0 || c % 200000 == 0) { stats("progress"); if (c >= 4096) dump(); }
    if (!old || !size) {
        if (mode) {
            return real()(old, size, align);
        }
        if (!size) { free(old); return NULL; }
        return aligned_alloc(align, size);
    }
    size_t uold = malloc_usable_size(old);
    void *nw;
    unsigned char snap[128];
    size_t snaplen = 0;
    if (mode) {
        /* Bytes at offsets >= size are never carried over logically, so marking them is harmless. */
        if (poison < 0) { const char *p = getenv("PF_POISON"); poison = p && *p == '1'; }
        if (poison && uold > size) memset((char *)old + size, POISON, uold - size);
        snaplen = uold < sizeof snap ? uold : sizeof snap;
        memcpy(snap, old, snaplen);
        nw = real()(old, size, align);
        if (!nw) return nw;
    } else {
        nw = aligned_alloc(align, size);
        if (!nw) return NULL;
        memcpy(nw, old, uold < size ? uold : size);
        free(old);
    }
    size_t unew = malloc_usable_size(nw);
    if (size <= 64) hist(size, align, uold, unew);
    if (uold > unew) {
        long r = ++risky;
        int wrote = 0;
        if (mode) {
            const unsigned char *p = (const unsigned char *)nw + unew;
            /* glibc reports 0 once the next chunk's header has lost its in-use bit. */
            wrote = !unew || (uold <= snaplen && !memcmp(p, snap + unew, uold - unew));
            if (wrote) ++overflows;
        }
        if (wrote || logged < 40) {
            ++logged;
            char b[256];
            int n = snprintf(b, sizeof b, "[pfprobe pid=%d] %s #%ld: size=%zu align=%zu usable(old)=%zu usable(new)=%zu -> %zu bytes past the block\n",
                             getpid(), mode ? (wrote ? "OVERFLOW WRITTEN" : "risk case, no overflow") : "would overflow", r, size, align, uold, unew,
                             uold - unew);
            out(b, n);
            if (wrote) stats("after overflow");
        }
    }
    return nw;
}

__attribute__((destructor)) static void fin(void) { if (calls) stats("exit"); }

/* Fault injection: every Nth 4-byte request from the pathfinder gets a 0x30 chunk, the block glibc returns when it exhausts a smallbin chunk. */
void *aligned_alloc(size_t align, size_t size) {
    static void *(*next)(size_t, size_t);
    if (!next) next = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
    if (size == 4) {
        if (inject_every < 0) { const char *p = getenv("PF_INJECT"); inject_every = p ? atoi(p) : 0; }
        char *ra = (char *)__builtin_return_address(0);
        if (inject_every > 0 && (base || (dl_iterate_phdr(cb, 0), base)) && ra > base && ra < base + 0x6d000) {
            long n = ++small4;
            if (n % inject_every == 0) { ++injected; return malloc(40); }
        }
    }
    return next(align, size);
}
