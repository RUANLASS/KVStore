# KVStore

A key-value store built from scratch in C++17: a custom hash table with a slab allocator, a thread-safe sharded wrapper, and a primary-replica server pair speaking a small TCP protocol.

## Features

- Custom hash table with separate chaining, hashed with xxhash
- Pooled slab allocator (`SlabAllocator<T>`) for `HT_Item`/`LinkedList` allocations, avoiding per-insert `new`/`delete`
- Thread-safe sharded hash table (`ShardedHashTable`): 16 independent shards, each guarded by its own `std::shared_mutex`, so operations on different shards never block each other
- Primary-replica replication over TCP (`kvserver` / `kvreplica`) with a newline-delimited text protocol
- Interactive CLI (`kvstore`) over the plain (non-sharded) `HashTable`

## Architecture

```
ShardedHashTable   <- concurrent wrapper, 16 independent shards
  └── HashTable     <- separate chaining, own allocators per table
        ├── SlabAllocator<HT_Item>
        └── SlabAllocator<LinkedList>

kvserver (primary)              kvreplica
  - ShardedHashTable               - ShardedHashTable
  - accepts client SET/GET/DEL     - connects to primary, sends SYNC
  - fans out REPLWRITE to          - applies REPLWRITE lines from primary
    connected replicas             - serves read-only clients (GET only)
```

The wire protocol (see [kv_protocol.hpp](include/kv_protocol.hpp)) is deliberately simple and newline-delimited

```
Client -> server:  SET <key> <value>\n | GET <key>\n | DEL <key>\n | SYNC\n
Server -> client:  OK\n | VALUE <value>\n | NIL\n | READONLY\n
Server -> replica: REPLWRITE SET <key> <value>\n | REPLWRITE DEL <key>\n
```

## Requirements

- C++17 compiler (g++/clang++)
- xxhash (e.g. `brew install xxhash` on macOS)
- POSIX sockets/threads (`kvserver`/`kvreplica`/sharded benchmarks link `-pthread`)

## Build

```
make
```

Other targets:

- `make kvserver` / `make kvreplica` — build the primary/replica binaries
- `make bench` — build and run all benchmark binaries
- `make clean` — remove build artifacts

## Usage

### CLI

```
./kvstore
```

Commands: `SET <key> <value>`, `GET <key>`, `PRINT`, `DELETE <key>`, `EXIT`

### Server / replica

```
./kvserver <port>
./kvreplica <primary_host> <primary_port> <read_port>
```

`kvserver` accepts client writes and reads, and fans writes out to any connected replicas. `kvreplica` connects to a primary, syncs the current dataset, applies live `REPLWRITE`s, and serves read-only clients on its own port (writes get `READONLY`).

## Testing

```
python3 test_kvstore.py
python3 test_replication.py
```

## Benchmarks

```
make bench
```

Runs `benchmarks` (slab allocator), `benchmarks_plain` (plain `new`/`delete`/`malloc`, for comparison), `benchmarks_sharded` (multi-threaded `ShardedHashTable`), and `benchmarks_profiling` in sequence.

## Project layout

```
include/             Public headers (hashtable.hpp, allocator.hpp, kv_protocol.hpp)
src/                 Implementation files (hash table, protocol, server, replica, benchmarks)
main.cpp             KVStore CLI entry point
test_kvstore.py      Correctness test for the CLI/hash table
test_replication.py  Correctness test for kvserver/kvreplica
```

## Known limitations

- No rehashing: `HashTable` and `ShardedHashTable` are fixed-size (default capacity 50000, 16 shards)
- No persistence: everything is in-memory
- No automatic failover or reconnection between primary and replicas — a replica that loses its connection just serves stale data
- `read_line` reads one byte at a time per connection (simplicity over throughput)


Github link: github.com/RUANLASS/KVStore
