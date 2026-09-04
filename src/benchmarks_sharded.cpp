#include "../include/hashtable.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

// Runs `thread_count` threads, each doing `ops_per_thread` mixed Get/Set ops
// against `table`, and returns aggregate ops/sec across all threads.
static double run_mixed_workload(ShardedHashTable& table, const std::vector<std::string>& keys,
                                  int thread_count, int ops_per_thread, double write_ratio) {
    std::vector<std::thread> workers;
    auto start = std::chrono::steady_clock::now();

    for (int t = 0; t < thread_count; t++) {
        workers.emplace_back([&, t]() {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> key_pick(0, static_cast<int>(keys.size()) - 1);
            std::uniform_real_distribution<double> coin(0.0, 1.0);
            for (int i = 0; i < ops_per_thread; i++) {
                const std::string& key = keys[key_pick(rng)];
                if (coin(rng) < write_ratio) {
                    table.Set(key, "v" + std::to_string(i));
                } else {
                    table.Get(key);
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    long total_ops = static_cast<long>(thread_count) * ops_per_thread;
    return total_ops / secs;
}

// ---------------------------------------------------------------------------
// Benchmark 1: throughput vs. shard count, fixed thread count
// (demonstrates why sharding matters: fewer shards -> more lock contention)
// ---------------------------------------------------------------------------

void bench_throughput_vs_shard_count() {
    printf("=== Throughput vs shard count (8 threads, mixed 50%% writes) ===\n");

    const int N = 20000;
    auto keys = make_keys(N);
    const int OPS_PER_THREAD = 25000;
    const int THREADS = 8;

    for (int shard_count : {1, 2, 4, 16, 64}) {
        ShardedHashTable table(shard_count);
        for (int i = 0; i < N; i++) table.Set(keys[i], "v" + std::to_string(i));

        double ops_per_sec = run_mixed_workload(table, keys, THREADS, OPS_PER_THREAD, 0.5);
        printf("  shards=%-3d: %.0f ops/sec\n", shard_count, ops_per_sec);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 2: throughput vs. thread count, fixed shard count
// (demonstrates that sharding actually enables parallel scaling)
// ---------------------------------------------------------------------------

void bench_throughput_vs_thread_count() {
    printf("=== Throughput vs thread count (16 shards, mixed 50%% writes) ===\n");

    const int N = 20000;
    auto keys = make_keys(N);
    const int TOTAL_OPS = 200000;
    const int SHARD_COUNT = 16;

    for (int threads : {1, 2, 4, 8}) {
        ShardedHashTable table(SHARD_COUNT);
        for (int i = 0; i < N; i++) table.Set(keys[i], "v" + std::to_string(i));

        int ops_per_thread = TOTAL_OPS / threads;
        double ops_per_sec = run_mixed_workload(table, keys, threads, ops_per_thread, 0.5);
        printf("  threads=%-2d: %.0f ops/sec\n", threads, ops_per_sec);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Benchmark 3: Get/Set latency percentiles under concurrent contention
// ---------------------------------------------------------------------------

void bench_latency_percentiles_concurrent() {
    printf("=== Latency percentiles under contention (8 threads, 16 shards) ===\n");

    const int N = 20000;
    auto keys = make_keys(N);
    const int SHARD_COUNT = 16;
    const int THREADS = 8;
    const int SAMPLES_PER_THREAD = 5000;

    ShardedHashTable table(SHARD_COUNT);
    for (int i = 0; i < N; i++) table.Set(keys[i], "v" + std::to_string(i));

    std::vector<std::vector<double>> get_ns_per_thread(THREADS);
    std::vector<std::vector<double>> set_ns_per_thread(THREADS);
    std::vector<std::thread> workers;

    for (int t = 0; t < THREADS; t++) {
        workers.emplace_back([&, t]() {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> key_pick(0, N - 1);
            auto& get_ns = get_ns_per_thread[t];
            auto& set_ns = set_ns_per_thread[t];
            get_ns.reserve(SAMPLES_PER_THREAD);
            set_ns.reserve(SAMPLES_PER_THREAD);

            for (int i = 0; i < SAMPLES_PER_THREAD; i++) {
                const std::string& key = keys[key_pick(rng)];

                auto t0 = std::chrono::steady_clock::now();
                table.Get(key);
                auto t1 = std::chrono::steady_clock::now();
                get_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());

                std::string val = "v" + std::to_string(i);
                auto t2 = std::chrono::steady_clock::now();
                table.Set(key, val);
                auto t3 = std::chrono::steady_clock::now();
                set_ns.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count());
            }
        });
    }
    for (auto& w : workers) w.join();

    std::vector<double> get_ns, set_ns;
    for (auto& v : get_ns_per_thread) get_ns.insert(get_ns.end(), v.begin(), v.end());
    for (auto& v : set_ns_per_thread) set_ns.insert(set_ns.end(), v.begin(), v.end());
    std::sort(get_ns.begin(), get_ns.end());
    std::sort(set_ns.begin(), set_ns.end());

    auto report = [&](const char* label, const std::vector<double>& v) {
        printf("  %-4s p50=%.0fns  p90=%.0fns  p99=%.0fns  p99.9=%.0fns  max=%.0fns\n",
               label, percentile(v, 50), percentile(v, 90), percentile(v, 99),
               percentile(v, 99.9), v.back());
    };
    report("GET", get_ns);
    report("SET", set_ns);
    printf("\n");
}

int main() {
    printf("Hardware concurrency: %u threads available\n\n", std::thread::hardware_concurrency());

    bench_throughput_vs_shard_count();
    bench_throughput_vs_thread_count();
    bench_latency_percentiles_concurrent();
    return 0;
}
