#include "../include/kv_protocol.hpp"
#include <sstream>

// Splits a line into whitespace-separated tokens. The value portion of
// SET/REPLWRITE SET is everything after the second token rejoined with
// single spaces: values may themselves contain spaces, keys may not.
static std::string join_rest(std::istringstream& iss) {
    std::string rest, word;
    bool first = true;
    while (iss >> word) {
        if (!first) rest += ' ';
        rest += word;
        first = false;
    }
    return rest;
}

Command parse_command(const std::string& line) {
    std::istringstream iss(line);
    std::string verb;
    iss >> verb;

    Command cmd;

    if (verb == "SET") {
        cmd.type = CommandType::SET;
        iss >> cmd.key;
        cmd.value = join_rest(iss);
    } else if (verb == "GET") {
        cmd.type = CommandType::GET;
        iss >> cmd.key;
    } else if (verb == "DEL") {
        cmd.type = CommandType::DEL;
        iss >> cmd.key;
    } else if (verb == "SYNC") {
        cmd.type = CommandType::SYNC;
    } else if (verb == "REPLWRITE") {
        std::string sub;
        iss >> sub;
        if (sub == "SET") {
            cmd.type = CommandType::REPLWRITE_SET;
            iss >> cmd.key;
            cmd.value = join_rest(iss);
        } else if (sub == "DEL") {
            cmd.type = CommandType::REPLWRITE_DEL;
            iss >> cmd.key;
        }
    }

    return cmd;
}

std::string serialize_ok() {
    return "OK\n";
}

std::string serialize_value(const std::string& value) {
    return "VALUE " + value + "\n";
}

std::string serialize_nil() {
    return "NIL\n";
}

std::string serialize_readonly() {
    return "READONLY\n";
}

std::string serialize_replwrite_set(const std::string& key, const std::string& value) {
    return "REPLWRITE SET " + key + " " + value + "\n";
}

std::string serialize_replwrite_del(const std::string& key) {
    return "REPLWRITE DEL " + key + "\n";
}
