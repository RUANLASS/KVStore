#include "../include/hashtable.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Profiling-focused benchmarks: allocation count on the hot path, a
// cache-locality proxy (sequential vs. scattered access latency), lock
// contention / atomic-operation cost, and latency distributions under
// varying concurrency. See WRITEUP.md for the full analysis of these
// numbers -- this file only produces them.
//
// Tooling note: this machine has neither `perf` nor Instruments/`valgrind`
// available (sandboxed macOS, no hardware performance counters exposed).
// Where a "real" cache-miss counter would normally be used, this file
// instead measures latency under access patterns specifically chosen to
// make cache effects visible (small working set vs. one that overflows
// L2/L3) -- a well-established technique for reasoning about cache
// behavior without hardware counters, clearly labeled as a proxy below.
// ---------------------------------------------------------------------------

static std::atomic<long> g_alloc_count{0};
static std::atomic<bool> g_counting{false};

void* operator new(std::size_t sz) {
    if (g_counting.load(std::memory_order_relaxed)) g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t sz) {
    if (g_counting.load(std::memory_order_relaxed)) g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static std::vector<std::string> make_keys(int n) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; i++) keys.push_back("key" + std::to_string(i));
    return keys;
}

static double percentile(const std::vector<double>& sorted_ns, double pct) {
    if (sorted_ns.empty()) return 0.0;
    size_t idx = static_cast<size_t>(pct / 100.0 * (sorted_ns.size() - 1));
    return sorted_ns[idx];
}

static void report_latency(const char* label, std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    printf("  %-4s p50=%.0fns  p90=%.0fns  p99=%.0fns  p99.9=%.0fns  max=%.0fns\n",
           label, percentile(v, 50), percentile(v, 90), percentile(v, 99),
           percentile(v, 99.9), v.back());
}

// ---------------------------------------------------------------------------
// 1. Allocation count on the hot path, broken down by operation type
// ---------------------------------------------------------------------------

void bench_hotpath_allocations() {
    printf("=== Allocation count on hot path (per operation type, steady state) ===\n");

    const int N = 5000;
    HashTable* table = create_table(CAPACITY);
    auto keys = make_keys(N);

    // Warm up: populate, then delete half back so free-lists are populated.
    // Not counted -- this is pool growth, a one-time cost, not steady state.
    for (int i = 0; i < N; i++) ht_insert(table, keys[i], "val" + std::to_string(i));
    for (int i = 0; i < N / 2; i++) ht_delete(table, keys[i]);

    const int OPS = 10000;

    auto measure = [&](const char* label, auto&& op) {
        g_alloc_count.store(0);
        g_counting.store(true);
        for (int i = 0; i < OPS; i++) op(i);
        g_counting.store(false);
        printf("  %-22s: %d ops -> %ld allocations (%.5f/op)\n",
               label, OPS, g_alloc_count.load(), static_cast<double>(g_alloc_count.load()) / OPS);
    };

    // GET on existing keys -- pure read, should be exactly zero.
    measure("GET (hit)", [&](int i) { ht_search(table, keys[i % (N / 2) + N / 2]); });

    // GET on missing keys -- also pure read, zero.
    measure("GET (miss)", [&](int i) { ht_search(table, "missing" + std::to_string(i)); });

    // SET updating an existing key -- in-place value assignment, zero new nodes.
    measure("SET (update existing)", [&](int i) {
        ht_insert(table, keys[N / 2], "v" + std::to_string(i));
    });

    // SET + DELETE cycling the same key -- pool should fully recycle, zero net allocations.
    measure("SET+DELETE (recycled)", [&](int i) {
        std::string k = "cycle" + std::to_string(i % 64); // bounded key space -> bounded pool growth
        ht_insert(table, k, "v");
        ht_delete(table, k);
    });

    free_table(table);
    printf("\n");
}

// ---------------------------------------------------------------------------
// 2. Cache-locality proxy: latency under a working set that fits in cache
// vs. one that doesn't, and sequential vs. scattered access order.
// ---------------------------------------------------------------------------

void bench_cache_locality_proxy() {
    printf("=== Cache-locality proxy (latency, not a hardware counter -- see note above) ===\n");

    auto run = [&](const char* label, int n, bool scattered) {
        HashTable* table = create_table(CAPACITY);
        auto keys = make_keys(n);
        for (int i = 0; i < n; i++) ht_insert(table, keys[i], "v" + std::to_string(i));

        std::vector<int> order(n);
        for (int i = 0; i < n; i++) order[i] = i;
        if (scattered) {
            std::mt19937 rng(42);
            std::shuffle(order.begin(), order.end(), rng);
        }

        const int PASSES = 20;
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(n) * PASSES);
        for (int p = 0; p < PASSES; p++) {
            for (int idx : order) {
                auto t0 = std::chrono::steady_clock::now();
                ht_search(table, keys[idx]);
                auto t1 = std::chrono::steady_clock::now();
                samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
        }

        printf("  %-38s (n=%-6d): ", label, n);
        report_latency("GET", samples);
        free_table(table);
    };

    // Small working set: keys + buckets touched comfortably fit in L2.
    run("Small working set, sequential", 200, false);
    run("Small working set, scattered", 200, true);
    // Large working set: far exceeds typical L2/L3, forces more DRAM traffic.
    run("Large working set, sequential", 200000, false);
    run("Large working set, scattered", 200000, true);
    printf("\n");
}

// ---------------------------------------------------------------------------
// 3. Atomic operation cost: uncontended vs. contended fetch_add
// ---------------------------------------------------------------------------

void bench_atomic_cost() {
    printf("=== Atomic fetch_add cost, uncontended vs. contended ===\n");

    const long ITERS_PER_THREAD = 2'000'000;

    // Uncontended: single thread, no cache-line bouncing.
    {
        std::atomic<long> counter{0};
        auto start = std::chrono::steady_clock::now();
        for (long i = 0; i < ITERS_PER_THREAD; i++) counter.fetch_add(1, std::memory_order_relaxed);
        auto end = std::chrono::steady_clock::now();
        double ns_per_op = std::chrono::duration<double, std::nano>(end - start).count() / ITERS_PER_THREAD;
        printf("  1 thread,  private counter : %.2f ns/op\n", ns_per_op);
    }

    // Contended: N threads hammering the SAME atomic (cache-line ping-pong).
    for (int threads : {2, 4, 8}) {
        std::atomic<long> counter{0};
        std::vector<std::thread> workers;
        auto start = std::chrono::steady_clock::now();
        for (int t = 0; t < threads; t++) {
            workers.emplace_back([&]() {
                for (long i = 0; i < ITERS_PER_THREAD; i++) counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& w : workers) w.join();
        auto end = std::chrono::steady_clock::now();
        long total_ops = static_cast<long>(threads) * ITERS_PER_THREAD;
        double ns_per_op = std::chrono::duration<double, std::nano>(end - start).count() / total_ops;
        printf("  %d threads, shared counter  : %.2f ns/op (aggregate, incl. cache-line contention)\n",
               threads, ns_per_op);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// 4. Lock contention, isolated: latency percentiles vs. thread count at a
// FIXED, deliberately low shard count, so the tail growth is attributable
// to lock wait time rather than any change in the underlying work per op.
// ---------------------------------------------------------------------------

void bench_lock_contention_isolated() {
    printf("=== Lock contention isolated (4 shards, fixed; thread count varies) ===\n");

    const int N = 20000;
    auto keys = make_keys(N);
    const int SHARD_COUNT = 4; // deliberately low, to make contention pronounced
    const int SAMPLES_PER_THREAD = 4000;

    for (int threads : {1, 2, 4, 8}) {
        ShardedHashTable table(SHARD_COUNT);
        for (int i = 0; i < N; i++) table.Set(keys[i], "v" + std::to_string(i));

        std::vector<std::vector<double>> set_ns_per_thread(threads);
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++) {
            workers.emplace_back([&, t]() {
                std::mt19937 rng(t + 1);
                std::uniform_int_distribution<int> key_pick(0, N - 1);
                auto& set_ns = set_ns_per_thread[t];
                set_ns.reserve(SAMPLES_PER_THREAD);
                for (int i = 0; i < SAMPLES_PER_THREAD; i++) {
                    const std::string& key = keys[key_pick(rng)];
                    auto t0 = std::chrono::steady_clock::now();
                    table.Set(key, "v" + std::to_string(i));
                    auto t1 = std::chrono::steady_clock::now();
                    set_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
                }
            });
        }
        for (auto& w : workers) w.join();

        std::vector<double> set_ns;
        for (auto& v : set_ns_per_thread) set_ns.insert(set_ns.end(), v.begin(), v.end());

        printf("  threads=%d:\n", threads);
        printf("    ");
        report_latency("SET", set_ns);
    }
    printf("\n");
}

int main() {
    printf("Hardware concurrency: %u threads available\n\n", std::thread::hardware_concurrency());

    bench_hotpath_allocations();
    bench_cache_locality_proxy();
    bench_atomic_cost();
    bench_lock_contention_isolated();
    return 0;
}
