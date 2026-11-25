#include <stdafx.h>
#include "virtual_memory.h"
#include "memory.h"

namespace
{
    constexpr uint32_t PageCount = (VirtualMemory::RegionEnd - VirtualMemory::RegionBegin) / VirtualMemory::PageSize;

    enum PageState : uint8_t
    {
        Free = 0,
        Reserved = 1,
        Committed = 2,
    };

    struct Allocation
    {
        uint32_t size;
        uint32_t protect;
    };

    std::mutex g_mutex;
    std::vector<uint8_t> g_pages(PageCount, Free);
    std::map<uint32_t, Allocation> g_allocations; // base -> region

    uint32_t PageIndex(uint32_t address)
    {
        return (address - VirtualMemory::RegionBegin) / VirtualMemory::PageSize;
    }

    uint32_t PageAddress(uint32_t index)
    {
        return VirtualMemory::RegionBegin + index * VirtualMemory::PageSize;
    }

    // Allocation containing address, or end().
    std::map<uint32_t, Allocation>::iterator FindAllocation(uint32_t address)
    {
        auto it = g_allocations.upper_bound(address);
        if (it == g_allocations.begin())
            return g_allocations.end();

        --it;
        return address < it->first + it->second.size ? it : g_allocations.end();
    }

    void CommitPages(uint32_t first, uint32_t count)
    {
        for (uint32_t i = first; i < first + count; i++)
        {
            if (g_pages[i] != Committed)
            {
                // Fresh commits read as zero, like on the console.
                memset(g_memory.Translate(PageAddress(i)), 0, VirtualMemory::PageSize);
                g_pages[i] = Committed;
            }
        }
    }
}

bool VirtualMemory::Contains(uint32_t address)
{
    return address >= RegionBegin && address < RegionEnd;
}

uint32_t VirtualMemory::Allocate(uint32_t address, uint32_t size, uint32_t alignment, bool commit, uint32_t protect)
{
    std::lock_guard lock(g_mutex);

    size = (size + PageSize - 1) & ~(PageSize - 1);
    alignment = std::max(alignment, PageSize);
    if (size == 0)
        return 0;

    if (address != 0)
    {
        if (!Contains(address) || address + size > RegionEnd)
            return 0;

        address &= ~(PageSize - 1);
        uint32_t first = PageIndex(address);
        uint32_t count = size / PageSize;

        auto existing = FindAllocation(address);
        if (existing != g_allocations.end())
        {
            // Committing (part of) a region that is already reserved.
            if (address + size > existing->first + existing->second.size)
                return 0;

            if (commit)
                CommitPages(first, count);

            return address;
        }

        for (uint32_t i = first; i < first + count; i++)
        {
            if (g_pages[i] != Free)
                return 0;
        }

        for (uint32_t i = first; i < first + count; i++)
            g_pages[i] = Reserved;

        if (commit)
            CommitPages(first, count);

        g_allocations[address] = { size, protect };
        return address;
    }

    // First fit. Allocations are few and large, so a linear scan is fine.
    const uint32_t count = size / PageSize;
    const uint32_t step = alignment / PageSize;

    for (uint32_t first = 0; first + count <= PageCount; first += step)
    {
        uint32_t i = first;
        while (i < first + count && g_pages[i] == Free)
            i++;

        if (i == first + count)
        {
            for (uint32_t j = first; j < first + count; j++)
                g_pages[j] = Reserved;

            if (commit)
                CommitPages(first, count);

            uint32_t base = PageAddress(first);
            g_allocations[base] = { size, protect };
            return base;
        }

        // Skip past the page that blocked this candidate.
        first = ((i / step) * step);
    }

    return 0;
}

bool VirtualMemory::Release(uint32_t address)
{
    std::lock_guard lock(g_mutex);

    auto it = g_allocations.find(address);
    if (it == g_allocations.end())
        return false;

    uint32_t first = PageIndex(it->first);
    uint32_t count = it->second.size / PageSize;
    for (uint32_t i = first; i < first + count; i++)
        g_pages[i] = Free;

    g_allocations.erase(it);
    return true;
}

void VirtualMemory::Decommit(uint32_t address, uint32_t size)
{
    std::lock_guard lock(g_mutex);

    if (!Contains(address))
        return;

    size = (size + PageSize - 1) & ~(PageSize - 1);
    uint32_t first = PageIndex(address & ~(PageSize - 1));
    uint32_t count = std::min<uint32_t>(size / PageSize, PageCount - first);
    for (uint32_t i = first; i < first + count; i++)
    {
        if (g_pages[i] == Committed)
            g_pages[i] = Reserved;
    }
}

bool VirtualMemory::Query(uint32_t address, RegionInfo& info)
{
    std::lock_guard lock(g_mutex);

    auto it = FindAllocation(address);
    if (it == g_allocations.end())
        return false;

    info.allocationBase = it->first;
    info.regionSize = it->second.size;
    info.protect = it->second.protect;
    info.committed = g_pages[PageIndex(address)] == Committed;
    return true;
}
