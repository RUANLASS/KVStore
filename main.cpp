
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include "include/hashtable.hpp"

int main() {
    HashTable* store = create_table(CAPACITY);
    std::string line;

    std::cout << "KVStore CLI. Commands: SET <key> <value>, GET <key>, PRINT, DELETE <key>, EXIT\n";

    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        if (command.empty()) {
            continue;
        } else if (command == "SET") {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);
            if (!value.empty() && value[0] == ' ') {
                value.erase(0, 1);
            }
            if (key.empty() || value.empty()) {
                std::cout << "Usage: SET <key> <value>\n";
                continue;
            }
            ht_insert(store, key, value);
            std::cout << "OK\n";
        } else if (command == "GET") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Usage: GET <key>\n";
                continue;
            }
<<<<<<< HEAD
            // auto it = store.find(key);
            // auto it = ht_search(store, key);
            // if (it) {
            print_table(store);
        } else if (command == "DELETE") {
            std::string key;
            if (key.empty()) {
                std::cout << "Usage: DELETE <key>\n";
                continue;
            }
            if (ht_delete(store, key)) {
                std::cout << "OK\n";
            } else {
                std::cout << "(nil)\n";
            }
        } else if (command == "EXIT" || command == "QUIT") {
            break;
        } else {
            std::cout << "Unknown command: " << command << "\n";
        }
    }

    return 0;
}
