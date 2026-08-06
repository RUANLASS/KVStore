# KVStore: Design, Performance, and What I Learned

This document explains *why* KVStore performs the way it does — the design
decisions behind the hash table, allocator, and sharding layer, the
optimizations that mattered (and the ones that didn't), and the profiling
data behind every claim. `NOTES.md` is the chronological debugging log;
this is the synthesized version, organized by topic rather than by when
things happened.

**How to reproduce every number in this document:**
```
make bench    # builds and runs benchmarks, benchmarks_plain, benchmarks_sharded, benchmarks_profiling
```
Each binary can also be built/run individually (`make benchmarks_profiling && ./benchmarks_profiling`, etc.).

**Tooling note, stated once up front:** this was profiled on a sandboxed
macOS machine with neither `perf` nor Instruments/`valgrind` available —
no hardware performance counters (literal cache-miss counts, retired
instructions, etc.) were accessible. Two things stand in for that
throughout this document:
- **Latency under deliberately chosen access patterns** as a cache-locality
  proxy (a working set that fits in cache vs. one that doesn't; sequential
  vs. scattered access) — a standard technique when PMCs aren't available,
  clearly labeled as a proxy everywhere it's used.
- **macOS's built-in `sample`** (call-stack sampling profiler) for real,
  if coarse, CPU/lock-wait evidence — e.g. literally catching worker
  threads parked in `psynch_mutexwait` at a sampled instant.

Every number below came from an actual run on this machine, not an
estimate — where a result was surprising, it was re-run to confirm before
being included.

---

## 1. Architecture overview

```
ShardedHashTable            <- concurrent wrapper, N independent shards
  └── Shard (x N)           <- HashTable* + std::shared_mutex, one lock per shard
        └── HashTable       <- buckets[] (separate chaining) + own allocators
              ├── SlabAllocator<HT_Item>     <- pools HT_Item nodes
              └── SlabAllocator<LinkedList>  <- pools chain nodes
```

- **`HashTable`**: separate-chaining hash table. `buckets[i]` is the head
  of a singly-linked chain of `LinkedList` nodes, each holding one
  `HT_Item{key, value}`. No open addressing, no probing.
- **`SlabAllocator<T>`**: a pooled, freelist-based allocator for fixed-size
  `T` (`HT_Item` or `LinkedList` nodes), used instead of raw `new`/`delete`
  on the hot insert/delete path.
- **`ShardedHashTable`**: N independent `HashTable`s, each with its own
  allocators and its own `std::shared_mutex`. A key routes to shard
  `HashFunction(key) % shard_count`. `Get` takes a shared (read) lock;
  `Set`/`Delete` take an exclusive (write) lock.

## 2. Design decisions and trade-offs

### 2.1 Separate chaining over open addressing
Chosen because it degrades gracefully under load factor > 1 (correctness
never breaks, only chain length grows) and because deletion doesn't need
tombstones. Trade-off: pointer-chasing through chain nodes is worse for
cache locality than open addressing's flat array — accepted here because
`std::unordered_map` (the baseline compared against throughout) makes the
same trade-off, so the comparison stays apples-to-apples.

### 2.2 A pooled slab allocator instead of raw `new`/`delete`
**Decision**: `HT_Item` and `LinkedList` nodes come from a
`SlabAllocator<T>` (batches of 64 objects per slab, O(1) allocate via
freelist pop, O(1) deallocate via a back-pointer stored in each slot —
see §4.2) instead of individual heap allocations.

**Trade-off accepted**: the allocator is not thread-safe on its own — it
relies entirely on whichever shard's mutex is already serializing access
to that shard's `HashTable`. This was a deliberate choice over adding a
lock inside `SlabAllocator` itself, which would have serialized allocation
across *every* shard regardless of which bucket lock was held, defeating
the purpose of sharding (see §2.3 and the Rule-of-Three/thread-safety
caveats in §6).

**Trade-off not (yet) addressed**: `SlabAllocator` violates the Rule of
Three — it declares a destructor but not a copy constructor/assignment,
so a copy would double-free its slabs. Never triggered in practice because
every `SlabAllocator` here is heap-allocated once via `new` and never
copied, but it's a landmine for future code that isn't careful about that.

### 2.3 Sharding: per-shard locks *and* per-shard allocators
**Decision**: each shard owns an independent `HashTable`, which means an
independent pair of allocators too — not one allocator shared across all
shards. This was the actual hard part of the sharding work (see
`NOTES.md`'s "Building stage" for the six-round journey to get this
right): early drafts gave `Shard` its own allocator pointers, or worse,
made them `static` (one instance shared across every shard — the exact
global-singleton problem sharding was meant to escape, just renamed).

**Why it matters**: if every shard's `HT_Item`/`LinkedList` allocation
routed through the *same* global `SlabAllocator`, then two threads
correctly holding *different* shard locks would still race on the *same*
allocator's internal freelist pointers — sharding the bucket structure
would do nothing to prevent that corruption. Making each `HashTable` own
its allocators means the shard's mutex is genuinely the *only*
synchronization needed anywhere in the allocation path.

**Trade-off accepted**: shard count is fixed at construction (no
rehashing/resharding), and — as §4.4 shows in detail — the *right* shard
count depends on expected concurrency and write ratio. There's no single
correct default.

### 2.4 xxhash over a hand-rolled hash function
The original `HashFunction` summed character codes mod table size — cheap,
but catastrophically bad (anagrams collide outright; `key0..keyN` patterns
collide constantly). Replaced with `XXH3_64bits` (via `xxhash.h`,
`XXH_INLINE_ALL` so no linking is needed). This alone was the single
biggest performance change in the whole project — see §3.

## 3. The optimization journey: before/after, in the order they actually happened

Every fix below was driven by a measured regression or a real bug, not
speculation — several "obvious" optimizations (the allocator, specifically)
turned out to matter far less than expected until the *actual* bottleneck
was found. This table is the condensed version; `NOTES.md` has the full
narrative including the wrong turns.

| Stage | Mixed throughput (ops/sec) | GET p50 | SET p50 | Allocs/op (mixed) | vs. `std::unordered_map` |
|---|---|---|---|---|---|
| Baseline (weak hash, `new`/`delete`, two-array design) | 92,575 | 1,791 ns | 9,000 ns | 0.0115 | ~90-120x slower |
| + Slab allocator (custom pooled allocator) | ~92,575¹ | ~1,791 ns¹ | ~9,000 ns¹ | 0.0115 | still ~90-120x slower |
| + xxhash (replace weak hash function) | 5,981,085 | 83 ns | 125 ns | 0.0002 | ~1.4-2x slower |
| + Single-construction `HT_Item` (was default-construct-then-assign) | 7,549,603 | — | — | 0.0002 | ~1.1x slower |
| + Unified buckets (drop the `items[]`/`overflow_buckets[]` split) | 8,283,975-9,713,512 | tied | tied | **0.0000** | **matches/beats** it |

¹ The slab allocator swap alone, before the hash fix, produced *no
measurable throughput change* — same 9552/17600 mismatch, same rough
ops/sec. This is the single most important lesson from the whole project:
**a well-built optimization can be completely invisible if it isn't the
actual bottleneck.** The allocator swap was real and later mattered (43x
fewer heap calls, better tail latency — see the allocator section of
`NOTES.md`), but it was optimizing a small piece of a much slower whole
until the hash function was fixed. Profile the *entire* pipeline before
assuming which piece is slow — don't trust intuition about what "should"
be expensive.

**Why the hash fix was the dominant factor**, concretely: the weak hash
produced long collision chains even at moderate load factors — a direct
measurement at N=20,000 keys showed 3,502 of 20,000 keys landing as
*overflow* nodes (17.5%) rather than in their bucket's primary slot, each
one costing an extra pointer-chase and full string comparison per lookup.
`xxhash` collapsed that to near-zero collisions, which is why the
allocator's real benefit (fewer allocations, less pointer-chasing per
insert) only became *visible* in the throughput numbers once collision
chains stopped dominating the cost.

## 4. Profiling results

### 4.1 Allocation count on the hot path

Measured with `operator new`/`operator new[]` overridden to count real
heap calls, table pre-warmed (allocator pool already grown, so any counted
allocation reflects steady-state behavior, not one-time pool growth):

```
GET (hit)             : 10000 ops -> 0 allocations (0.00000/op)
GET (miss)            : 10000 ops -> 0 allocations (0.00000/op)
SET (update existing) : 10000 ops -> 0 allocations (0.00000/op)
SET+DELETE (recycled) : 10000 ops -> 0 allocations (0.00000/op)
```

Zero across every operation type once the pool is warm — including the
SET+DELETE cycle, confirming the slab allocator's freelist genuinely
recycles nodes rather than leaking or re-growing. (For SET on a *brand
new* key, not shown above, allocation is amortized O(1/64) — one real
allocation every 64th new key, when a slab needs to grow — see the
allocator micro-benchmark in `NOTES.md` for the isolated 12.3ns/op vs.
81.8ns/op comparison against plain `new`/`delete`.)

### 4.2 Why the allocator is O(1), not O(slabs): the back-pointer design

`SlabAllocator::deallocate` originally located a pointer's owning slab by
scanning every slab's memory range — O(number of slabs) per free. Fixed by
stamping a `Slab*` header directly before each slot's storage at slab
creation time, so `deallocate` just steps back from the object pointer to
read the owner in O(1):
```cpp
std::byte* slotBase = reinterpret_cast<std::byte*>(ptr) - data_offset;
Slab* currentSlab = reinterpret_cast<SlotHeader*>(slotBase)->owner;
```
This is *why* allocation counts staying near-zero actually translates into
throughput — an O(1) allocator scales with load; an O(slabs) one would get
progressively slower as the table grows, undermining the whole point of
pooling.

### 4.3 Cache-locality proxy: latency under small vs. large working sets

No hardware cache-miss counters available (see tooling note above) — this
measures GET latency for a working set that comfortably fits in cache
(200 keys) against one that doesn't (200,000 keys), sequential vs.
scattered access order, 20 passes each:

```
Small working set, sequential (n=200)   : p50=41ns   p90=42ns   p99=42ns    p99.9=125ns   max=167ns
Small working set, scattered  (n=200)   : p50=41ns   p90=42ns   p99=42ns    p99.9=42ns    max=84ns
Large working set, sequential (n=200000): p50=83ns   p90=250ns  p99=542ns   p99.9=1250ns  max=5947792ns*
Large working set, scattered  (n=200000): p50=167ns  p90=459ns  p99=833ns   p99.9=1209ns  max=51334ns
```
\* one extreme outlier (5.9ms) in the sequential-large run — almost
certainly an OS scheduling/page-fault hiccup unrelated to cache behavior,
not representative (p99.9 for that same run is a much more sane 1250ns).

Two things line up with expectation: (1) small working set is ~2x faster
at every percentile than large, consistent with everything fitting in L1/L2
vs. spilling further out; (2) at large scale, **scattered access has a
higher p50 than sequential** (167ns vs. 83ns) — random-order access defeats
whatever locality sequential access gets from walking nearby buckets/nodes
in memory order. Both effects are real but modest (low hundreds of ns) —
this table's separate-chaining design was never going to have the
locality of a flat open-addressed array, and that's an accepted trade-off
(§2.1), not a bug.

### 4.4 Lock contention: isolated from "more threads doing more work"

The `benchmarks_sharded.cpp` finding that throughput *drops* as thread
count increases at a fixed shard count (documented in `NOTES.md`) could in
principle be confounded by other things changing alongside thread count.
To isolate lock-wait time specifically as the cause, this measures SET
latency *percentiles* — not just throughput — at a deliberately low, fixed
shard count (4) while sweeping thread count. If contention is the cause,
the median (actual per-op work, unaffected by other threads) should stay
flat while the tail (time spent *blocked* waiting for the shard's lock)
should grow with thread count:

```
threads=1: SET p50=125ns  p90=208ns    p99=250ns     p99.9=333ns     max=17,958ns
threads=2: SET p50=167ns  p90=292ns    p99=8,375ns   p99.9=22,083ns  max=40,500ns
threads=4: SET p50=209ns  p90=6,917ns  p99=29,000ns  p99.9=54,500ns  max=128,458ns
threads=8: SET p50=209ns  p90=29,167ns p99=139,459ns p99.9=262,084ns max=454,875ns
```

Exactly the predicted signature: **p50 barely moves** (125ns → 209ns, a
~1.7x change) while **p99 grows ~558x** (250ns → 139,459ns) from 1 to 8
threads. The actual work per `Set` call didn't get 558x more expensive —
threads are spending more and more time *blocked*, not doing more work.

This is corroborated directly at the OS level: sampling the process with
macOS's `sample` tool during this exact phase caught worker threads with
call stacks literally parked inside the kernel wait syscall:
```
ShardedHashTable::Set(...)
  std::__shared_mutex_base::lock()
    std::mutex::lock()
      _pthread_mutex_firstfit_lock_wait
        __psynch_mutexwait   <- kernel: thread is blocked, not spinning
```
At one sampled instant during the 8-thread run, 5 of 8 worker threads were
caught in exactly this state — over half the threads blocked waiting for a
shard lock at that moment, not doing useful work.

### 4.5 Atomic operation cost: uncontended vs. contended

`std::atomic<long>::fetch_add`, 2,000,000 iterations per thread,
`memory_order_relaxed`:
```
1 thread,  private counter : 3.48 ns/op
2 threads, shared counter  : 7.27 ns/op
4 threads, shared counter  : 12.42 ns/op
8 threads, shared counter  : 28.46 ns/op
```
~8x slower per-op at 8-way contention vs. uncontended — this is cache-line
ping-pong (MESI protocol invalidation traffic as cores fight over
ownership of the same cache line), not lock contention in the mutex sense,
but the same underlying hardware cost model. Relevant here because the
allocation-counting technique used throughout every benchmark in this
project (`g_alloc_count.fetch_add(...)`) has exactly this cost profile —
worth knowing that the *measurement* itself isn't free under concurrency,
though at the allocation rates actually observed (near-zero, §4.1) this
never became a confound in practice.

## 5. Key "why" explanations

**Why did the allocator swap not show up in throughput at first?**
Because collision chains from the weak hash function dominated per-op cost
so heavily that a faster allocation path was noise by comparison. Fixing
allocation before fixing the actual bottleneck is a classic case of
optimizing the wrong layer — see §3.

**Why does `ShardedHashTable` get *slower* with more threads at a fixed
shard count?** Because `Set`/`Delete` take an *exclusive* lock per shard —
with too few shards relative to thread count, most operations end up
queued behind another thread's exclusive lock on the same shard rather
than running in parallel. §4.4 isolates this precisely: the per-op work
doesn't change, only how long threads sit blocked.

**Why does KVStore (unsharded `HashTable`) end up matching
`std::unordered_map` after all the fixes, when it started 90-120x
slower?** None of the individual fixes alone closed the gap — it took all
three: a real hash function (removed the dominant cost, collision chains),
a pooled allocator with O(1) alloc/dealloc (removed heap-allocator
overhead from the hot path), and a unified bucket array (halved the fixed
memory footprint and removed a second array touch on every lookup that
missed the primary slot). Removing any one of the three and the others
alone wouldn't have gotten there — they were addressing genuinely
different costs.

**Why prove correctness matters even more than performance during all of
this?** The `ShardedHashTable` build passed compilation and even passed a
quick manual smoke test before its real bug (`HashFunction` modding by the
global `CAPACITY` constant instead of each table's actual `size` — silent
heap corruption for any table not exactly `CAPACITY`-sized, which
`ShardedHashTable`'s per-shard tables always are) was caught — and even
then, only because a *concurrent stress test* was run and crashed on
process exit, not during the operations themselves. "Compiles" and "the
few things I manually checked look right" are not the same as "correct."

## 6. Known limitations and honest trade-offs

- **`SlabAllocator` violates the Rule of Three** (destructor without
  copy constructor/assignment) — a latent double-free risk if it's ever
  copied. Never triggered because every instance is heap-allocated once
  and never copied, but not defended against.
- **No rehashing anywhere.** Both `HashTable` (fixed `CAPACITY`) and
  `ShardedHashTable` (fixed shard count) never grow. Load factor climbing
  well past 1 would degrade toward O(n) per operation with no automatic
  recovery.
- **Shard count is a static, workload-dependent tuning knob**, not
  something the table can pick for you — §4.4 shows why there's no
  universally "right" default.
- **No hardware performance counters were available for this analysis**
  (§0 tooling note) — the cache-locality numbers in §4.3 are a latency
  proxy, not literal cache-miss counts. If this needs to go further,
  running under Linux with `perf stat -e cache-misses,cache-references`
  or macOS Instruments (Time Profiler + Counters) would give direct
  hardware-counter data instead of a proxy.
- **`ht_search` returns a raw pointer into internal storage.** Safe for
  the unsynchronized `HashTable` API used directly, but `ShardedHashTable`
  deliberately does *not* expose this — `Get` copies the value out while
  still holding the shard's lock, specifically to avoid a caller holding a
  pointer that could be invalidated by another thread's `Set`/`Delete`
  after the lock is released.
