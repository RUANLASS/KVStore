#pragma once
#include "../include/hashtable.hpp"
#include "../include/allocator.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef BENCH_PLAIN_ALLOCATOR
// static SlabAllocator<LinkedList> linkedlist_allocator;
#endif

LinkedList* allocate_list(HashTable* table)
{
    // Allocates memory for a LinkedList pointer, with `next` starting NULL.
#ifdef BENCH_PLAIN_ALLOCATOR
    LinkedList* list = (LinkedList*) malloc(sizeof(LinkedList));
    list->item = NULL;
    list->next = NULL;
    return list;
#else
    // Value-initialized via the slab allocator, so `next` starts NULL too.
    return table->list_allocator->allocate();
#endif
}

LinkedList* linkedlist_insert(HashTable* table, LinkedList* list, HT_Item* item)
{
    // Inserts the item onto the LinkedList.
    if (!list)
    {
        LinkedList* head = allocate_list(table);
        head->item = item;
        head->next = NULL;
        list = head;
        return list;
    }
    else if (list->next == NULL)
    {
        LinkedList* node = allocate_list(table);
        node->item = item;
        node->next = NULL;
        list->next = node;
        return list;
    }

    LinkedList* temp = list;

    while (temp->next)
    {
        temp = temp->next;
    }

    LinkedList* node = allocate_list(table);
    node->item = item;
    node->next = NULL;
    temp->next = node;
    return list;
}

static void free_list_node(HashTable* table, LinkedList* node) {
    free_item(table, node->item);
#ifdef BENCH_PLAIN_ALLOCATOR
    free(node);
#else
    table->list_allocator->deallocate(node);
#endif
}

LinkedList* linkedlist_delete(HashTable* table, LinkedList* list, const std::string& key){
    if (!list) {
        return list;
    }

    if (list->item->key == key) {
        // Match at the head: the node after it becomes the new head.
        LinkedList* next = list->next;
        free_list_node(table, list);
        return next;
    }

    LinkedList* prev = list;
    LinkedList* curr = list->next;
    while (curr) {
        if (curr->item->key == key) {
            prev->next = curr->next;
            free_list_node(table, curr);
            return list;
        }
        prev = curr;
        curr = curr->next;
    }

    // Key not found; list is unchanged.
    return list;
}

void free_linkedlist(HashTable* table, LinkedList* list)
{
    LinkedList* temp = list;

    while (list)
    {
        temp = list;
        list = list->next;
        free_list_node(table, temp);
    }
}