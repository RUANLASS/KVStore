#ifndef HASHTABLE_HPP
#define HASHTABLE_HPP

#include <string>
#include "allocator.hpp"
#include <optional>
#include <functional>

typedef struct HashTable HashTable;

#define CAPACITY 50000

// ----------------------------------------SHARDED HASH TABLE----------------------------------------
unsigned long HashFunction(const std::string& str);

// Concurrent wrapper around HashTable: N independent shards, each guarded by
// its own std::shared_mutex. A key routes to shard HashFunction(key) %
// shard_count, so operations on different shards never block each other.
class ShardedHashTable {
private:
    struct Shard;   // Defined in the .cpp.
    std::vector<std::unique_ptr<Shard>> shards;
    int shard_count;
    unsigned long get_shard_index(const std::string& key) const;
public:
    explicit ShardedHashTable(int num_shards = 16);
    // Needed explicitly: Shard is an incomplete type here, so the compiler
    // can't generate the destructor implicitly in the header.
    ~ShardedHashTable();
    ShardedHashTable(const ShardedHashTable&) = delete;
    ShardedHashTable& operator=(const ShardedHashTable&) = delete;

    std::optional<std::string> Get(const std::string& key) const;
    void Set(const std::string& key, const std::string& value);
    bool Delete(const std::string& key);

    // Walks every shard and invokes fn(key, value) for each entry, used to
    // build the initial snapshot sent to a newly joined replica. Each shard
    // is locked and drained in turn rather than the whole table at once, so
    // this is not a single atomic point-in-time view: a write landing in a
    // shard already visited won't appear here. Matches the eventual-
    // consistency model used throughout the replication layer.
    void ForEachSnapshot(const std::function<void(const std::string&, const std::string&)>& fn) const;
};

// ----------------------------------------HASH TABLE ITEM----------------------------------------

typedef struct HT_Item
{
    std::string key;
    std::string value;
    HT_Item(std::string k, std::string v);
} HT_Item;

// ----------------------------------------LINKED LIST FOR BUCKETS----------------------------------------

typedef struct LinkedList {
    HT_Item* item;
    struct LinkedList* next;
} LinkedList;

LinkedList* allocate_list(HashTable* table);
LinkedList* linkedlist_insert(HashTable* table, LinkedList* list, HT_Item* item);
LinkedList* linkedlist_delete(HashTable* table, LinkedList* list, const std::string& key);
void free_linkedlist(HashTable* table, LinkedList* list);

// ----------------------------------------HASH TABLE----------------------------------------

typedef struct HashTable{
    LinkedList** buckets;  // buckets[i] is the head of that bucket's chain (NULL if empty).
    int size;
    int count;
    SlabAllocator<HT_Item>* item_allocator;
    SlabAllocator<LinkedList>* list_allocator;
} HashTable;

LinkedList** create_buckets(HashTable* table);
void free_buckets(HashTable* table);

HT_Item* create_item(HashTable* table, const std::string& key, const std::string& value);
HashTable* create_table(int size);
void free_item(HashTable* table, HT_Item* item);
void free_table(HashTable* table);
void print_table(HashTable* table);

void ht_insert(HashTable* table, const std::string& key, const std::string& value);
std::string* ht_search(HashTable* table, const std::string& key);
bool ht_delete(HashTable* table, const std::string& key);
void print_search(HashTable* table, const std::string& key);

#endif // HASHTABLE_HPP
