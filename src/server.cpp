// kvserver: the primary side of the primary-replica architecture.
//
//   kvserver (primary)
//     - TCP listener, one thread per connection
//     - ShardedHashTable for storage
//     - replica registry: connected replica socket fds
//     - on client write: apply locally, then fan out REPLWRITE to replicas
//
// Reuses ShardedHashTable as-is; the only addition to the storage layer is
// ForEachSnapshot (see hashtable.cpp), used for the initial replica sync.

#include "../include/hashtable.hpp"
#include "../include/kv_protocol.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Registry of connected replica sockets. Mutex-guarded since both the accept
// thread (on SYNC) and every client-handling thread (on write fan-out) touch it.
struct ReplicaRegistry {
    std::mutex mtx;
    std::vector<int> fds;
};

// Reads one '\n'-terminated line from a socket, byte at a time (simplicity
// over throughput for this phase). Returns false on EOF/error, meaning the
// peer disconnected.
static bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        out += c;
    }
}

static void send_all(int fd, const std::string& data) {
    send(fd, data.data(), data.size(), 0);
}

// Sends one REPLWRITE line to every registered replica. If a write to a
// replica socket fails (broken pipe, replica died), that replica is dropped
// from the registry and the primary keeps serving clients; replication must
// never block or crash normal request handling.
static void fanout_to_replicas(ReplicaRegistry& registry, const std::string& line) {
    std::lock_guard<std::mutex> lock(registry.mtx);
    for (auto it = registry.fds.begin(); it != registry.fds.end();) {
        ssize_t n = send(*it, line.data(), line.size(), MSG_NOSIGNAL);
        if (n < 0) {
            close(*it);
            it = registry.fds.erase(it);
        } else {
            ++it;
        }
    }
}

// Handles the SYNC handshake for one newly connected replica.
//
// A replica needs a consistent starting point and must not miss or
// double-apply any write that lands during the handoff. This holds one
// process-wide write-serialization mutex only while (a) taking the snapshot
// and (b) registering the new replica socket; normal request handling stays
// fully sharded and concurrent otherwise.
static void handle_sync(int fd, ShardedHashTable& store, ReplicaRegistry& registry,
                         std::mutex& write_mtx) {
    std::lock_guard<std::mutex> write_lock(write_mtx);

    // Stream the current dataset as REPLWRITE SET lines before this socket is
    // registered for live writes, so the replica sees everything that existed
    // before it joined first, then everything after, with no gap because
    // write_mtx is held across both steps.
    store.ForEachSnapshot([&](const std::string& key, const std::string& value) {
        send_all(fd, serialize_replwrite_set(key, value));
    });

    {
        std::lock_guard<std::mutex> lock(registry.mtx);
        registry.fds.push_back(fd);
    }
}

// One thread per connection.
static void handle_connection(int fd, ShardedHashTable* store, ReplicaRegistry* registry,
                               std::mutex* write_mtx) {
    std::string line;
    bool is_replica = false;

    while (read_line(fd, line)) {
        Command cmd = parse_command(line);

        switch (cmd.type) {
            case CommandType::SET: {
                // Every client write acquires write_mtx before calling Set;
                // cheap and uncontended in the common case, only actually
                // serializes against a concurrent SYNC.
                std::lock_guard<std::mutex> lock(*write_mtx);
                store->Set(cmd.key, cmd.value);
                fanout_to_replicas(*registry, serialize_replwrite_set(cmd.key, cmd.value));
                send_all(fd, serialize_ok());
                break;
            }
            case CommandType::GET: {
                auto v = store->Get(cmd.key);
                send_all(fd, v ? serialize_value(*v) : serialize_nil());
                break;
            }
            case CommandType::DEL: {
                std::lock_guard<std::mutex> lock(*write_mtx);
                bool existed = store->Delete(cmd.key);
                if (existed) {
                    fanout_to_replicas(*registry, serialize_replwrite_del(cmd.key));
                }
                send_all(fd, serialize_ok());
                break;
            }
            case CommandType::SYNC: {
                handle_sync(fd, *store, *registry, *write_mtx);
                is_replica = true;
                break;
            }
            default: {
                // A malformed or unrecognized line still needs some response
                // so a client waiting on a reply doesn't hang forever.
                send_all(fd, serialize_nil());
                break;
            }
        }

        // After SYNC, this connection is a replica: it won't send further
        // commands, it just needs to be watched for disconnect (read_line
        // returning false, at the top of this loop) so it can be dropped
        // from the registry below. Do not break here: the fd was just
        // registered for fan-out by handle_sync, and closing it now would
        // end the connection immediately after the handshake instead of
        // waiting for an actual disconnect.
    }

    if (is_replica) {
        std::lock_guard<std::mutex> lock(registry->mtx);
        auto& fds = registry->fds;
        fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
    }
    close(fd);
}

int main(int argc, char** argv) {
    int port = 6380;
    if (argc > 1) port = std::atoi(argv[1]);

    ShardedHashTable store(16);
    ReplicaRegistry registry;
    std::mutex write_mtx;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 64) < 0) {
        perror("listen");
        return 1;
    }

    printf("kvserver (primary) listening on port %d\n", port);

    while (true) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        std::thread(handle_connection, client_fd, &store, &registry, &write_mtx).detach();
    }

    return 0;
}
