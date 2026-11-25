#pragma once

#include <cstdint>

namespace dr
{
    // Guest addresses describing the loaded executable, set up by the loader (main.cpp).
    struct ModuleInfo
    {
        uint32_t xexHeader = 0;        // copy of the XEX header in guest memory
        uint32_t ldrDataEntry = 0;     // fake LDR_DATA_TABLE_ENTRY (its address is the module handle)
        uint32_t fakeXamHandle = 0;
        uint32_t fakeKernelHandle = 0;
    };

    void SetModuleInfo(const ModuleInfo& info);
}
