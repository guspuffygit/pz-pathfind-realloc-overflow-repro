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

## Reproduction

The first row of the table above depends on heap fragmentation that a production server reaches after hours. `libpfprobe.so` forces that row. It is an `LD_PRELOAD` library that gives every Nth 4-byte request from the pathfinder a 0x30 chunk, which is a block glibc itself may legally return. Everything after that is the game's own code.

The probe also interposes `reallocate_aligned`, which the pathfinder calls through its PLT. It calls the library's own function and checks whether bytes landed past the new block.

1. Build the probe.

   ```
   make
   ```

2. Start the dedicated server with it.

   ```
   LD_PRELOAD=$PWD/libpfprobe.so PF_LOG=/tmp/pfprobe.log PF_MODE=verify PF_INJECT=200 ./start-server.sh
   ```

3. Make something path across 5 or more floors. Zombies chasing a player up a tall building does it. `storm-driver/EvalScript.java` generates such requests on an empty server, but it needs the Storm mod loader.

4. Watch `/tmp/pfprobe.log`.

| Variable | Meaning |
| --- | --- |
| `PF_LOG` | Log file. |
| `PF_MODE=verify` | Call the library's own function and log `OVERFLOW WRITTEN` when bytes land past the new block. |
| `PF_INJECT=N` | Give every Nth 4-byte request from the pathfinder a 0x30 chunk. |

The function offset `0x5aff0` in `probe.c` matches the 42.22.2 build.

## Expected result

Measured on 42.22.2 under glibc 2.39, with path requests between floors of the downtown Louisville towers.

| Library | Result |
| --- | --- |
| Shipped | `OVERFLOW WRITTEN` on the first forced case. The server died seconds later with SIGSEGV in `SearchNode::init` on `PathfindNativeThread`. |
| Same library with the copy clamped to `size` | 38 forced cases, 0 overflows, 8,712 paths found, no crash in 5 minutes. |

Log from the shipped library:

```
[pfprobe] first call from .../linux64/libPZPathFind64.so, bytes at +0x5afff: 48 89 d7 e8 (ORIGINAL)
[pfprobe] OVERFLOW WRITTEN #1: size=8 align=1 usable(old)=40 usable(new)=0 -> 40 bytes past the block
#  SIGSEGV (0xb) at pc=0x00007e51762cfd67
# C  [libPZPathFind64.so+0x55d67]  SearchNode::init(VGAStar*, Square*)+0x17
```

`usable(new)=0` is glibc reporting that the chunk after the new block lost its in-use bit.

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
