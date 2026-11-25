#pragma once

// Included into every recompiled source: ppc_config.h includes it when PPC_INCLUDE_DETAIL is
// defined (runtime/CMakeLists.txt, DRRecompLib). Stores XenonRecomp marks as MMIO (the ones
// followed by eieio) still write guest memory as before, and also reach the runtime, which acts on
// the registers it models: the XMA decoder's kick/lock/clear (apu/xma.cpp).

#include <cstdint>

void DrMmioWrite32(uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y)                         \
    do                                                 \
    {                                                  \
        const uint32_t mmioAddress_ = (x);             \
        const uint32_t mmioValue_ = (y);               \
        PPC_STORE_U32(mmioAddress_, mmioValue_);       \
        DrMmioWrite32(mmioAddress_, mmioValue_);       \
    } while (0)
