CXX = g++
XXHASH_PREFIX = $(shell brew --prefix xxhash)
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude -I$(XXHASH_PREFIX)/include
TARGET = kvstore
BENCH_TARGET = benchmarks
BENCH_PLAIN_TARGET = benchmarks_plain
BENCH_SHARDED_TARGET = benchmarks_sharded
BENCH_PROFILING_TARGET = benchmarks_profiling

all: $(TARGET)

$(TARGET): main.cpp src/hashtable.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) main.cpp src/hashtable.cpp

$(BENCH_TARGET): src/benchmarks.cpp src/hashtable.cpp
	$(CXX) $(CXXFLAGS) -O2 -o $(BENCH_TARGET) src/benchmarks.cpp src/hashtable.cpp

# Same benchmark source, same hashtable logic, but HT_Item/LinkedList allocation
# goes through plain new/delete/malloc instead of the SlabAllocator -- for a
# true apples-to-apples custom-allocator-vs-regular-allocator comparison.
$(BENCH_PLAIN_TARGET): src/benchmarks.cpp src/hashtable.cpp
	$(CXX) $(CXXFLAGS) -O2 -DBENCH_PLAIN_ALLOCATOR -o $(BENCH_PLAIN_TARGET) src/benchmarks.cpp src/hashtable.cpp

# Multi-threaded benchmarks for ShardedHashTable -- needs -pthread.
$(BENCH_SHARDED_TARGET): src/benchmarks_sharded.cpp src/hashtable.cpp
	$(CXX) $(CXXFLAGS) -O2 -pthread -o $(BENCH_SHARDED_TARGET) src/benchmarks_sharded.cpp src/hashtable.cpp

$(BENCH_PROFILING_TARGET): src/benchmarks_profiling.cpp src/hashtable.cpp
	$(CXX) $(CXXFLAGS) -O2 -pthread -o $(BENCH_PROFILING_TARGET) src/benchmarks_profiling.cpp src/hashtable.cpp

bench: $(BENCH_TARGET) $(BENCH_PLAIN_TARGET) $(BENCH_SHARDED_TARGET) $(BENCH_PROFILING_TARGET)
	./$(BENCH_TARGET)
	@echo
	./$(BENCH_PLAIN_TARGET)
	@echo
	./$(BENCH_SHARDED_TARGET)
	@echo
	./$(BENCH_PROFILING_TARGET)

clean:
	rm -f $(TARGET) $(BENCH_TARGET) $(BENCH_PLAIN_TARGET) $(BENCH_SHARDED_TARGET) $(BENCH_PROFILING_TARGET)

.PHONY: all clean
