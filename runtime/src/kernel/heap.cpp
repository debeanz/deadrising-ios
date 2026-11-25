#include <stdafx.h>
#include "heap.h"
#include "memory.h"
#include "function.h"

constexpr size_t RESERVED_BEGIN = 0x7FEA0000;
constexpr size_t RESERVED_END = 0xA0000000;

// Dead Rising layout (the game runs its own heaps on top of NtAllocateVirtualMemory):
//   0x00020000-0x40000000  host-side allocations (kernel objects, thread stacks, XAM buffers)
//   0x40000000-0x7F000000  NtAllocateVirtualMemory pages (see kernel/virtual_memory.cpp)
//   0x82000000-...          XEX image + recompiled function table
//   0xA0000000-0xFFFFFFFF  physical memory (MmAllocatePhysicalMemoryEx)
constexpr size_t USER_HEAP_END = 0x40000000;

void Heap::Init()
{
    heap = o1heapInit(g_memory.Translate(0x20000), USER_HEAP_END - 0x20000);
    // 512 MB, like the console: physical address = virtual & 0x1FFFFFFF must map back to the same
    // block. Allocations above 0xC0000000 made D3D hand the GPU addresses that alias other memory.
    physicalBase = static_cast<uint8_t*>(g_memory.Translate(RESERVED_END));
    physicalSize = 0x20000000;
    PhysicalAddFree(0, uint32_t(physicalSize));
}

void Heap::PhysicalAddFree(uint32_t offset, uint32_t size)
{
    // Merge with the free neighbours.
    auto next = physicalFree.lower_bound(offset);
    if (next != physicalFree.end() && next->first == offset + size)
    {
        size += next->second;
        PhysicalRemoveFree(next);
    }
    auto prev = physicalFree.lower_bound(offset);
    if (prev != physicalFree.begin())
    {
        --prev;
        if (prev->first + prev->second == offset)
        {
            offset = prev->first;
            size += prev->second;
            PhysicalRemoveFree(prev);
        }
    }
    physicalFree.emplace(offset, size);
    physicalFreeBySize.emplace(size, offset);
}

void Heap::PhysicalRemoveFree(std::map<uint32_t, uint32_t>::iterator it)
{
    auto range = physicalFreeBySize.equal_range(it->second);
    for (auto bySize = range.first; bySize != range.second; ++bySize)
        if (bySize->second == it->first)
        {
            physicalFreeBySize.erase(bySize);
            break;
        }
    physicalFree.erase(it);
}

void* Heap::Alloc(size_t size)
{
    std::lock_guard lock(mutex);

    return o1heapAllocate(heap, std::max<size_t>(1, size));
}

void* Heap::AllocPhysical(size_t size, size_t alignment)
{
    // Page granular (as on the console); best fit among the free blocks that hold an aligned start.
    const uint32_t pageSize = 0x1000;
    const uint32_t bytes = uint32_t((std::max<size_t>(1, size) + pageSize - 1) & ~size_t(pageSize - 1));
    const uint32_t align = uint32_t(std::max<size_t>(pageSize, alignment));

    std::lock_guard lock(physicalMutex);

    for (auto it = physicalFreeBySize.lower_bound(bytes); it != physicalFreeBySize.end(); ++it)
    {
        const uint32_t blockOffset = it->second, blockSize = it->first;
        const uint32_t start = (blockOffset + align - 1) & ~(align - 1);
        if (uint64_t(start) + bytes > uint64_t(blockOffset) + blockSize)
            continue;

        PhysicalRemoveFree(physicalFree.find(blockOffset));
        if (start > blockOffset)
            PhysicalAddFree(blockOffset, start - blockOffset);
        if (start + bytes < blockOffset + blockSize)
            PhysicalAddFree(start + bytes, blockOffset + blockSize - (start + bytes));
        physicalBlocks[start] = { start, bytes };
        physicalInUse += bytes;
        return physicalBase + start;
    }

    static bool reported;
    if (!reported)
    {
        reported = true;
        fprintf(stderr, "[heap] out of physical memory: %zu bytes (alignment %zu) requested, %zu MB in use\n",
            size, alignment, physicalInUse >> 20);
    }
    return nullptr;
}

void Heap::Free(void* ptr)
{
    if (ptr >= physicalBase && ptr < physicalBase + physicalSize)
    {
        std::lock_guard lock(physicalMutex);
        const uint32_t offset = uint32_t(static_cast<uint8_t*>(ptr) - physicalBase);
        auto it = physicalBlocks.find(offset);
        if (it != physicalBlocks.end())
        {
            physicalInUse -= it->second.second;
            PhysicalAddFree(it->second.first, it->second.second);
            physicalBlocks.erase(it);
        }
    }
    else
    {
        std::lock_guard lock(mutex);
        o1heapFree(heap, ptr);
    }
}

size_t Heap::Size(void* ptr)
{
    if (ptr >= physicalBase && ptr < physicalBase + physicalSize)
    {
        std::lock_guard lock(physicalMutex);
        auto it = physicalBlocks.find(uint32_t(static_cast<uint8_t*>(ptr) - physicalBase));
        return it != physicalBlocks.end() ? it->second.second : 0;
    }

    if (ptr)
        return *((size_t*)ptr - 2) - O1HEAP_ALIGNMENT; // relies on fragment header in o1heap.c

    return 0;
}

uint32_t RtlAllocateHeap(uint32_t heapHandle, uint32_t flags, uint32_t size)
{
    void* ptr = g_userHeap.Alloc(size);
    if ((flags & 0x8) != 0)
        memset(ptr, 0, size);

    assert(ptr);
    return g_memory.MapVirtual(ptr);
}

uint32_t RtlReAllocateHeap(uint32_t heapHandle, uint32_t flags, uint32_t memoryPointer, uint32_t size)
{
    void* ptr = g_userHeap.Alloc(size);
    if ((flags & 0x8) != 0)
        memset(ptr, 0, size);

    if (memoryPointer != 0)
    {
        void* oldPtr = g_memory.Translate(memoryPointer);
        memcpy(ptr, oldPtr, std::min<size_t>(size, g_userHeap.Size(oldPtr)));
        g_userHeap.Free(oldPtr);
    }

    assert(ptr);
    return g_memory.MapVirtual(ptr);
}

uint32_t RtlFreeHeap(uint32_t heapHandle, uint32_t flags, uint32_t memoryPointer)
{
    if (memoryPointer != NULL)
        g_userHeap.Free(g_memory.Translate(memoryPointer));

    return true;
}

uint32_t RtlSizeHeap(uint32_t heapHandle, uint32_t flags, uint32_t memoryPointer)
{
    if (memoryPointer != NULL)
        return (uint32_t)g_userHeap.Size(g_memory.Translate(memoryPointer));

    return 0;
}

uint32_t XAllocMem(uint32_t size, uint32_t flags)
{
    void* ptr = (flags & 0x80000000) != 0 ?
        g_userHeap.AllocPhysical(size, (1ull << ((flags >> 24) & 0xF))) :
        g_userHeap.Alloc(size);

    if ((flags & 0x40000000) != 0)
        memset(ptr, 0, size);

    assert(ptr);
    return g_memory.MapVirtual(ptr);
}

void XFreeMem(uint32_t baseAddress, uint32_t flags)
{
    if (baseAddress != NULL)
        g_userHeap.Free(g_memory.Translate(baseAddress));
}



