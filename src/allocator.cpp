#pragma once
#include <algorithm>
#include <iostream>
#include <cstddef>
#include <new>
#include <utility>

template <typename T, std::size_t ObjectsPerSlab = 64>
class SlabAllocator {
private:
    struct Slab; // forward declaration so SlotHeader can point back to its owner

    struct Node {
        Node* next;
    };

    // Sits immediately before each slot's storage so deallocate() can find its
    // owning Slab in O(1) instead of scanning every slab's memory range.
    struct SlotHeader {
        Slab* owner;
    };

    static constexpr std::size_t align_up(std::size_t n, std::size_t align) {
        return (n + align - 1) / align * align;
    }

    static constexpr std::size_t storage_size = std::max(sizeof(T), sizeof(Node));
    static constexpr std::size_t storage_align = std::max(alignof(T), alignof(Node));
    // Where each slot's object storage begins, right after its SlotHeader.
    static constexpr std::size_t data_offset = align_up(sizeof(SlotHeader), storage_align);
    static constexpr std::size_t slot_align = std::max(storage_align, alignof(SlotHeader));
    // Round the whole slot up to slot_align so every slot in the pool stays
    // aligned, not just the first one.
    static constexpr std::size_t slot_size = align_up(data_offset + storage_size, slot_align);

    struct Slab {
        std::byte* memoryPool;     // Pointer to the raw contiguous chunks
        Node* freeListHead;        // Points to the first available slot in this slab
        std::size_t allocatedCount;// Tracking how many objects are active
        Slab* prev;                // Doubly-linked list pointers
        Slab* next;

        Slab() : memoryPool(nullptr), freeListHead(nullptr), allocatedCount(0), prev(nullptr), next(nullptr) {
            memoryPool = static_cast<std::byte*>(::operator new[](slot_size * ObjectsPerSlab, std::align_val_t{slot_align}));

            for (std::size_t i = 0; i < ObjectsPerSlab; ++i) {
                std::byte* slotBase = memoryPool + (i * slot_size);

                // Stamp this slab as the owner of the slot, once, up front.
                reinterpret_cast<SlotHeader*>(slotBase)->owner = this;

                Node* node = reinterpret_cast<Node*>(slotBase + data_offset);
                node->next = freeListHead;
                freeListHead = node;
            }
        }

        ~Slab() {
            ::operator delete[](memoryPool, std::align_val_t{slot_align});
        }
    };

    // State list heads for tracking slab fullness states
    Slab* partialSlabs = nullptr;
    Slab* fullSlabs = nullptr;
    Slab* freeSlabs = nullptr;

    // Helper functions to manage the lists
    void removeSlabFromList(Slab*& listHead, Slab* slab) {
        if (slab->prev) slab->prev->next = slab->next;
        if (slab->next) slab->next->prev = slab->prev;
        if (listHead == slab) listHead = slab->next;
        slab->prev = nullptr;
        slab->next = nullptr;
    }

    void insertSlabIntoList(Slab*& listHead, Slab* slab) {
        slab->next = listHead;
        if (listHead) {
            listHead->prev = slab;
        }
        listHead = slab;
    }

public:
    SlabAllocator() = default;

    ~SlabAllocator() {
        // Purge all memory owned across states
        auto clearList = [](Slab* head) {
            while (head) {
                Slab* temp = head;
                head = head->next;
                delete temp;
            }
        };
        clearList(partialSlabs);
        clearList(fullSlabs);
        clearList(freeSlabs);
    }

    // Allocate memory chunk and construct object in place
    template <typename... Args>
    T* allocate(Args&&... args) {
        Slab* targetSlab = nullptr;

        // 1. Prioritize extracting from a partially filled slab
        if (partialSlabs) {
            targetSlab = partialSlabs;
        }
        // 2. Fall back to an existing completely empty slab
        else if (freeSlabs) {
            targetSlab = freeSlabs;
            removeSlabFromList(freeSlabs, targetSlab);
            insertSlabIntoList(partialSlabs, targetSlab);
        }
        // 3. Instantiate a fresh new slab block on total depletion
        else {
            targetSlab = new Slab();
            insertSlabIntoList(partialSlabs, targetSlab);
        }

        // Pop the first available free slot from the selected slab's freelist
        Node* allocatedNode = targetSlab->freeListHead;
        targetSlab->freeListHead = allocatedNode->next;
        targetSlab->allocatedCount++;

        // Shift slab to full tracker list if capacity is met
        if (targetSlab->allocatedCount == ObjectsPerSlab) {
            removeSlabFromList(partialSlabs, targetSlab);
            insertSlabIntoList(fullSlabs, targetSlab);
        }

        // Return perfectly-forwarded Constructed Instance pointer via Placement New
        T* objectPtr = reinterpret_cast<T*>(allocatedNode);
        return ::new (static_cast<void*>(objectPtr)) T(std::forward<Args>(args)...);
    }

    // Deallocate object and call its destructor
    void deallocate(T* ptr) {
        if (!ptr) return;

        // Call explicit cleanup destructor
        ptr->~T();

        // Read the owning slab straight out of the header stored right before
        // this slot's storage -- O(1), no scanning across slabs required.
        std::byte* slotBase = reinterpret_cast<std::byte*>(ptr) - data_offset;
        Slab* currentSlab = reinterpret_cast<SlotHeader*>(slotBase)->owner;

        // If it was full, it drops to partial state upon receiving a slot back.
        if (currentSlab->allocatedCount == ObjectsPerSlab) {
            removeSlabFromList(fullSlabs, currentSlab);
            insertSlabIntoList(partialSlabs, currentSlab);
        }

        // Insert the slot back to the front of the owning slab's freelist
        Node* freedNode = reinterpret_cast<Node*>(ptr);
        freedNode->next = currentSlab->freeListHead;
        currentSlab->freeListHead = freedNode;
        currentSlab->allocatedCount--;

        // If the slab becomes empty, move it to the free list to reduce pressure
        if (currentSlab->allocatedCount == 0) {
            removeSlabFromList(partialSlabs, currentSlab);
            insertSlabIntoList(freeSlabs, currentSlab);
        }
    }
};
