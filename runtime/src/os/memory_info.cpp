// See memory_info.h.

#include <stdafx.h>
#include "memory_info.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#elif defined(_WIN32)
#include <psapi.h>
#endif

uint64_t MemoryInfo::FootprintMB()
{
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.phys_footprint >> 20;
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return counters.PrivateUsage >> 20;
    return 0;
#else
    return 0;
#endif
}

int64_t MemoryInfo::AvailableMB()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return int64_t(os_proc_available_memory() >> 20);
#else
    return -1;
#endif
}

std::string MemoryInfo::Describe()
{
    const int64_t available = AvailableMB();
    if (available >= 0)
        return fmt::format("footprint {} MB, {} MB left before iOS ends the app", FootprintMB(), available);
    return fmt::format("footprint {} MB", FootprintMB());
}
