#ifndef HASHTABLE_HPP
#define HASHTABLE_HPP

#include <string>
#include "allocator.hpp"
#include <optional>

typedef struct HashTable HashTable;

#define CAPACITY 50000

// ----------------------------------------SHARDED HASH TABLE ITEM----------------------------------------
unsigned long HashFunction(const std::string& str);

class ShardedHashTable {
private:
    struct Shard;   // defined in the .cpp
    std::vector<std::unique_ptr<Shard>> shards;
    int shard_count;
    unsigned long get_shard_index(const std::string& key) const;
public:
    explicit ShardedHashTable(int num_shards = 16);
    ~ShardedHashTable();   // needed now: Shard is incomplete here, so the compiler
                            // can't generate the destructor implicitly in the header
    ShardedHashTable(const ShardedHashTable&) = delete;
    ShardedHashTable& operator=(const ShardedHashTable&) = delete;

    std::optional<std::string> Get(const std::string& key) const;
    void Set(const std::string& key, const std::string& value);
    bool Delete(const std::string& key);
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
    // HT_Item** items;
    LinkedList** buckets;
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
