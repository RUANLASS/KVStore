#include "../include/hashtable.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Global allocation counter. Overriding operator new/delete lets us measure
// real heap traffic (as opposed to slab-pool reuse) around any block of code.
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

static void report_latency(const char* label, const std::vector<double>& v) {
    printf("  %-4s p50=%.0fns  p90=%.0fns  p99=%.0fns  p99.9=%.0fns  max=%.0fns\n",
           label, percentile(v, 50), percentile(v, 90), percentile(v, 99),
           percentile(v, 99.9), v.back());
}

// ---------------------------------------------------------------------------
// Benchmark 1: allocation counter under a mixed GET/SET workload
// ---------------------------------------------------------------------------

void bench_allocation_counter_kvstore() {
    const int N = 5000;
    HashTable* table = create_table(CAPACITY);
    auto keys = make_keys(N);

    // Warm-up: populate the table and let the slab pool grow to size. Not counted.
    for (int i = 0; i < N; i++) ht_insert(table, keys[i], "val" + std::to_string(i));

    // Delete half back so the free-lists have slots ready, mimicking real churn
    // instead of a monotonically growing table.
    for (int i = 0; i < N / 2; i++) ht_delete(table, keys[i]);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_pick(0, N - 1);

    const int OPS = 20000;
    g_alloc_count.store(0);
    g_counting.store(true);
    for (int i = 0; i < OPS; i++) {
        int k = key_pick(rng);
        if (i % 2 == 0) {
            ht_insert(table, keys[k], "val" + std::to_string(i));
        } else {
            ht_search(table, keys[k]);
        }
    }
    g_counting.store(false);
    long allocs = g_alloc_count.load();

    printf("  KVStore%-16s: %d ops -> %ld heap allocations (%.4f allocations/op)\n",
#ifdef BENCH_PLAIN_ALLOCATOR
           " (plain new/delete)",
#else
           " (slab allocator)",
#endif
           OPS, allocs, static_cast<double>(allocs) / OPS);

    free_table(table);
}

void bench_allocation_counter_stl() {
    const int N = 5000;
    std::unordered_map<std::string, std::string> table;
    auto keys = make_keys(N);

    for (int i = 0; i < N; i++) table[keys[i]] = "val" + std::to_string(i);
    for (int i = 0; i < N / 2; i++) table.erase(keys[i]);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_pick(0, N - 1);

    const int OPS = 20000;
    g_alloc_count.store(0);
    g_counting.store(true);
    for (int i = 0; i < OPS; i++) {
        int k = key_pick(rng);
        if (i % 2 == 0) {
            table[keys[k]] = "val" + std::to_string(i);
        } else {
            auto it = table.find(keys[k]);
            (void)it;
        }
    }
    g_counting.store(false);
    long allocs = g_alloc_count.load();

    printf("  std::unordered_map : %d ops -> %ld heap allocations (%.4f allocations/op)\n",
           OPS, allocs, static_cast<double>(allocs) / OPS);
}

void bench_allocation_counter() {
    printf("=== Allocation counter (mixed GET/SET) ===\n");
    bench_allocation_counter_kvstore();
    bench_allocation_counter_stl();
    printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 2: throughput, read-heavy and mixed workloads
// ---------------------------------------------------------------------------

void bench_throughput_kvstore() {
    const int N = 20000;
    HashTable* table = create_table(CAPACITY);
    auto keys = make_keys(N);
    for (int i = 0; i < N; i++) ht_insert(table, keys[i], "val" + std::to_string(i));

    std::mt19937 rng(1);
    std::uniform_int_distribution<int> key_pick(0, N - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);

    auto run_workload = [&](const char* label, double write_ratio, int ops) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ops; i++) {
            int k = key_pick(rng);
            if (coin(rng) < write_ratio) {
                ht_insert(table, keys[k], "val" + std::to_string(i));
            } else {
                ht_search(table, keys[k]);
            }
        }
        auto end = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(end - start).count();
        printf("  KVStore %-20s: %d ops in %.4f s -> %.0f ops/sec\n", label, ops, secs, ops / secs);
    };

    run_workload("Read-heavy (5% w)", 0.05, 200000);
    run_workload("Mixed (50% w)", 0.50, 200000);

    free_table(table);
}

void bench_throughput_stl() {
    const int N = 20000;
    std::unordered_map<std::string, std::string> table;
    auto keys = make_keys(N);
    for (int i = 0; i < N; i++) table[keys[i]] = "val" + std::to_string(i);

    std::mt19937 rng(1);
    std::uniform_int_distribution<int> key_pick(0, N - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);

    auto run_workload = [&](const char* label, double write_ratio, int ops) {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ops; i++) {
            int k = key_pick(rng);
            if (coin(rng) < write_ratio) {
                table[keys[k]] = "val" + std::to_string(i);
            } else {
                auto it = table.find(keys[k]);
                (void)it;
            }
        }
        auto end = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(end - start).count();
        printf("  unordered_map %-20s: %d ops in %.4f s -> %.0f ops/sec\n", label, ops, secs, ops / secs);
    };

    run_workload("Read-heavy (5% w)", 0.05, 200000);
    run_workload("Mixed (50% w)", 0.50, 200000);
}

void bench_throughput() {
    printf("=== Throughput ===\n");
    bench_throughput_kvstore();
    bench_throughput_stl();
    printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 3: latency percentiles, GET and SET measured separately
// ---------------------------------------------------------------------------

void bench_latency_percentiles_kvstore() {
    const int N = 20000;
    HashTable* table = create_table(CAPACITY);
    auto keys = make_keys(N);
    for (int i = 0; i < N; i++) ht_insert(table, keys[i], "val" + std::to_string(i));

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> key_pick(0, N - 1);

    const int SAMPLES = 50000;
    std::vector<double> get_ns, set_ns;
    get_ns.reserve(SAMPLES);
    set_ns.reserve(SAMPLES);

    for (int i = 0; i < SAMPLES; i++) {
        int k = key_pick(rng);
        auto t0 = std::chrono::steady_clock::now();
        ht_search(table, keys[k]);
        auto t1 = std::chrono::steady_clock::now();
        get_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    for (int i = 0; i < SAMPLES; i++) {
        int k = key_pick(rng);
        std::string val = "val" + std::to_string(i);
        auto t0 = std::chrono::steady_clock::now();
        ht_insert(table, keys[k], val);
        auto t1 = std::chrono::steady_clock::now();
        set_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    std::sort(get_ns.begin(), get_ns.end());
    std::sort(set_ns.begin(), set_ns.end());

    printf("  KVStore:\n");
    report_latency("GET", get_ns);
    report_latency("SET", set_ns);

    free_table(table);
}

void bench_latency_percentiles_stl() {
    const int N = 20000;
    std::unordered_map<std::string, std::string> table;
    auto keys = make_keys(N);
    for (int i = 0; i < N; i++) table[keys[i]] = "val" + std::to_string(i);

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> key_pick(0, N - 1);

    const int SAMPLES = 50000;
    std::vector<double> get_ns, set_ns;
    get_ns.reserve(SAMPLES);
    set_ns.reserve(SAMPLES);

    for (int i = 0; i < SAMPLES; i++) {
        int k = key_pick(rng);
        auto t0 = std::chrono::steady_clock::now();
        auto it = table.find(keys[k]);
        (void)it;
        auto t1 = std::chrono::steady_clock::now();
        get_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    for (int i = 0; i < SAMPLES; i++) {
        int k = key_pick(rng);
        std::string val = "val" + std::to_string(i);
        auto t0 = std::chrono::steady_clock::now();
        table[keys[k]] = val;
        auto t1 = std::chrono::steady_clock::now();
        set_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    std::sort(get_ns.begin(), get_ns.end());
    std::sort(set_ns.begin(), set_ns.end());

    printf("  std::unordered_map:\n");
    report_latency("GET", get_ns);
    report_latency("SET", set_ns);
}

void bench_latency_percentiles() {
    printf("=== Latency percentiles ===\n");
    bench_latency_percentiles_kvstore();
    bench_latency_percentiles_stl();
    printf("\n");
}

int main() {
#ifdef BENCH_PLAIN_ALLOCATOR
    printf("KVStore allocator backend: plain new/delete/malloc (BENCH_PLAIN_ALLOCATOR)\n\n");
#else
    printf("KVStore allocator backend: SlabAllocator (custom)\n\n");
#endif

    bench_allocation_counter();
    bench_throughput();
    bench_latency_percentiles();
    return 0;
}
