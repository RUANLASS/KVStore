# Hash table collision-chain bug + slab allocator

## Symptom

`test_kvstore.py` (5000 SET/GET/DELETE ops) reported **9552 mismatches out of
17600 commands** — colliding keys would `SET` successfully but a later `GET`
reported "does not exist", as if the value had vanished.

Minimal repro: 4 keys that hash to the same bucket (`key25`, `key34`, `key43`,
`key52` all hash to index 432 under the additive `HashFunction`):

```
SET key25 v25
SET key34 v34
SET key43 v43
SET key52 v52
GET key25   -> Key:key25, Value:v25
GET key34   -> Key:key34, Value:v34
GET key43   -> Key:key43 does not exist   <-- lost
GET key52   -> Key:key52, Value:v52
```

3 colliding keys worked fine; the bug only appeared from the 4th collision
onward, which is why quick manual spot-checks kept missing it.

## Root cause 1 (the actual bug): broken tail-finding loop

`linkedlist_insert` in `src/linkedlist.cpp` used this loop to find the last
node in the overflow chain before appending:

```cpp
LinkedList* temp = list;
while (temp->next->next) {
    temp = temp->next;
}
temp->next = node;
```

For a chain `head -> A -> B` (B is the real last node, `B->next == NULL`),
this stops one node too early: starting at `temp = head`, it checks
`head->next->next` (= `A->next` = `B`, non-NULL) and advances to `temp = A`.
Now it checks `A->next->next` (= `B->next` = `NULL`) and stops — with `temp`
still at `A`, not `B`. The new node then gets appended as `A->next = node`,
**overwriting the pointer to B and orphaning/leaking it**.

Fix: walk until `temp->next` itself is `NULL` (i.e. `temp` really is the last
node), not until the node *after* it is:

```cpp
while (temp->next) {
    temp = temp->next;
}
temp->next = node;
```

This is a deterministic bug — it fires every time a 4th (or later) key lands
in the same overflow chain, regardless of allocator.

## Root cause 2 (a real but secondary bug): uninitialized `next` pointer

Separately, `allocate_list()` originally used raw `malloc`:

```cpp
LinkedList* allocate_list() {
    return (LinkedList*) malloc(sizeof(LinkedList));
}
```

`malloc` does not zero memory, so a freshly allocated node's `next` field
held garbage instead of `NULL`. In a short-lived process with little prior
heap activity, freshly-mapped OS pages are often zero-filled by luck, which
is exactly why small manual tests (a couple of SETs in an otherwise-idle
process) didn't show any problem. Once the allocator had churned through
thousands of prior allocations/frees (as in the stress test), reused memory
carried non-zero leftover bytes into `next`, corrupting chain traversal.

This was real undefined behavior, but turned out to be secondary — root
cause 1 (the loop bug) reproduced the mismatch deterministically on its own,
independent of what garbage (if any) was in `next`.

## Fix: SlabAllocator<T>

`src/allocator.cpp` defines a generic `SlabAllocator<T, ObjectsPerSlab>`:
pre-allocates slabs of `ObjectsPerSlab` fixed-size slots, tracks
free/partial/full slabs, and hands out slots via placement-new
(`T(std::forward<Args>(args)...)`). For a type like `LinkedList` or
`HT_Item` with no user-defined constructor, calling `allocate()` with no
arguments **value-initializes** the object — zeroing it before construction
— which eliminates the uninitialized-`next` class of bug by construction,
instead of relying on the OS happening to hand out zeroed pages.

Wired in:
- `src/hashtable.cpp`: `create_item`/`free_item` now allocate/deallocate
  `HT_Item` via a static `SlabAllocator<HT_Item> ht_item_allocator`
  (previously raw `new`/`delete`).
- `src/linkedlist.cpp`: `allocate_list`/`free_linkedlist` now
  allocate/deallocate `LinkedList` nodes via a static
  `SlabAllocator<LinkedList> linkedlist_allocator` (previously raw
  `malloc`/`free`).
- `free_linkedlist` was also updated to call `free_item(temp->item)` instead
  of `delete temp->item` — items are no longer allocated with plain `new`,
  so mixing in a raw `delete` would have been undefined behavior against the
  slab-allocated memory.

Both `linkedlist.cpp` and `allocator.cpp` needed `#pragma once`: they're
`.cpp` files included textually (templates and the small C-style API need
their full definitions visible at the include site), and without a guard,
`hashtable.cpp`'s include chain pulled each of them in twice, causing
"redefinition" errors.

## Verification

`test_kvstore.py` now passes at both 5000 and 20000 keys:

```
$ python3 test_kvstore.py 5000
Running 17600 commands against ./kvstore (N=5000)...
PASSED: all 17600 commands (5000 keys) behaved correctly.

$ python3 test_kvstore.py 20000
Running 70100 commands against ./kvstore (N=20000)...
PASSED: all 70100 commands (20000 keys) behaved correctly.
```

## Follow-up: SlabAllocator::deallocate() was O(slabs), not O(1)

### Problem

The original `deallocate(T* ptr)` didn't know which `Slab` a pointer belonged
to, so it found out by brute force — `locateSlab` walked the `partialSlabs`
list, then the `fullSlabs` list, checking whether `ptr` fell inside each
slab's `[memoryPool, memoryPool + slabMemoryBounds)` byte range:

```cpp
auto locateSlab = [&](Slab* head) -> Slab* {
    Slab* curr = head;
    while (curr) {
        std::byte* bytePtr = reinterpret_cast<std::byte*>(ptr);
        if (bytePtr >= curr->memoryPool && bytePtr < (curr->memoryPool + slabMemoryBounds)) {
            return curr;
        }
        curr = curr->next;
    }
    return nullptr;
};
```

With `ObjectsPerSlab = 64`, a workload with N live objects spreads across
roughly `N/64` slabs, so a single `deallocate()` call could inspect up to
`N/64` slabs before finding (or failing to find) the right one — an O(number
of slabs) cost on every free. That directly undermines the point of building
a slab allocator in the first place: predictable, cheap allocation/deallocation.

### Fix: back-pointer stored in each slot

Instead of searching for the owning slab, stamp it into the memory itself so
it can be read back in O(1). Each slot's layout changed from:

```
[ T / Node storage ]
```

to:

```
[ SlotHeader { Slab* owner } ][ padding to alignment ][ T / Node storage ]
```

- `SlotHeader` is a one-word struct holding a `Slab*`. It requires only a
  forward declaration of `Slab` (a pointer to an incomplete type is fine).
- `data_offset` is where the actual object/free-list storage begins:
  `sizeof(SlotHeader)` rounded up to `storage_align` (`max(alignof(T), alignof(Node))`),
  so the object itself stays correctly aligned.
- `slot_size` is the *whole* slot (header + padding + storage) rounded up to
  `slot_align`, so slot `i` at `memoryPool + i * slot_size` is aligned too —
  this also incidentally fixed a latent alignment bug: previously `slotSize`
  wasn't guaranteed to be a multiple of the required alignment, so slots
  after the first weren't guaranteed to be properly aligned for an arbitrary
  `T`.

In the `Slab` constructor, each slot's header is stamped once, up front:

```cpp
reinterpret_cast<SlotHeader*>(slotBase)->owner = this;
```

`allocate()` is unchanged — it still pops a free-list `Node` and
placement-news `T` at the same address (the data portion, past the header).

`deallocate()` no longer scans anything — it steps back from the object
pointer to its header and reads the owner directly:

```cpp
std::byte* slotBase = reinterpret_cast<std::byte*>(ptr) - data_offset;
Slab* currentSlab = reinterpret_cast<SlotHeader*>(slotBase)->owner;
```

Everything after that (moving the slab between the full/partial/free lists)
is unchanged, since that logic only needed to know *the* slab, not *find* it.

### Trade-off

The old code also used the `locateSlab` scan to detect "this pointer doesn't
belong to this allocator at all" and print an error. That check is gone now
— ownership is trusted from the header rather than verified against a known
memory range. Passing `deallocate()` a pointer that never came from this
allocator was already undefined behavior before this change (and still is),
so nothing new is broken, but it now fails silently instead of logging an
error first.

Also added the missing `#include <algorithm>` (for `std::max`), which
previously compiled only by accident via a transitive include.

### Verification

Rebuilt and reran the stress test — still fully green, now with zero
compiler warnings (the `std::max` portability warning is gone too):

```
$ python3 test_kvstore.py 20000
Running 70100 commands against ./kvstore (N=20000)...
PASSED: all 70100 commands (20000 keys) behaved correctly.
```

## Benchmarks: slab allocator vs plain new/delete vs std::unordered_map

`src/benchmarks.cpp` measures three things: heap-allocation count under mixed
GET/SET, throughput (read-heavy 5% writes, and mixed 50% writes), and GET/SET
latency percentiles (p50/p90/p99/p99.9/max). Each is run against our
`HashTable` and against `std::unordered_map<std::string, std::string>` for a
side-by-side baseline. Run with `make bench` (builds and runs both
`benchmarks` and `benchmarks_plain` back to back).

### Custom allocator vs plain new/delete

`hashtable.cpp`/`linkedlist.cpp` route `HT_Item`/`LinkedList` allocation
through `SlabAllocator` normally, but compiling with `-DBENCH_PLAIN_ALLOCATOR`
(a new `benchmarks_plain` Makefile target) swaps in plain `new`/`delete`/
`malloc`/`free` instead — same hash table logic, only the allocator differs.

| Metric | Slab allocator | Plain new/delete |
|---|---|---|
| Allocations per op (mixed) | 0.0115 | 0.5000 |
| Throughput, mixed 50% write | 92,575 ops/sec | 84,911 ops/sec |
| SET p99 latency | 24,291 ns | 27,333 ns |
| SET p99.9 latency | 37,792 ns | 59,417 ns |
| SET max latency | 201,250 ns | 268,834 ns |

~43x fewer heap calls with the slab allocator, and consistently better tail
latency, though the median/throughput difference is modest (~9%) — allocation
is only one part of the per-op cost here.

### KVStore vs std::unordered_map: the real bottleneck was the hash function

At this point KVStore was **~90-120x slower** than `std::unordered_map` on
GET/SET latency and throughput, regardless of which allocator it used. That
ruled out the allocator as the dominant cost. The actual cause: `HashFunction`
was `sum of character codes mod CAPACITY` — the same weak hash whose
collisions we'd already found manually (anagrams like `ab`/`ba` collide
outright; dense key sets like `key0`..`key20000` collide constantly, per
`NOTES.md` above). Every GET/SET was walking long collision chains on top of
a raw array scan, while `std::unordered_map` stays near O(1) with a much
stronger hash and proper rehashing.

### Fix: replace HashFunction with xxhash

```cpp
unsigned long HashFunction(const std::string& str){
    return static_cast<unsigned long>(XXH3_64bits(str.data(), str.size())) % CAPACITY;
};
```

`#define XXH_INLINE_ALL` before `#include "xxhash.h"` inlines the whole
implementation from the header, so no linking against `libxxhash` is needed
— just the include path (`brew install xxhash`, then
`-I$(brew --prefix xxhash)/include`, added to `CXXFLAGS` in the `Makefile`).

Result — throughput up ~65x, latency down ~20x, closing nearly the entire
gap with `std::unordered_map`:

| Metric | Old hash (sum-of-chars) | xxhash | std::unordered_map |
|---|---|---|---|
| Mixed throughput | 92,575 ops/sec | **5,981,085 ops/sec** | 8,257,851 ops/sec |
| GET p50 | 1,791 ns | **83 ns** | 42 ns |
| SET p50 | 9,000 ns | **125 ns** | 83 ns |
| Allocations per op (mixed) | 0.0115 | **0.0002** | 0.1076 |

KVStore went from ~90-120x slower than `std::unordered_map` to roughly
1.4-2x slower. The allocation count also dropped further with xxhash, since
fewer collisions means fewer `LinkedList` overflow-node allocations too.

### Verification

```
$ python3 test_kvstore.py 20000
Running 70100 commands against ./kvstore (N=20000)...
PASSED: all 70100 commands (20000 keys) behaved correctly.
```

### Lesson

The custom slab allocator was a genuine, measurable win on its own narrow
axis (allocation count, tail latency) — but it was optimizing a small piece
of a much slower whole. The bottleneck that actually mattered (weak hash
function causing long collision chains) was found only by benchmarking
against a real baseline (`std::unordered_map`), not by looking at the
allocator in isolation. Profile the whole pipeline before assuming which
piece is slow.

## Remaining gap after xxhash: default-construct-then-assign in create_item

Even after the hash fix, KVStore was still measurably behind
`std::unordered_map` (see "why is std::unordered_map still performing
better" investigation). One concrete, fixable piece of that gap: `create_item`
was allocating an `HT_Item` with `ht_item_allocator.allocate()` (no args) —
which default-constructed two empty `std::string`s via `SlabAllocator`'s
`T(std::forward<Args>(args)...)` with an empty arg pack — and then
copy-assigning the real key/value over them:

```cpp
HT_Item* item = ht_item_allocator.allocate();  // default-constructs 2 empty strings
item->key = key;                                // then copy-assigns over them
item->value = value;
```

That's two constructions plus two assignments per insert, versus
`std::unordered_map`'s single in-place construction of its key/value pair.

### Fix: give HT_Item a real 2-arg constructor, construct once

```cpp
// include/hashtable.hpp
typedef struct HT_Item {
    std::string key;
    std::string value;
    HT_Item(std::string key, std::string value);
} HT_Item;

// src/hashtable.cpp
HT_Item::HT_Item(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}

HT_Item* create_item(const std::string& key, const std::string& value){
    return ht_item_allocator.allocate(key, value);
}
```

`SlabAllocator::allocate(Args&&... args)` already forwards its arguments into
a single placement-new (`T(std::forward<Args>(args)...)`), so passing
`key`/`value` straight through now constructs the `HT_Item` exactly once,
directly with the real values.

Two mistakes surfaced (and got fixed) on the way to this:
- Declaring the 2-arg constructor suppresses the compiler-generated default
  constructor. The `BENCH_PLAIN_ALLOCATOR` build path (`new HT_Item()`) was
  still calling that now-nonexistent default constructor, and needed to
  become `new HT_Item(key, value)` — plus it was missing a `return item;`,
  falling off the end of a non-`void` function.
- The constructor itself takes `k`/`v` **by value** (an unavoidable copy
  from the caller's `const std::string&`), so the initializer list needs
  `key(std::move(k)), value(std::move(v))` rather than `key(k), value(v)` —
  otherwise it copies each string a second time instead of moving out of
  the already-copied parameter.
- Also: the declared constructor initially had no *definition* anywhere,
  which compiled fine (declarations are enough for the compiler) but failed
  to *link* — a reminder that `g++ -c` (compile-only) won't catch a missing
  function body; only the final link step will.

### Verification and measured improvement

Stress test still green at 20,000 keys. Throughput improved further on top
of the xxhash fix — construction overhead was a real, separate cost from
hashing:

| Metric | After xxhash fix, before constructor fix | After constructor fix |
|---|---|---|
| Read-heavy throughput | 6,805,103 ops/sec | **10,275,776 ops/sec** |
| Mixed throughput | 5,981,085 ops/sec | **7,549,603 ops/sec** |
| Allocations per op (mixed) | 0.0002 | 0.0002 (unchanged — same allocator) |

For comparison, `std::unordered_map` on the same run: 11,854,632 ops/sec
read-heavy, 8,455,855 ops/sec mixed. KVStore now sits at roughly **87-89% of
`std::unordered_map`'s throughput**, up from ~1.4-2x slower before this fix
and ~90-120x slower before the xxhash fix.

```
$ python3 test_kvstore.py 20000
Running 70100 commands against ./kvstore (N=20000)...
PASSED: all 70100 commands (20000 keys) behaved correctly.
```

## Unified buckets: dropping the items[] / overflow_buckets[] split

Even after the xxhash and constructor fixes, KVStore was still behind
`std::unordered_map` by ~11-13%. The remaining structural cause (see the
"why is std::unordered_map still performing better" investigation): the
`HashTable` had **two separate `CAPACITY`-sized arrays** —
`items[]` (a "primary slot" per bucket) and `overflow_buckets[]` (a
`LinkedList*` chain for everyone else per bucket) — 781 KB fixed, regardless
of how many keys were actually stored, versus `std::unordered_map`'s single
bucket array sized to the real element count (~201 KB at N=20000). Every
lookup that missed the primary slot had to also touch a second,
separately-allocated array, and every function (`ht_insert`/`ht_search`/
`ht_delete`) had to special-case "is this the primary item, or do I walk the
overflow chain instead."

### Fix: one array, every bucket is just a chain

```cpp
// include/hashtable.hpp
typedef struct HashTable{
    LinkedList** buckets;   // buckets[index] is the head of that bucket's chain (NULL if empty)
    int size;
    int count;
} HashTable;
```

A bucket with one item is now just a chain of length 1 — no distinction
between "the primary item" and "an overflow item," matching how
`std::unordered_map` itself does separate chaining internally.

New building block, `linkedlist_delete` (`src/linkedlist.cpp`), mirrors
`linkedlist_insert`'s style — unlink the node matching `key` from a chain,
free it, and return the (possibly new) head:

```cpp
LinkedList* linkedlist_delete(LinkedList* list, const std::string& key){
    if (!list) return list;
    if (list->item->key == key) {
        LinkedList* next = list->next;
        free_list_node(list);
        return next;
    }
    LinkedList* prev = list;
    LinkedList* curr = list->next;
    while (curr) {
        if (curr->item->key == key) {
            prev->next = curr->next;
            free_list_node(curr);
            return list;
        }
        prev = curr;
        curr = curr->next;
    }
    return list;  // key not found; unchanged
}
```
(`free_list_node` was factored out of `free_linkedlist` so the
allocator-toggling `#ifdef BENCH_PLAIN_ALLOCATOR` block for freeing a single
node only exists in one place.) Verified in isolation first, against a
standalone harness covering middle/head/last-node/empty/not-found deletion
cases, before wiring it into `hashtable.cpp` — this caught nothing broken in
`linkedlist_delete` itself, which meant later compile/logic errors were
correctly localized to `hashtable.cpp`'s integration of it.

`hashtable.cpp` then collapsed onto this single-array model:
- **`ht_insert`**: walk the bucket's chain once — update `value` in place if
  the key's found, otherwise `create_item` + `linkedlist_insert`, capturing
  the returned head back into `table->buckets[index]`.
- **`ht_search`**: unchanged in spirit, already walked `buckets[index]`'s
  chain.
- **`ht_delete`**: check existence via `ht_search` first (avoids
  dereferencing a possibly-NULL bucket), then
  `table->buckets[index] = linkedlist_delete(table->buckets[index], key)`.
- **`free_table`**: collapsed to `free_buckets(table); free(table);` —
  `free_buckets` already frees every item reachable from each bucket's
  chain, so the old separate `items[]`-walking loop was redundant.
- **`print_table`**: fixed to walk each bucket's *full* chain instead of
  only printing the first item (previously silently dropped collided keys
  from the printout).
- **`handle_collision`** removed entirely (from both `.cpp` and `.hpp`) —
  with every bucket uniformly a chain, "insert into an empty vs. non-empty
  bucket" became the same code path, so the function had no remaining
  purpose. Confirmed nothing else referenced it before deleting.

### Bugs hit and fixed along the way

The migration from two arrays to one went through a rough intermediate
state with several real bugs, caught by compiling and by re-running
`test_kvstore.py` after each attempt rather than assuming a "looks right"
diff was correct:

- Several spots still referenced the now-nonexistent `table->items` field
  (compile errors).
- `HT_Item* curr_item = table->buckets[index];` — type mismatch, treating a
  `LinkedList*` as an `HT_Item*` (compile error).
- `linkedlist_insert`'s and `linkedlist_delete`'s return values were
  discarded in a couple of call sites instead of being reassigned back to
  `table->buckets[index]` — meaning newly-inserted items in an empty bucket
  were silently lost (never linked into the table), and an update-path
  could free an item it had just linked into the chain (use-after-free).
- `ht_delete` did `table->buckets[index]->item` before checking whether
  `buckets[index]` was `NULL` — crashed on deleting a key that hashed to an
  empty bucket.
- A commented-out `else if (...) { ... }` block left its closing brace
  commented out while the code inside stayed live, unbalancing the braces
  and turning a later `return false;` into a dangling statement outside any
  function (compile error).

### Verification and measured improvement

Stress test green at 20,000 keys, `PRINT`/collision spot-checks correct,
memory footprint back to a single `CAPACITY`-sized array (~391 KB instead of
781 KB). Throughput not only closed the remaining gap with
`std::unordered_map`, it went past it on this workload:

| Metric | Before (two arrays) | After (unified buckets) | std::unordered_map |
|---|---|---|---|
| Read-heavy throughput | 10,275,776 ops/sec | **12,311,196-13,474,100 ops/sec** | 11,340,869-11,847,990 ops/sec |
| Mixed throughput | 7,549,603 ops/sec | **8,283,975-9,713,512 ops/sec** | 8,377,957-8,667,170 ops/sec |
| Allocations per op (mixed) | 0.0002 | **0.0000** | 0.1076 |
| GET/SET p50/p90/p99/p99.9 | slightly behind unordered_map | **essentially tied** with unordered_map at every percentile | — |

Confirmed stable across repeated runs, not a one-off — KVStore now matches
or slightly beats `std::unordered_map` on this benchmark, up from ~87-89%
of its throughput before this refactor and ~90-120x slower before the
xxhash fix at the start of this investigation.

```
$ python3 test_kvstore.py 20000
Running 70100 commands against ./kvstore (N=20000)...
PASSED: all 70100 commands (20000 keys) behaved correctly.
```

# ShardedHashTable: a concurrent, sharded wrapper around HashTable

## Motivation and design

`HashTable` has no synchronization at all — fine for the single-threaded
`kvstore` CLI, unsafe for concurrent access. `ShardedHashTable` wraps N
independent `HashTable*` instances ("shards"), each with its own
`std::shared_mutex`. A key is routed to a shard via
`HashFunction(key) % shard_count`; `Get` takes a shared (read) lock, `Set`/
`Delete` take an exclusive (write) lock — so operations on *different*
shards never block each other, and multiple concurrent `Get`s on the *same*
shard don't block each other either.

Critically, each shard's `HashTable` also owns its **own**
`SlabAllocator<HT_Item>`/`SlabAllocator<LinkedList>` (via new `item_allocator`/
`list_allocator` fields added to `HashTable` itself). Earlier drafts tried
giving `Shard` its own allocators instead, or worse, made them `static`
(one instance shared across every `Shard`) — both wrong. The allocators
needed to live on `HashTable`, because every `HashTable`-touching function
(`create_item`, `free_item`, `allocate_list`, `linkedlist_insert`,
`linkedlist_delete`, `free_linkedlist`, `free_list_node`) already receives
`HashTable* table`, and every one of those calls happens only while holding
that shard's mutex. That means the allocators don't need any locking of
their own — the shard's mutex is the only synchronization required, and
each shard's allocator is only ever touched by whichever thread currently
holds that shard's lock.

## Building stage: what went wrong across many iterations, and why

This went through roughly six rounds of "attempt a fix → recompile → find
the next error" before it built cleanly. Rather than transcribe every
round, here's every distinct category of bug hit, since several recurred
in slightly different forms:

**Syntax-level, from the very first sketch:**
- Missing semicolons after `struct Shard{...}` and `class ShardedHashTable{...}`.
- Missing `#include <shared_mutex>`, `<vector>`, `<memory>`, `<optional>`
  (including one literal typo: `#include <shared mutex>` — a space instead
  of an underscore, which is a different, invalid header name entirely).
- A constructor declared as `ShardedHashTable(int num_shards=16, HashTable* table)`
  — a required parameter after a defaulted one, which C++ doesn't allow.

**`Shard` construction and lifetime:**
- `shards(num_shards)` in the constructor's initializer list *value-constructs*
  `num_shards` shards (all null `map` pointers), and then the constructor
  body *also* `push_back`s `num_shards` real ones — doubling the vector to
  `2 * num_shards` entries, with every real shard sitting in the
  unreachable second half (since `get_shard_index` still mods by the
  original `shard_count`). Every `Get`/`Set`/`Delete` would have
  dereferenced a null shard pointer. Fixed by only `reserve()`-ing capacity
  and `push_back`-ing exactly once per shard.
- `Shard` contains a `std::shared_mutex`, which is neither copyable nor
  movable — `std::vector<Shard>` can't be grown/reallocated normally.
  Settled on `std::vector<std::unique_ptr<Shard>>` (the vector only ever
  moves pointers, never the mutex itself) with `reserve()` + `emplace_back`
  used exactly `shard_count` times.
- A destructor written as `Shard::~Shard() { ... }` *nested inside*
  `ShardedHashTable`'s class body — invalid; an out-of-class qualified
  definition must live at namespace scope. Fixed by defining `~Shard()`
  directly inside `struct Shard { ... }`'s own body instead.
- Per-shard allocator members were briefly declared `static` — meaning one
  instance shared across *every* `Shard`, exactly the global-singleton
  problem this whole exercise was trying to escape, just renamed. Removed
  entirely once the allocators moved onto `HashTable` itself (see Design,
  above) — `Shard` ended up not needing any allocator fields of its own.
- Member name mismatches between where a field was declared
  (`ht_item_allocator`) and where it was used (`item_allocator`) — plain
  typos, but they only surface as compiler errors, so each one cost a
  full recompile cycle to catch.

**API/ownership churn:**
- The free function `create_table(int size)` was briefly repurposed to
  return `ShardedHashTable*` instead of `HashTable*` — which conflicts with
  its existing declaration in `hashtable.hpp` (`main.cpp`/`benchmarks.cpp`
  both call it expecting a `HashTable*`), and also with `ShardedHashTable`'s
  own constructor, which calls `create_table(...)` and assigns the result
  to a `HashTable* map` member. Reverted `create_table` to its original
  contract; `ShardedHashTable` just calls it once per shard like any other
  caller.
- `SlabAllocator` moved from `src/allocator.cpp` to `include/allocator.hpp`
  (needed so `HashTable` could hold typed `SlabAllocator<HT_Item>*`/
  `SlabAllocator<LinkedList>*` members) — but `allocator.cpp` was left
  behind and kept being `#include`d from both `hashtable.cpp` and
  `linkedlist.cpp` on top of the new header, redefining `SlabAllocator`.
  Deleted `allocator.cpp`, switched every include to the header.
- `allocator.hpp` itself had no include guard at all. Harmless while it was
  reached through exactly one path, but once `hashtable.hpp`,
  `hashtable.cpp`, and `linkedlist.cpp` all ended up including it (some
  directly, some transitively), it redefined `SlabAllocator` against
  itself. Added `#pragma once`.
- `ShardedHashTable` was declared in the header but then the **entire
  class body was pasted again** into `hashtable.cpp` — an outright
  redefinition. Fixed with the standard header/`.cpp` split: header
  forward-declares `struct Shard;` and declares the class's member
  functions; `.cpp` provides the full `Shard` definition plus every method
  body qualified with `ShardedHashTable::`.
- One fix attempt changed `ht_search`'s return type to
  `std::optional<std::string>`, to make it "return by copy instead of
  pointer" — the right idea, wrong layer. `ht_search` is the low-level,
  single-table, unsynchronized primitive that `benchmarks.cpp` calls
  directly in three places expecting a raw `std::string*`; changing its
  signature broke both that and the header declaration. The copy-out
  belongs in `ShardedHashTable::Get` (copy `*v` into the `std::optional`
  while still holding the shard's lock), not in `ht_search` itself.
  Reverted `ht_search`, kept the copy-out only in `Get`.
- `HashTable` referenced by pointer in prototypes (`allocate_list(HashTable* table)`
  etc.) before it was even forward-declared in the header — added
  `typedef struct HashTable HashTable;` near the top.
- The biggest mechanical piece: threading `HashTable* table` through
  `create_item`, `free_item`, `allocate_list`, `linkedlist_insert`,
  `linkedlist_delete`, `free_linkedlist`, and the internal
  `free_list_node` helper, so each one reaches *its own table's*
  allocators instead of a bare (and, once the old globals were removed,
  literally undeclared) identifier. This took several passes because
  individual call sites kept getting missed — e.g. `free_list_node` gained
  the `table` parameter before its own body was updated to actually pass
  `table` into `free_item`, `linkedlist_insert` gained the parameter before
  its three internal `allocate_list()` calls were updated to
  `allocate_list(table)`, and so on.

## Testing stage: a real, silent bug that only running it caught

Once everything compiled, a standalone test was written exercising
`ShardedHashTable` directly (basic correctness across 1000 keys,
update-in-place, delete, missing-key lookups, then an 8-thread ×
5000-ops-each concurrent stress test hammering `Set`/`Get`/`Delete`
simultaneously).

**First run segfaulted** — but only after every operation had already
printed correct results, during process cleanup. `lldb`'s backtrace
pinned it to `SlabAllocator<HT_Item>::Slab::~Slab()`, inside macOS
malloc's own corruption detector (`free_tiny_botch`) — meaning the heap
had already been corrupted earlier, and this was just where it got
*detected*, not where it happened.

**Root cause**: `HashFunction` unconditionally modded its result by the
global `CAPACITY` constant (50000):
```cpp
unsigned long HashFunction(const std::string& str){
    return static_cast<unsigned long>(XXH3_64bits(str.data(), str.size())) % CAPACITY;
};
```
`ht_insert`/`ht_search`/`ht_delete` used that result directly as an index
into `table->buckets[index]`. That's only safe when a table's actual
`size` equals `CAPACITY` — true for every table ever built before this
(`main.cpp`, `benchmarks.cpp`, `test_kvstore.py` all go through
`create_table(CAPACITY)`), so this had been a **dormant** bug the entire
time, never exercised. `ShardedHashTable` was the first thing to ever
build a table with a *different* size — `create_table(CAPACITY / shard_count)`,
e.g. 3125 buckets for 16 shards. `HashFunction` still returned indices up
to 49999, so roughly 94% of inserts wrote past the end of a 3125-element
`buckets` array — silently corrupting whatever heap memory happened to sit
after it (in this run, a `SlabAllocator` slab), with no crash until that
corrupted memory was eventually freed.

**Fix**: `HashFunction` now returns the raw hash with no modulo baked in;
each caller (`ht_insert`/`ht_search`/`ht_delete`) mods by `table->size`
instead of the global constant:
```cpp
unsigned long index = HashFunction(key) % table->size;
```

**Re-verification after the fix:**
- Base `HashTable`/`kvstore` stress test still green at 20,000 keys —
  confirms this change didn't regress the (already-correct, since
  `size == CAPACITY` there) unsharded path.
- The `ShardedHashTable` functional + concurrency test passed cleanly
  across 5 repeated runs.
- Also run under ThreadSanitizer (`-fsanitize=thread`) — zero data races
  reported across the 8-thread concurrent section, corroborating that the
  per-shard-mutex + per-table-allocator design is actually race-free, not
  just "didn't happen to crash this time."

## Benchmarks: `src/benchmarks_sharded.cpp`

A new file (wired into the `Makefile` as `benchmarks_sharded`, needs
`-pthread`) with three benchmarks, run on an 8-core machine
(`std::thread::hardware_concurrency() == 8`):

**1. Throughput vs. shard count, fixed at 8 threads, 50% writes** — shows
sharding actually working: more shards means less lock contention per
shard.
```
shards=1  : 336,635 ops/sec
shards=2  : 520,754 ops/sec
shards=4  : 864,919 ops/sec
shards=16 : 2,902,726 ops/sec
shards=64 : 7,892,984 ops/sec
```

**2. Throughput vs. thread count, fixed at 16 shards, 50% writes** — the
counter-intuitive result, confirmed reproducible across 3 runs:
```
threads=1 : 7,785,459 – 7,882,563 ops/sec
threads=2 : 6,171,268 – 6,378,492 ops/sec
threads=4 : 5,628,808 – 5,652,798 ops/sec
threads=8 : 3,373,532 – 3,415,796 ops/sec
```
Throughput *decreases* as thread count increases, at fixed shard count.
This isn't a bug — it's the direct consequence of benchmark 1: 16 shards
provides nowhere near enough parallelism for 8 threads doing a 50%-write
workload, since `Set`/`Delete` take an *exclusive* per-shard lock that
blocks every other thread targeting that same shard. More threads
competing for the same 16 locks means more time spent blocked, not more
useful work done. The shard-count sweep above confirms this directly: at
the same 8 threads, 64 shards deliver ~2.7x the throughput of 16 shards.
The takeaway: shard count needs to scale with expected concurrency and
write ratio — there's no single "right" shard count independent of the
actual workload.

**3. Get/Set latency percentiles under contention (8 threads, 16 shards)**:
```
GET  p50=125ns   p90=458ns   p99=62,500ns   p99.9=150,709ns  max=327,708ns
SET  p50=84ns    p90=208ns   p99=41,875ns   p99.9=108,959ns  max=275,416ns
```
The median latencies are fast (consistent with the uncontended,
single-threaded numbers measured earlier for the base `HashTable`), but
the tail is dramatically worse — p99 is ~500-750x the median, directly
quantifying the cost of lock contention under concurrent load with too
few shards for the thread count. This is the same underlying effect as
benchmark 2, seen from the latency side instead of the throughput side.
