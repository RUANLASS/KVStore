// kvreplica: the replica side of the primary-replica architecture.
//
//   kvreplica
//     - connects out to the primary
//     - applies incoming REPLWRITE lines to its own ShardedHashTable
//     - runs its own TCP listener for read clients (GET only; SET/DEL -> READONLY)

#include "../include/hashtable.hpp"
#include "../include/kv_protocol.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

// Connects out to the primary, sends SYNC, then loops reading REPLWRITE
// lines and applying them to the local ShardedHashTable.
static void replication_loop(int primary_fd, ShardedHashTable* store) {
    send_all(primary_fd, "SYNC\n");

    std::string line;
    while (read_line(primary_fd, line)) {
        Command cmd = parse_command(line);
        if (cmd.type == CommandType::REPLWRITE_SET) {
            store->Set(cmd.key, cmd.value);
        } else if (cmd.type == CommandType::REPLWRITE_DEL) {
            store->Delete(cmd.key);
        }
        // Anything else on this connection is unexpected (the primary only
        // ever sends REPLWRITE lines here) and is ignored rather than treated
        // as fatal, since a malformed line shouldn't tear down replication.
    }

    // The primary closed the connection or the socket errored. This phase
    // has no reconnect/retry logic: automatic failover and reconnection are
    // out of scope, so the replica just stops applying updates and keeps
    // serving whatever it already has.
    fprintf(stderr, "kvreplica: lost connection to primary, serving stale data\n");
    close(primary_fd);
}

// Serves read clients on the replica's own listener; writes are rejected.
static void handle_client_connection(int fd, ShardedHashTable* store) {
    std::string line;
    while (read_line(fd, line)) {
        Command cmd = parse_command(line);
        switch (cmd.type) {
            case CommandType::GET: {
                auto v = store->Get(cmd.key);
                send_all(fd, v ? serialize_value(*v) : serialize_nil());
                break;
            }
            case CommandType::SET:
            case CommandType::DEL: {
                // A replica never accepts writes directly from clients; only
                // REPLWRITE from the primary mutates its store.
                send_all(fd, serialize_readonly());
                break;
            }
            default:
                send_all(fd, serialize_nil());
                break;
        }
    }
    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <primary_host> <primary_port> <read_port>\n", argv[0]);
        return 1;
    }
    const char* primary_host = argv[1];
    int primary_port = std::atoi(argv[2]);
    int read_port = std::atoi(argv[3]);

    ShardedHashTable store(16);

    // Connect out to the primary.
    int primary_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (primary_fd < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in primary_addr{};
    primary_addr.sin_family = AF_INET;
    primary_addr.sin_port = htons(primary_port);
    if (inet_pton(AF_INET, primary_host, &primary_addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid primary host: %s\n", primary_host);
        return 1;
    }
    if (connect(primary_fd, (sockaddr*)&primary_addr, sizeof(primary_addr)) < 0) {
        perror("connect");
        return 1;
    }

    std::thread(replication_loop, primary_fd, &store).detach();

    // Own listener for read clients.
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(read_port);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 64) < 0) {
        perror("listen");
        return 1;
    }

    printf("kvreplica listening on port %d, replicating from %s:%d\n", read_port, primary_host,
           primary_port);

    while (true) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        std::thread(handle_client_connection, client_fd, &store).detach();
    }

    return 0;
}
