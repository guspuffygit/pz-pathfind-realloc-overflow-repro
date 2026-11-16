# Heap overflow in `reallocate_aligned` (libPZPathFind64.so)

## Summary

- `reallocate_aligned(void* old, size_t size, size_t align)` copies `malloc_usable_size(old)` bytes into a new block of `size` bytes.
- glibc sometimes gives a small request a larger chunk than it asked for. When the old block's chunk is larger than the new block's chunk, the copy writes past the new block and overwrites the next chunk's header.
- The dedicated server then aborts inside glibc (`malloc(): invalid size (unsorted)`) or segfaults on `PathfindNativeThread`.
- Seen on Project Zomboid 42.22.2, Linux dedicated server, glibc 2.39. Library SHA-256 `e4c7d5c7f236be53cce9ac38fb655d626f15b25e018a422ea824f3b50fbf07d2`.
- A production server with about 20 players aborted this way twice in three days.

## Root cause

The function at `libPZPathFind64.so+0x5aff0` behaves like this.

```c
void* reallocate_aligned(void* old, size_t size, size_t align) {
    if (size == 0) { free(old); return NULL; }
    void* p = aligned_alloc(align, size);
    if (p == NULL) return NULL;
    memcpy(p, old, malloc_usable_size(old));   // not clamped to size
    free(old);
    return p;
}
```

`malloc_usable_size(old)` can exceed `size` even when the list is growing.

| Step | Request | Chunk glibc returns | Usable bytes |
| --- | --- | --- | --- |
| `ArrayList<bool>` allocates capacity 4 | 4 bytes | 0x30, because no 0x20 chunk is free and a 0x30 chunk cannot be split | 40 |
| The list grows to capacity 8 | 8 bytes | 0x20 | 24 |
| `memcpy(new, old, 40)` | | | 16 bytes past the block |

The victim in the field is the `ArrayList<bool>` local to `PolygonalMap2::findPathHighLevelThenLowLevel`, which `HLAStar::findPath` fills. It grows from 4 to 8 when a path crosses 5 level transitions. All 136 call sites of `reallocate_aligned` share the defect.

## Suggested fix

```c
size_t n = malloc_usable_size(old);
memcpy(p, old, n < size ? n : size);
```

Passing the old element count from the caller would be cleaner than asking the allocator. The Windows build was not checked.

## Backtrace from production

```
abort
malloc_printerr("malloc(): invalid size (unsorted)")
_int_malloc(bytes=48)
operator new
HLAStar::getSearchNode
HLAStar::addSuccessor
HLAStar::addSuccessorsOnEdge
HLAStar::getSuccessorsOnAdjacentChunks
HLAStar::getSuccessors
HLAStar::addChunkLevelAndAdjacentToList
HLAStar::findPath
PolygonalMap2::findPathHighLevelThenLowLevel
Java_zombie_pathfind_nativeCode_PathfindNative_findPath
```

The abort happens at a later allocation, so the faulting frame is rarely `reallocate_aligned` itself.

## Reproduction 1, standalone

This needs gcc, make and the shipped library. It takes a second.

```
make
./repro "/path/to/Project Zomboid Dedicated Server/linux64/libPZPathFind64.so"
```

`repro.c` loads the library, steers glibc into handing a 4-byte request a 0x30 chunk using ordinary malloc and free calls, and then calls the library's own `reallocate_aligned(old, 8, 1)`.

Expected output with the shipped library:

```
step 1: aligned_alloc(1, 4) returned a block with 40 usable bytes (glibc gave out a whole 0x30 chunk)
step 2: reallocate_aligned(old, 8, 1) returned a block with 24 usable bytes

OVERFLOW: memcpy copied 40 bytes into a 24-byte block.
16 bytes of the old block landed on the next chunk. Its size field now reads 0xa5a5a5a5a5a5a5a5.

step 3: the owner of the damaged chunk frees it
Segmentation fault
```

`make run-patched LIB=<path>` builds a fixed copy of the library with `patch.py` and runs the same test. It prints `OK: nothing was written past the new block` and exits 0.

Verified on glibc 2.39 (Ubuntu 24.04) and glibc 2.43.

## Reproduction 2, on a running dedicated server

`libpfprobe.so` is an `LD_PRELOAD` library that interposes `reallocate_aligned`. The pathfinder calls the function through its PLT, so the preload sees every call.

```
make
LD_PRELOAD=$PWD/libpfprobe.so PF_LOG=/tmp/pfprobe.log PF_MODE=verify PF_INJECT=200 ./start-server.sh
```

| Variable | Meaning |
| --- | --- |
| `PF_LOG` | Log file. |
| `PF_MODE=count` | Default. The probe does a safe copy itself and logs `would overflow` for each call where the old chunk is larger than the new one. Safe on a production server. |
| `PF_MODE=verify` | The probe calls the library's own function and logs `OVERFLOW WRITTEN` when bytes land past the new block. |
| `PF_INJECT=N` | Every Nth 4-byte request from the pathfinder gets a 0x30 chunk. This forces the allocator state from the table above, which otherwise depends on hours of heap fragmentation. |

Then make something path across 5 or more floors. Zombies chasing a player up a tall building does it.

Results on 42.22.2 under glibc 2.39, with path requests between floors of the downtown Louisville towers:

| Library | Result |
| --- | --- |
| Shipped | `OVERFLOW WRITTEN` on the first injected case. The server died seconds later with SIGSEGV in `SearchNode::init` on `PathfindNativeThread`. |
| Patched with `patch.py` | 38 injected cases, 0 overflows, 8,712 paths found, no crash in 5 minutes. |

Without injection the probe saw 3 million reallocations in 15 minutes and no natural hit, so the natural trigger is rare on a fresh heap.

`storm-driver/EvalScript.java` is the request generator used for those runs. It needs the Storm mod loader and is included for reference.

## Files

| File | Purpose |
| --- | --- |
| `repro.c` | Standalone reproduction. |
| `probe.c` | `LD_PRELOAD` probe for a live server. The function offset `0x5aff0` matches the 42.22.2 build. |
| `patch.py` | Rewrites the function in a copy of the library so it copies `min(usable(old), size)`. 94 bytes change, all inside `0x5aff0..0x5b060`. |
| `storm-driver/EvalScript.java` | Path request generator for a Storm server. |
