#pragma once

#include <cstdint>

// Guest virtual memory for NtAllocateVirtualMemory & co.
// Pages live in 0x40000000-0x7F000000 of the 4 GB guest space, which is already
// backed by host memory, so "reserve" and "commit" only track state here.
namespace VirtualMemory
{
    constexpr uint32_t RegionBegin = 0x40000000;
    constexpr uint32_t RegionEnd = 0x7F000000;
    constexpr uint32_t PageSize = 0x1000;

    struct RegionInfo
    {
        uint32_t allocationBase;
        uint32_t regionSize;
        uint32_t protect;
        bool committed;
    };

    // Returns the guest address, or 0 when out of space.
    // address != 0 asks for that exact address (reserving or committing inside an existing region).
    uint32_t Allocate(uint32_t address, uint32_t size, uint32_t alignment, bool commit, uint32_t protect);

    // Frees the whole region containing address. Returns false if address is not an allocation base.
    bool Release(uint32_t address);

    // Marks pages as decommitted (contents are zeroed on the next commit).
    void Decommit(uint32_t address, uint32_t size);

    bool Query(uint32_t address, RegionInfo& info);

    bool Contains(uint32_t address);
}
