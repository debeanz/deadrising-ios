#pragma once

// Precompiled header for the Dead Rising runtime.
// Structure follows UnleashedRecomp (GPL-3.0); trimmed to what the kernel layer needs.

#define NOMINMAX

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xbox.h>
#include <xxhash.h>
#include <ankerl/unordered_dense.h>
#include <fmt/core.h>
#include <o1heap.h>
#include <ppc/ppc_recomp_shared.h>

#include "framework.h"
#include "mutex.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <pthread.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
