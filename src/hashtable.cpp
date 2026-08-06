#define XXH_INLINE_ALL
#include "xxhash.h"
#include "../include/hashtable.hpp"
#include "linkedlist.cpp"
#include "../include/allocator.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <new>
#include <shared_mutex>
#include <vector>
#include <memory>
#include <optional>

#ifndef BENCH_PLAIN_ALLOCATOR
// static SlabAllocator<HT_Item> ht_item_allocator;
#endif
HT_Item::HT_Item(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}

unsigned long HashFunction(const std::string& str){
    return static_cast<unsigned long>(XXH3_64bits(str.data(), str.size()));
};


struct ShardedHashTable::Shard {
    HashTable* map = nullptr;
    mutable std::shared_mutex mutex;

    ~Shard() {
        if (map) free_table(map);
    }
};

ShardedHashTable::ShardedHashTable(int num_shards) : shard_count(num_shards) {
    shards.reserve(shard_count);
    for (int i = 0; i < shard_count; ++i) {
        auto shard = std::make_unique<Shard>();
        shard->map = create_table(CAPACITY / shard_count);
        shards.push_back(std::move(shard));
    }
}

ShardedHashTable::~ShardedHashTable() = default;

unsigned long ShardedHashTable::get_shard_index(const std::string& key) const {
    return HashFunction(key) % shard_count;
}

std::optional<std::string> ShardedHashTable::Get(const std::string& key) const {
    Shard& shard = *shards[get_shard_index(key)];
    std::shared_lock lock(shard.mutex);
    std::string* v = ht_search(shard.map, key);
    return v ? std::optional<std::string>(*v) : std::nullopt;
}

void ShardedHashTable::Set(const std::string& key, const std::string& value) {
    Shard& shard = *shards[get_shard_index(key)];
    std::unique_lock lock(shard.mutex);
    ht_insert(shard.map, key, value);
}

bool ShardedHashTable::Delete(const std::string& key) {
    Shard& shard = *shards[get_shard_index(key)];
    std::unique_lock lock(shard.mutex);
    return ht_delete(shard.map, key);
}

// ----------------------------------------HASH TABLE----------------------------------------

LinkedList** create_buckets(HashTable* table)
{
    // Create the overflow buckets; an array of LinkedLists.
    LinkedList** buckets = (LinkedList**) calloc(table->size, sizeof(LinkedList*));

    for (int i = 0; i < table->size; i++)
        buckets[i] = NULL;

    return buckets;
}

void free_buckets(HashTable* table)
{
    // Free all the overflow bucket lists.
    LinkedList** buckets = table->buckets;

    for (int i = 0; i < table->size; i++)
        free_linkedlist(table, buckets[i]);

    free(buckets);
}

HT_Item* create_item(HashTable* table, const std::string& key, const std::string& value){
#ifdef BENCH_PLAIN_ALLOCATOR
    HT_Item* item = new HT_Item(key,value);
    return item;
#else
    return table->item_allocator->allocate(key, value);    
#endif
}

HashTable* create_table(int size){
    HashTable* table = (HashTable*)malloc(sizeof(HashTable));
    table->size = size;
    table->count = 0;
    table->item_allocator = new SlabAllocator<HT_Item>();   
    table->list_allocator = new SlabAllocator<LinkedList>();
    table->buckets = create_buckets(table);
    return table;
}

// Need this function if you ever write anything like malloc/calloc. Since you alloced key, value, item, you need to free all when you delete.
void free_item(HashTable* table, HT_Item* item){
#ifdef BENCH_PLAIN_ALLOCATOR
    delete item;
#else
    table->item_allocator->deallocate(item);
#endif
}

void free_table(HashTable* table){
    // free_buckets already frees every item reachable from each bucket's chain.
    free_buckets(table);
    delete table->item_allocator;                            
    delete table->list_allocator;
    free(table);
}

void print_table(HashTable* table)
{
    printf("\nHash Table\n-------------------\n");

    for (int i = 0; i < table->size; i++)
    {
        for (LinkedList* node = table->buckets[i]; node != NULL; node = node->next)
        {
            printf("Index:%d, Key:%s, Value:%s\n", i, node->item->key.c_str(), node->item->value.c_str());
        }
    }

    printf("-------------------\n\n");
}

void ht_insert(HashTable* table, const std::string& key, const std::string& value){
    unsigned long index = HashFunction(key) % table->size;
    LinkedList* head = table->buckets[index];

    // Key already present: update its value in place, nothing new to allocate.
    for (LinkedList* node = head; node != NULL; node = node->next) {
        if (node->item->key == key) {
            node->item->value = value;
            return;
        }
    }

    // Key not present: append a new item to the bucket's chain.
    HT_Item* new_item = create_item(table, key, value);
    table->buckets[index] = linkedlist_insert(table, head, new_item);
    table->count++;
}

std::string* ht_search(HashTable* table, const std::string& key){
    unsigned long index = HashFunction(key) % table->size;

    LinkedList* head = table->buckets[index];
    while (head != NULL){
        if (head->item->key == key)
            return &head->item->value;
        head = head->next;
    }

    return NULL;
}

bool ht_delete(HashTable* table, const std::string& key)
{
    unsigned long index = HashFunction(key) % table->size;

    if (!ht_search(table, key)) {
        return false;
    }

    table->buckets[index] = linkedlist_delete(table, table->buckets[index], key);
    table->count--;
    return true;
}

void print_search(HashTable* table, const std::string& key){
    std::string* val;
    if ((val = ht_search(table, key)) == NULL){
        printf("Key:%s does not exist\n", key.c_str());
        return;
    }
    else {
        printf("Key:%s, Value:%s\n", key.c_str(), val->c_str());
    }
}


