#include <stdafx.h>
#include "memory.h"

// The Xbox 360 shows its 512 MB of physical memory three times: at 0xA0000000 (64 KB pages),
// 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB pages). D3D writes through one view and reads
// through another, so these must be real aliases of the same memory, not separate regions.
static constexpr uint64_t kPhysicalSize = 0x20000000;
static constexpr uint64_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

#ifdef _WIN32
static uint8_t* MapGuestMemory(uint8_t* preferred)
{
    // Find a free 4 GB range, then rebuild it as: low 2.5 GB private memory + 3 views of one section.
    auto* probe = (uint8_t*)VirtualAlloc(preferred, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (probe == nullptr)
        return nullptr;
    VirtualFree(probe, 0, MEM_RELEASE);

    if (VirtualAlloc(probe, kPhysicalViews[0], MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != probe)
        return nullptr;

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPhysicalSize), nullptr);
    if (section == nullptr)
    {
        VirtualFree(probe, 0, MEM_RELEASE);
        return nullptr;
    }

    for (uint64_t view : kPhysicalViews)
    {
        if (MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, probe + view) != probe + view)
        {
            CloseHandle(section);
            VirtualFree(probe, 0, MEM_RELEASE);
            return nullptr;
        }
    }

    CloseHandle(section); // the views keep the section alive
    return probe;
}
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>

// The physical window already lives at base + 0xA0000000 (anonymous memory): remap it (shared,
// not copied) over the other two views.
static bool AliasPhysicalViews(uint8_t* base)
{
    for (size_t i = 1; i < std::size(kPhysicalViews); i++)
    {
        vm_address_t target = vm_address_t(base + kPhysicalViews[i]);
        vm_prot_t current, maximum;
        const kern_return_t result = vm_remap(mach_task_self(), &target, vm_size_t(kPhysicalSize), 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), vm_address_t(base + kPhysicalViews[0]),
            FALSE /* share, don't copy */, &current, &maximum, VM_INHERIT_NONE);
        if (result != KERN_SUCCESS || target != vm_address_t(base + kPhysicalViews[i]))
            return false;
    }
    return true;
}
#else
#include <sys/mman.h>
#include <unistd.h>

// Linux: one memfd mapped three times over the reservation.
static bool AliasPhysicalViews(uint8_t* base)
{
    const int fd = memfd_create("dr_physical", 0);
    if (fd < 0 || ftruncate(fd, off_t(kPhysicalSize)) != 0)
        return false;
    for (uint64_t view : kPhysicalViews)
        if (mmap(base + view, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != base + view)
        {
            close(fd);
            return false;
        }
    close(fd);
    return true;
}
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = MapGuestMemory((uint8_t*)0x100000000ull);

    if (base == nullptr)
        base = MapGuestMemory(nullptr);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    // 4 GB of address space (iOS: needs the extended virtual addressing entitlement), then the
    // physical window at 0xA0000000 aliased at 0xC0000000 and 0xE0000000 like on Windows.
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
    {
        base = nullptr;
        return;
    }

    if (!AliasPhysicalViews(base))
        fprintf(stderr, "[memory] could not alias the physical memory views; D3D may read stale data\n");

    // Read-only zeros: a read through a null guest pointer reads zero instead of crashing (Windows
    // does the same per read and logs it: NullReadHandler in main.cpp); writes still crash.
    mprotect(base, 4096, PROT_READ);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
