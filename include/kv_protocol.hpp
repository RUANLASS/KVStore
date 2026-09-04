#ifndef KV_PROTOCOL_HPP
#define KV_PROTOCOL_HPP

#include <string>

// Wire protocol for the networked KVStore: a deliberately simple,
// newline-delimited ASCII protocol (not RESP-compatible).
//
//   Client -> server:  SET <key> <value>\n | GET <key>\n | DEL <key>\n | SYNC\n
//   Server -> client:  OK\n | VALUE <value>\n | NIL\n | READONLY\n
//   Server -> replica: REPLWRITE SET <key> <value>\n | REPLWRITE DEL <key>\n

enum class CommandType {
    SET,
    GET,
    DEL,
    SYNC,
    REPLWRITE_SET,
    REPLWRITE_DEL,
    UNKNOWN
};

struct Command {
    CommandType type = CommandType::UNKNOWN;
    std::string key;
    std::string value;
};

// Parses one line (no trailing '\n') of the protocol above into a Command.
// Unrecognized input becomes CommandType::UNKNOWN so callers can respond
// with an error instead of crashing on malformed input from the network.
Command parse_command(const std::string& line);

// Server -> client response serializers.
std::string serialize_ok();
std::string serialize_value(const std::string& value);
std::string serialize_nil();
std::string serialize_readonly();

// Server -> replica fan-out serializers (REPLWRITE lines, see above).
std::string serialize_replwrite_set(const std::string& key, const std::string& value);
std::string serialize_replwrite_del(const std::string& key);

#endif // KV_PROTOCOL_HPP
