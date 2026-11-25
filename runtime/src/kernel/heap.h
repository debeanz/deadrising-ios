#pragma once

#include "mutex.h"
#include <map>
#include <unordered_map>

struct Heap
{
    Mutex mutex;
    O1HeapInstance* heap;

    // Physical memory (0xA0000000-0xBFFFFFFF, 512 MB like the console): page-granular best fit.
    // o1heap rounds blocks up to powers of two, which wasted enough of it for Dead Rising to run
    // out when the prologue loads.
    Mutex physicalMutex;
    uint8_t* physicalBase = nullptr;
    size_t physicalSize = 0;
    size_t physicalInUse = 0;
    std::map<uint32_t, uint32_t> physicalFree;                                  // offset -> size
    std::multimap<uint32_t, uint32_t> physicalFreeBySize;                       // size -> offset
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> physicalBlocks; // returned offset -> (block offset, size)

    void Init();

    void* Alloc(size_t size);
    void* AllocPhysical(size_t size, size_t alignment);
    void Free(void* ptr);

    size_t Size(void* ptr);

    template<typename T, typename... Args>
    T* Alloc(Args&&... args)
    {
        T* obj = (T*)Alloc(sizeof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }

    template<typename T, typename... Args>
    T* AllocPhysical(Args&&... args)
    {
        T* obj = (T*)AllocPhysical(sizeof(T), alignof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }

private:
    void PhysicalAddFree(uint32_t offset, uint32_t size);
    void PhysicalRemoveFree(std::map<uint32_t, uint32_t>::iterator it);
};

extern Heap g_userHeap;
