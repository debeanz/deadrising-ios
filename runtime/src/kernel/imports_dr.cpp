// Kernel/XAM imports that Dead Rising needs and UnleashedRecomp only stubs (or lacks).
// UnleashedRecomp replaces Sonic's XAPI and D3D code at a higher level, so it never needed
// real file I/O or virtual memory in the kernel. Dead Rising runs its own XAPI code, which
// calls straight into these.
//
// Semantics follow Xenia's kernel implementation where the console behaviour matters.

#include <stdafx.h>
#include <profiler/hitch_stats.h>
#include <cpu/guest_thread.h>
#include <cpu/ppc_context.h>
#include <os/logger.h>
#include "function.h"
#include "heap.h"
#include "memory.h"
#include "xam.h"
#include "xdm.h"
#include "virtual_memory.h"
#include "io/file_system_dr.h"
#include "imports_dr.h"
#include <os/hang_watchdog.h>
#include <gpu/command_processor.h>
#include <apu/audio.h>
#include <apu/xma.h>
#ifdef DR_CALL_PROFILER
#include <profiler/call_profiler.h>
#endif

void SignalEventHandle(uint32_t handle); // imports.cpp

// ---------------------------------------------------------------------------------------------
// User APCs: I/O completion routines run when the issuing thread next waits alertably.
// ---------------------------------------------------------------------------------------------

namespace
{
    struct GuestApc
    {
        uint32_t routine;
        uint32_t context;
        uint32_t arg1;
        uint32_t arg2;
    };

    thread_local std::vector<GuestApc> t_pendingApcs;
}

static void QueueUserApc(uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2)
{
    t_pendingApcs.push_back({ routine, context, arg1, arg2 });
}

static const bool g_traceImports = []
{
    const char* value = getenv("DR_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}();

void TraceImportCall(const char* name, PPCContext& ctx)
{
    HangWatchdog::NoteImport(name, uint32_t(ctx.lr)); // "[watchdog]": each guest thread's last import
    if (!g_traceImports)
        return;

    static std::mutex mutex;
    static std::unordered_map<const char*, uint32_t> counts;

    uint32_t count;
    {
        std::lock_guard lock(mutex);
        count = ++counts[name];
    }

    // DR_TRACE=2: from the first message box on, every call except the frequent locking ones
    // (4000 lines), to see what the game waits on after it.
    // DR_TRACE_FROM=<import name> starts it at that import instead.
    static const bool traceAfterMessageBox = getenv("DR_TRACE")[0] == '2';
    static const char* traceFrom = getenv("DR_TRACE_FROM") ? getenv("DR_TRACE_FROM") : "XamShowMessageBoxUI";
    static std::atomic<bool> tracingAll{ false };
    static std::atomic<uint32_t> tracedAll{ 0 };
    if (traceAfterMessageBox && strstr(name, traceFrom) != nullptr)
        tracingAll = true;
    if (tracingAll && tracedAll < 30000 && strstr(name, "SpinLock") == nullptr && strstr(name, "Irql") == nullptr &&
        strstr(name, "CriticalSection") == nullptr && strstr(name, "CriticalRegion") == nullptr && strstr(name, "KeQueryPerformance") == nullptr &&
        strstr(name, "XamInputGetCapabilities") == nullptr)
    {
        tracedAll++;
        LOGFN("[trace] tid={:04X} {} #{} (r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr={:08X}",
            GuestThread::GetCurrentThreadId() & 0xFFFF, strncmp(name, "__imp__", 7) == 0 ? name + 7 : name, count, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
        return;
    }

    if (count <= 20 || (count & (count - 1)) == 0)
    {
        if (strncmp(name, "__imp__", 7) == 0)
            name += 7;
        LOGFN("[trace] tid={:04X} {} #{} (r3={:08X} r4={:08X} r5={:08X}) lr={:08X}",
            GuestThread::GetCurrentThreadId() & 0xFFFF, name, count, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
    }
}

// Called at the start of every alertable wait (imports.cpp). Returns true if any APC ran,
// in which case the wait returns STATUS_USER_APC like on the console.
bool DeliverUserApcs()
{
    if (t_pendingApcs.empty())
        return false;

    auto* ctx = GetPPCContext();
    while (!t_pendingApcs.empty())
    {
        GuestApc apc = t_pendingApcs.front();
        t_pendingApcs.erase(t_pendingApcs.begin());

        if (g_traceImports)
            LOGFN("[trace] APC 0x{:08X}(0x{:08X}, 0x{:08X})", apc.routine, apc.context, apc.arg1);

        // Run on a copy of the thread's registers so the interrupted code sees no change,
        // with some stack below the caller's frame left untouched.
        PPCContext saved = *ctx;
        ctx->r1.u64 = (saved.r1.u32 - 0x400) & ~0xFull;
        ctx->r3.u64 = apc.context;
        ctx->r4.u64 = apc.arg1;
        ctx->r5.u64 = apc.arg2;
        g_memory.FindFunction(apc.routine)(*ctx, g_memory.base);
        *ctx = saved;
    }

    return true;
}

namespace dr
{

// ---------------------------------------------------------------------------------------------
// Status codes (prefixed to stay clear of the Windows SDK macros)
// ---------------------------------------------------------------------------------------------

constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t X_STATUS_PROCEDURE_NOT_FOUND = 0xC000007A;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;

constexpr uint32_t X_ERROR_SUCCESS = 0;
constexpr uint32_t X_ERROR_FUNCTION_FAILED = 0x65B;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
constexpr uint32_t X_ERROR_IO_PENDING = 997;

// ---------------------------------------------------------------------------------------------
// Tracing: every call into an import that is not fully implemented is logged (first few times)
// ---------------------------------------------------------------------------------------------

static void Trace(const char* name, const char* note = nullptr)
{
    static std::mutex mutex;
    static std::unordered_map<std::string_view, uint32_t> counts;

    uint32_t count;
    {
        std::lock_guard lock(mutex);
        count = ++counts[name];
    }

    if (count > 8 && (count & (count - 1)) != 0)
        return; // after 8 calls, only log powers of two

    auto* ctx = GetPPCContext();
    if (ctx)
    {
        LOGFN_WARNING("[import] {}{}{} (r3={:08X} r4={:08X} r5={:08X} r6={:08X}) lr={:08X} #{}",
            name, note ? " - " : "", note ? note : "", ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, uint32_t(ctx->lr), count);
    }
    else
    {
        LOGFN_WARNING("[import] {}{}{} #{}", name, note ? " - " : "", note ? note : "", count);
    }
}

#define DR_STUB_RETURN(name, value) \
    static uint32_t name##_Stub() { Trace(#name); return (value); }

// ---------------------------------------------------------------------------------------------
// Guest printf (sprintf, vsprintf, _vsnprintf, vswprintf, DbgPrint)
// ---------------------------------------------------------------------------------------------

// Reads 64-bit argument slots: either from registers/stack (variadic call) or from a va_list.
struct GuestArgs
{
    PPCContext* ctx = nullptr;
    uint32_t vaList = 0; // guest pointer, if reading from a va_list
    uint32_t slot = 0;

    uint64_t Next()
    {
        uint32_t i = slot++;
        if (vaList)
            return *(be<uint64_t>*)g_memory.Translate(vaList + i * 8);

        if (i < 8)
        {
            PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
            return regs[i]->u64;
        }

        return *(be<uint64_t>*)g_memory.Translate(ctx->r1.u32 + 0x50 + (i - 8) * 8);
    }
};

static std::string ReadGuestWide(uint32_t address, size_t maxChars = 0x10000)
{
    std::string out;
    if (!address)
        return "(null)";

    auto* p = (be<uint16_t>*)g_memory.Translate(address);
    for (size_t i = 0; i < maxChars; i++)
    {
        uint16_t c = p[i];
        if (!c)
            break;
        out += c < 0x80 ? char(c) : '?';
    }
    return out;
}

// MS-style printf. wideFormat: inside wprintf, %s is wide and %S is narrow.
static std::string GuestFormat(std::string_view fmt, GuestArgs& args, bool wideFormat)
{
    std::string out;
    out.reserve(fmt.size() + 32);

    for (size_t i = 0; i < fmt.size(); i++)
    {
        char c = fmt[i];
        if (c != '%')
        {
            out += c;
            continue;
        }

        if (i + 1 < fmt.size() && fmt[i + 1] == '%')
        {
            out += '%';
            i++;
            continue;
        }

        // Rebuild the spec for the host snprintf: %[flags][width][.precision]conv
        std::string spec = "%";
        size_t j = i + 1;

        while (j < fmt.size() && strchr("-+ #0", fmt[j]))
            spec += fmt[j++];

        if (j < fmt.size() && fmt[j] == '*')
        {
            spec += std::to_string(int32_t(args.Next()));
            j++;
        }
        else
        {
            while (j < fmt.size() && isdigit((unsigned char)fmt[j]))
                spec += fmt[j++];
        }

        if (j < fmt.size() && fmt[j] == '.')
        {
            spec += fmt[j++];
            if (j < fmt.size() && fmt[j] == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                j++;
            }
            else
            {
                while (j < fmt.size() && isdigit((unsigned char)fmt[j]))
                    spec += fmt[j++];
            }
        }

        // Length modifiers
        bool is64 = false, isShort = false, isChar = false, isLong = false, isWideStr = false;
        while (j < fmt.size())
        {
            if (fmt.substr(j, 3) == "I64") { is64 = true; j += 3; }
            else if (fmt.substr(j, 3) == "I32") { j += 3; }
            else if (fmt.substr(j, 2) == "ll") { is64 = true; j += 2; }
            else if (fmt.substr(j, 2) == "hh") { isChar = true; j += 2; }
            else if (fmt[j] == 'h') { isShort = true; j++; }
            else if (fmt[j] == 'l') { isLong = true; j++; }
            else if (fmt[j] == 'w') { isWideStr = true; j++; }
            else if (fmt[j] == 'z' || fmt[j] == 't' || fmt[j] == 'j' || fmt[j] == 'L') { j++; }
            else break;
        }

        if (j >= fmt.size())
            break;

        char conv = fmt[j];
        char buf[512];

        switch (conv)
        {
            case 'd':
            case 'i':
            {
                int64_t v = int64_t(args.Next());
                if (!is64)
                    v = isChar ? int8_t(v) : isShort ? int16_t(v) : int32_t(v);
                snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)v);
                out += buf;
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                uint64_t v = args.Next();
                if (!is64)
                    v = isChar ? uint8_t(v) : isShort ? uint16_t(v) : uint32_t(v);
                snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)v);
                out += buf;
                break;
            }

            case 'p':
            {
                snprintf(buf, sizeof(buf), "%08X", uint32_t(args.Next()));
                out += buf;
                break;
            }

            case 'c':
            case 'C':
            {
                char ch = char(args.Next());
                snprintf(buf, sizeof(buf), (spec + "c").c_str(), ch);
                out += buf;
                break;
            }

            case 's':
            case 'S':
            {
                uint32_t ptr = uint32_t(args.Next());
                // narrow unless: %ls/%ws, %S in narrow printf, or %s in wide printf (MS rules)
                bool wide = isWideStr || (isLong && conv == 's') || (wideFormat ? (conv == 's' && !isShort) : (conv == 'S'));
                std::string s = wide ? ReadGuestWide(ptr) : (ptr ? std::string((const char*)g_memory.Translate(ptr)) : "(null)");
                // snprintf's precision/width still apply
                std::vector<char> big(s.size() + 512);
                snprintf(big.data(), big.size(), (spec + "s").c_str(), s.c_str());
                out += big.data();
                break;
            }

            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                // Variadic doubles travel in the integer slots as raw bits.
                double v = std::bit_cast<double>(args.Next());
                snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
                out += buf;
                break;
            }

            case 'n':
                args.Next(); // never write through %n
                break;

            default:
                out += spec;
                out += conv;
                break;
        }

        i = j;
    }

    return out;
}

static uint32_t sprintf_Impl()
{
    auto* ctx = GetPPCContext();
    uint32_t buffer = ctx->r3.u32;
    GuestArgs args{ ctx, 0, 2 };
    std::string s = GuestFormat((const char*)g_memory.Translate(ctx->r4.u32), args, false);
    memcpy(g_memory.Translate(buffer), s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}

static uint32_t vsprintf_Impl(uint32_t buffer, const char* format, uint32_t vaList)
{
    GuestArgs args{ GetPPCContext(), vaList, 0 };
    std::string s = GuestFormat(format, args, false);
    memcpy(g_memory.Translate(buffer), s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}

static int32_t _vsnprintf_Impl(uint32_t buffer, uint32_t count, const char* format, uint32_t vaList)
{
    GuestArgs args{ GetPPCContext(), vaList, 0 };
    std::string s = GuestFormat(format, args, false);
    char* dst = (char*)g_memory.Translate(buffer);

    if (s.size() > count)
    {
        memcpy(dst, s.c_str(), count);
        return -1;
    }

    memcpy(dst, s.c_str(), s.size());
    if (s.size() < count)
        dst[s.size()] = '\0';
    return int32_t(s.size());
}

static int32_t vswprintf_Impl(uint32_t buffer, uint32_t format, uint32_t vaList)
{
    GuestArgs args{ GetPPCContext(), vaList, 0 };
    std::string s = GuestFormat(ReadGuestWide(format), args, true);
    auto* dst = (be<uint16_t>*)g_memory.Translate(buffer);
    for (size_t i = 0; i < s.size(); i++)
        dst[i] = uint16_t((unsigned char)s[i]);
    dst[s.size()] = 0;
    return int32_t(s.size());
}

static uint32_t DbgPrint_Impl()
{
    auto* ctx = GetPPCContext();
    GuestArgs args{ ctx, 0, 1 };
    std::string s = GuestFormat((const char*)g_memory.Translate(ctx->r3.u32), args, false);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    LOGFN("[DbgPrint] {}", s);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------

constexpr uint32_t X_MEM_COMMIT = 0x1000;
constexpr uint32_t X_MEM_RESERVE = 0x2000;
constexpr uint32_t X_MEM_DECOMMIT = 0x4000;
constexpr uint32_t X_MEM_RELEASE = 0x8000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

static uint32_t NtAllocateVirtualMemory_Impl(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
    uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
    uint32_t requested = *baseAddress;
    uint32_t size = *regionSize;

    if (size == 0)
        return X_STATUS_INVALID_PARAMETER;

    uint32_t alignment = (allocationType & X_MEM_16MB_PAGES) ? 0x1000000 :
                         (allocationType & X_MEM_LARGE_PAGES) ? 0x10000 : 0x1000;

    bool commit = (allocationType & X_MEM_COMMIT) != 0;
    uint32_t address = VirtualMemory::Allocate(requested, (size + alignment - 1) & ~(alignment - 1), alignment, commit, protect);

    if (address == 0)
    {
        LOGFN_ERROR("NtAllocateVirtualMemory failed: base={:08X} size={:08X} type={:08X}", requested, size, allocationType);
        return X_STATUS_NO_MEMORY;
    }

    *baseAddress = address;
    *regionSize = (size + alignment - 1) & ~(alignment - 1);
    return X_STATUS_SUCCESS;
}

static uint32_t NtFreeVirtualMemory_Impl(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
{
    uint32_t address = *baseAddress;

    if (freeType & X_MEM_RELEASE)
    {
        VirtualMemory::RegionInfo info{};
        if (!VirtualMemory::Query(address, info) || !VirtualMemory::Release(info.allocationBase))
            return X_STATUS_MEMORY_NOT_ALLOCATED;

        *baseAddress = info.allocationBase;
        if (regionSize)
            *regionSize = info.regionSize;
        return X_STATUS_SUCCESS;
    }

    if (freeType & X_MEM_DECOMMIT)
    {
        VirtualMemory::Decommit(address, regionSize ? uint32_t(*regionSize) : 0);
        return X_STATUS_SUCCESS;
    }

    return X_STATUS_INVALID_PARAMETER;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> BaseAddress;
    be<uint32_t> AllocationBase;
    be<uint32_t> AllocationProtect;
    be<uint32_t> RegionSize;
    be<uint32_t> State;
    be<uint32_t> Protect;
    be<uint32_t> Type;
};

static uint32_t NtQueryVirtualMemory_Impl(uint32_t address, X_MEMORY_BASIC_INFORMATION* info, uint32_t debugMemory)
{
    VirtualMemory::RegionInfo region{};
    memset(info, 0, sizeof(*info));
    info->BaseAddress = address & ~0xFFFu;

    if (!VirtualMemory::Query(address, region))
    {
        info->State = 0x10000; // MEM_FREE
        info->Protect = 0x01;  // PAGE_NOACCESS
        info->RegionSize = 0x1000;
        return X_STATUS_SUCCESS;
    }

    info->AllocationBase = region.allocationBase;
    info->AllocationProtect = region.protect;
    info->RegionSize = region.allocationBase + region.regionSize - (address & ~0xFFFu);
    info->State = region.committed ? X_MEM_COMMIT : X_MEM_RESERVE;
    info->Protect = region.protect;
    info->Type = 0x20000; // MEM_PRIVATE
    return X_STATUS_SUCCESS;
}

static uint32_t MmQueryAllocationSize_Impl(uint32_t address)
{
    VirtualMemory::RegionInfo region{};
    if (VirtualMemory::Query(address, region))
        return region.regionSize;

    if (address >= 0xA0000000)
        return uint32_t(g_userHeap.Size(g_memory.Translate(address)));

    return 0;
}

static uint32_t MmSetAddressProtect_Impl(uint32_t address, uint32_t size, uint32_t protect)
{
    return X_STATUS_SUCCESS; // the host keeps everything read/write
}

static uint32_t ExAllocatePool_Impl(uint32_t size)
{
    void* ptr = g_userHeap.Alloc(size);
    return ptr ? g_memory.MapVirtual(ptr) : 0;
}

static void ExFreePool_Impl(uint32_t address)
{
    if (address)
        g_userHeap.Free(g_memory.Translate(address));
}

static uint32_t XamAlloc_Impl(uint32_t flags, uint32_t size, be<uint32_t>* out)
{
    void* ptr = g_userHeap.Alloc(size);
    if (!ptr)
        return X_ERROR_FUNCTION_FAILED;

    memset(ptr, 0, size);
    *out = g_memory.MapVirtual(ptr);
    return X_ERROR_SUCCESS;
}

static uint32_t XamFree_Impl(uint32_t address)
{
    if (address)
        g_userHeap.Free(g_memory.Translate(address));
    return X_ERROR_SUCCESS;
}

static void RtlFillMemoryUlong_Impl(uint32_t destination, uint32_t length, uint32_t pattern)
{
    // The pattern is written in guest (big-endian) order.
    auto* p = (be<uint32_t>*)g_memory.Translate(destination);
    for (uint32_t i = 0; i < length / 4; i++)
        p[i] = pattern;
}

static uint32_t RtlCompareMemoryUlong_Impl(uint32_t source, uint32_t length, uint32_t pattern)
{
    auto* p = (be<uint32_t>*)g_memory.Translate(source);
    uint32_t i = 0;
    for (; i < length / 4; i++)
    {
        if (p[i] != pattern)
            break;
    }
    return i * 4;
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

struct DrFileHandle final : KernelObject
{
    static constexpr uint32_t kMagic = 0x44524648; // 'DRFH'
    uint32_t magic = kMagic;

    ~DrFileHandle() override { magic = 0; }

    std::mutex mutex;
    std::fstream stream;
    std::filesystem::path path;
    std::string guestPath;
    bool isDirectory = false;
    uint64_t position = 0;

    // NtQueryDirectoryFile state
    std::vector<std::filesystem::directory_entry> entries;
    size_t entryIndex = 0;
    bool scanStarted = false;
};

constexpr uint32_t FILE_SUPERSEDE = 0;
constexpr uint32_t FILE_OPEN = 1;
constexpr uint32_t FILE_CREATE = 2;
constexpr uint32_t FILE_OPEN_IF = 3;
constexpr uint32_t FILE_OVERWRITE = 4;
constexpr uint32_t FILE_OVERWRITE_IF = 5;

constexpr uint32_t FILE_DIRECTORY_FILE = 0x1;
constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x40;

constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x1;
constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

static std::string GetObjectName(XOBJECT_ATTRIBUTES* attributes)
{
    if (!attributes || !attributes->Name.get())
        return {};

    XANSI_STRING* name = attributes->Name.get();
    const char* buffer = name->Buffer.get();
    std::string path = buffer ? std::string(buffer, name->Length) : std::string();

    // Relative to an open directory handle. 0xFFFFFFFD is ObDosDevices ("\??\": game:, D:, ...),
    // and other values near the top are pseudo-handles; both mean "no root directory".
    uint32_t root = attributes->RootDirectory;
    if (root != 0 && root < 0xFFFFFF00 && IsKernelObject(root))
    {
        auto* dir = GetKernelObject<DrFileHandle>(root);
        if (dir->magic == DrFileHandle::kMagic)
        {
            std::string base = dir->guestPath;
            if (!base.empty() && base.back() != '\\')
                base += '\\';
            path = base + path;
        }
        else
        {
            LOGFN_WARNING("[file] root directory handle 0x{:08X} is not a file handle (path \"{}\")", root, path);
        }
    }

    return path;
}

static uint64_t ToFileTime(std::filesystem::file_time_type t)
{
    // FILETIME: 100 ns since 1601-01-01.
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(t - std::filesystem::file_time_type::clock::now() + system_clock::now());
    int64_t unix100ns = duration_cast<nanoseconds>(sys.time_since_epoch()).count() / 100;
    return uint64_t(unix100ns + 116444736000000000LL);
}

struct X_FILE_NETWORK_OPEN_INFORMATION
{
    be<uint64_t> CreationTime;
    be<uint64_t> LastAccessTime;
    be<uint64_t> LastWriteTime;
    be<uint64_t> ChangeTime;
    be<uint64_t> AllocationSize;
    be<uint64_t> EndOfFile;
    be<uint32_t> FileAttributes;
    be<uint32_t> Pad;
};
static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);

static void FillNetworkOpenInfo(const std::filesystem::path& path, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    std::error_code ec;
    bool dir = std::filesystem::is_directory(path, ec);
    uint64_t size = dir ? 0 : std::filesystem::file_size(path, ec);
    uint64_t time = ToFileTime(std::filesystem::last_write_time(path, ec));

    info->CreationTime = time;
    info->LastAccessTime = time;
    info->LastWriteTime = time;
    info->ChangeTime = time;
    info->AllocationSize = (size + 0xFFF) & ~0xFFFull;
    info->EndOfFile = size;
    info->FileAttributes = dir ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
    info->Pad = 0;
}

static uint32_t NtCreateFile_Impl(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
    XIO_STATUS_BLOCK* ioStatus, be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess,
    uint32_t createDisposition, uint32_t createOptions)
{
    std::string guestPath = GetObjectName(attributes);

    FileSystemDR::Resolved resolved;
    if (!FileSystemDR::Resolve(guestPath, resolved))
    {
        LOGFN_WARNING("[file] unknown device: \"{}\"", guestPath);
        if (ioStatus) { ioStatus->Status = X_STATUS_OBJECT_PATH_NOT_FOUND; ioStatus->Information = 0; }
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    const bool wantsWrite = (desiredAccess & (0x2 | 0x4 | 0x40000000 | 0x10000000)) != 0; // FILE_WRITE_DATA, APPEND, GENERIC_WRITE/ALL
    const bool create = createDisposition != FILE_OPEN && createDisposition != FILE_OVERWRITE;
    const bool truncate = createDisposition == FILE_SUPERSEDE || createDisposition == FILE_OVERWRITE || createDisposition == FILE_OVERWRITE_IF;
    uint32_t information = 1; // FILE_OPENED

    if (!resolved.exists)
    {
        if (!create || !resolved.writable)
        {
            LOGFN_WARNING("[file] not found: \"{}\"", guestPath);
            if (ioStatus) { ioStatus->Status = X_STATUS_OBJECT_NAME_NOT_FOUND; ioStatus->Information = 0; }
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }

        std::error_code ec;
        if (createOptions & FILE_DIRECTORY_FILE)
        {
            std::filesystem::create_directories(resolved.hostPath, ec);
            resolved.isDirectory = true;
        }
        else
        {
            std::filesystem::create_directories(resolved.hostPath.parent_path(), ec);
            std::ofstream(resolved.hostPath, std::ios::binary).close();
        }

        FileSystemDR::NotifyCreated(resolved.hostPath);
        information = 2; // FILE_CREATED
    }
    else if (createDisposition == FILE_CREATE)
    {
        if (ioStatus) { ioStatus->Status = X_STATUS_OBJECT_NAME_COLLISION; ioStatus->Information = 0; }
        return X_STATUS_OBJECT_NAME_COLLISION;
    }

    if (resolved.isDirectory && (createOptions & FILE_NON_DIRECTORY_FILE))
    {
        if (ioStatus) { ioStatus->Status = X_STATUS_FILE_IS_A_DIRECTORY; ioStatus->Information = 0; }
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }

    if (!resolved.isDirectory && (createOptions & FILE_DIRECTORY_FILE))
    {
        if (ioStatus) { ioStatus->Status = X_STATUS_NOT_A_DIRECTORY; ioStatus->Information = 0; }
        return X_STATUS_NOT_A_DIRECTORY;
    }

    auto* handle = CreateKernelObject<DrFileHandle>();
    handle->path = resolved.hostPath;
    handle->guestPath = guestPath;
    handle->isDirectory = resolved.isDirectory;

    if (!resolved.isDirectory)
    {
        std::ios::openmode mode = std::ios::binary | std::ios::in;
        if (wantsWrite && resolved.writable)
        {
            mode |= std::ios::out;
            if (truncate && information != 2)
            {
                mode |= std::ios::trunc;
                information = 3; // FILE_OVERWRITTEN
            }
        }

        handle->stream.open(resolved.hostPath, mode);
        if (!handle->stream.is_open())
        {
            DestroyKernelObject(handle);
            LOGFN_WARNING("[file] could not open: \"{}\"", guestPath);
            if (ioStatus) { ioStatus->Status = X_STATUS_ACCESS_DENIED; ioStatus->Information = 0; }
            return X_STATUS_ACCESS_DENIED;
        }
    }

    LOGFN("[file] open \"{}\" -> {}{}", guestPath, (const char*)resolved.hostPath.filename().u8string().c_str(), resolved.isDirectory ? " (dir)" : "");

    *fileHandle = GetKernelHandle(handle);
    if (ioStatus) { ioStatus->Status = X_STATUS_SUCCESS; ioStatus->Information = information; }
    return X_STATUS_SUCCESS;
}

static uint32_t NtOpenFile_Impl(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
    XIO_STATUS_BLOCK* ioStatus, uint32_t openOptions)
{
    return NtCreateFile_Impl(fileHandle, desiredAccess, attributes, ioStatus, nullptr, 0, 0, FILE_OPEN, openOptions);
}

// I/O finished (always synchronously here): signal the event and queue the completion routine.
// The low bit of the APC routine is a flag (Xenia: "do not queue to I/O ports"), not part of the address.
static void CompleteIo(uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus)
{
    SignalEventHandle(eventHandle);

    if ((apcRoutine & ~1u) != 0 && apcContext != 0)
        QueueUserApc(apcRoutine & ~1u, apcContext, ioStatus ? g_memory.MapVirtual(ioStatus) : 0, 0);
}

static DrFileHandle* GetFile(uint32_t handle)
{
    if (handle == 0 || handle >= 0xFFFFFF00 || !IsKernelObject(handle))
        return nullptr;

    auto* file = GetKernelObject<DrFileHandle>(handle);
    return file->magic == DrFileHandle::kMagic ? file : nullptr;
}

static uint32_t NtReadFile_Impl(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* ioStatus, uint32_t buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    auto* file = GetFile(fileHandle);
    if (!file || file->isDirectory)
        return X_STATUS_INVALID_HANDLE;


    uint32_t status = X_STATUS_SUCCESS;
    uint32_t bytesRead = 0;
    {
        HitchStats::Timer timer(HitchStats::FileRead);
        std::lock_guard lock(file->mutex);

        uint64_t offset = file->position;
        if (byteOffset && uint64_t(*byteOffset) != 0xFFFFFFFFFFFFFFFEull) // FILE_USE_FILE_POINTER_POSITION
            offset = *byteOffset;

        file->stream.clear();
        file->stream.seekg(std::streamoff(offset));
        file->stream.read((char*)g_memory.Translate(buffer), length);
        bytesRead = uint32_t(file->stream.gcount());
        timer.bytes = bytesRead;
        file->position = offset + bytesRead;

        if (bytesRead == 0 && length != 0)
            status = X_STATUS_END_OF_FILE;
    }

    if (ioStatus)
    {
        ioStatus->Status = status;
        ioStatus->Information = bytesRead;
    }

    CompleteIo(eventHandle, apcRoutine, apcContext, ioStatus);
    return status;
}

static uint32_t NtWriteFile_Impl(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* ioStatus, uint32_t buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    auto* file = GetFile(fileHandle);
    if (!file || file->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    {
        std::lock_guard lock(file->mutex);

        uint64_t offset = file->position;
        if (byteOffset && uint64_t(*byteOffset) != 0xFFFFFFFFFFFFFFFEull)
            offset = *byteOffset;

        file->stream.clear();
        file->stream.seekp(std::streamoff(offset));
        file->stream.write((const char*)g_memory.Translate(buffer), length);
        file->stream.flush();
        file->position = offset + length;
    }

    if (ioStatus)
    {
        ioStatus->Status = X_STATUS_SUCCESS;
        ioStatus->Information = length;
    }

    CompleteIo(eventHandle, apcRoutine, apcContext, ioStatus);
    return X_STATUS_SUCCESS;
}

static uint64_t FileSize(DrFileHandle* file)
{
    std::error_code ec;
    return file->isDirectory ? 0 : std::filesystem::file_size(file->path, ec);
}

struct X_FILE_STANDARD_INFORMATION
{
    be<uint64_t> AllocationSize;
    be<uint64_t> EndOfFile;
    be<uint32_t> NumberOfLinks;
    uint8_t DeletePending;
    uint8_t Directory;
    uint8_t Pad[2];
};

static uint32_t NtQueryInformationFile_Impl(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, uint32_t info, uint32_t length, uint32_t infoClass)
{
    auto* file = GetFile(fileHandle);
    if (!file)
        return X_STATUS_INVALID_HANDLE;

    void* out = g_memory.Translate(info);
    uint32_t written = 0;
    uint32_t status = X_STATUS_SUCCESS;

    switch (infoClass)
    {
        case 5: // FileStandardInformation
        {
            auto* s = (X_FILE_STANDARD_INFORMATION*)out;
            uint64_t size = FileSize(file);
            memset(s, 0, sizeof(*s));
            s->AllocationSize = (size + 0xFFF) & ~0xFFFull;
            s->EndOfFile = size;
            s->NumberOfLinks = 1;
            s->Directory = file->isDirectory;
            written = sizeof(*s);
            break;
        }

        case 14: // FilePositionInformation
            *(be<uint64_t>*)out = file->position;
            written = 8;
            break;

        case 20: // FileEndOfFileInformation
            *(be<uint64_t>*)out = FileSize(file);
            written = 8;
            break;

        case 17: // FileAlignmentInformation
            *(be<uint32_t>*)out = 0;
            written = 4;
            break;

        case 34: // FileNetworkOpenInformation
            FillNetworkOpenInfo(file->path, (X_FILE_NETWORK_OPEN_INFORMATION*)out);
            written = sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
            break;

        default:
            LOGFN_WARNING("[file] NtQueryInformationFile: unhandled class {}", infoClass);
            status = X_STATUS_INVALID_PARAMETER;
            break;
    }

    if (ioStatus)
    {
        ioStatus->Status = status;
        ioStatus->Information = written;
    }
    return status;
}

static uint32_t NtSetInformationFile_Impl(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, uint32_t info, uint32_t length, uint32_t infoClass)
{
    auto* file = GetFile(fileHandle);
    if (!file)
        return X_STATUS_INVALID_HANDLE;

    void* in = g_memory.Translate(info);
    uint32_t status = X_STATUS_SUCCESS;

    switch (infoClass)
    {
        case 14: // FilePositionInformation
            file->position = *(be<uint64_t>*)in;
            break;

        case 19: // FileAllocationInformation
        case 20: // FileEndOfFileInformation
        {
            std::lock_guard lock(file->mutex);
            file->stream.flush();
            std::error_code ec;
            std::filesystem::resize_file(file->path, *(be<uint64_t>*)in, ec);
            break;
        }

        case 13: // FileDispositionInformation (delete on close)
            Trace("NtSetInformationFile", "delete-on-close ignored");
            break;

        default:
            LOGFN_WARNING("[file] NtSetInformationFile: unhandled class {}", infoClass);
            status = X_STATUS_INVALID_PARAMETER;
            break;
    }

    if (ioStatus)
    {
        ioStatus->Status = status;
        ioStatus->Information = 0;
    }
    return status;
}

static uint32_t NtQueryFullAttributesFile_Impl(XOBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    std::string guestPath = GetObjectName(attributes);
    FileSystemDR::Resolved resolved;
    if (!FileSystemDR::Resolve(guestPath, resolved) || !resolved.exists)
        return X_STATUS_OBJECT_NAME_NOT_FOUND;

    FillNetworkOpenInfo(resolved.hostPath, info);
    return X_STATUS_SUCCESS;
}

static uint32_t NtQueryVolumeInformationFile_Impl(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus, uint32_t info, uint32_t length, uint32_t infoClass)
{
    uint8_t* out = (uint8_t*)g_memory.Translate(info);
    uint32_t written = 0;
    uint32_t status = X_STATUS_SUCCESS;

    switch (infoClass)
    {
        case 1: // FileFsVolumeInformation
        {
            memset(out, 0, std::min<uint32_t>(length, 0x20));
            *(be<uint32_t>*)(out + 8) = 0x12345678; // serial number
            written = std::min<uint32_t>(length, 0x18);
            break;
        }

        case 3: // FileFsSizeInformation
        {
            *(be<uint64_t>*)(out + 0) = 0x100000;  // total allocation units
            *(be<uint64_t>*)(out + 8) = 0x80000;   // available
            *(be<uint32_t>*)(out + 16) = 32;       // sectors per unit
            *(be<uint32_t>*)(out + 20) = 512;      // bytes per sector
            written = 24;
            break;
        }

        case 5: // FileFsAttributeInformation
        {
            *(be<uint32_t>*)(out + 0) = 0;
            *(be<uint32_t>*)(out + 4) = 255;
            *(be<uint32_t>*)(out + 8) = 4;
            memcpy(out + 12, "FATX", 4);
            written = 16;
            break;
        }

        default:
            LOGFN_WARNING("[file] NtQueryVolumeInformationFile: unhandled class {}", infoClass);
            status = X_STATUS_INVALID_PARAMETER;
            break;
    }

    if (ioStatus)
    {
        ioStatus->Status = status;
        ioStatus->Information = written;
    }
    return status;
}

struct X_FILE_DIRECTORY_INFORMATION
{
    be<uint32_t> NextEntryOffset;
    be<uint32_t> FileIndex;
    be<uint64_t> CreationTime;
    be<uint64_t> LastAccessTime;
    be<uint64_t> LastWriteTime;
    be<uint64_t> ChangeTime;
    be<uint64_t> EndOfFile;
    be<uint64_t> AllocationSize;
    be<uint32_t> FileAttributes;
    be<uint32_t> FileNameLength;
    char FileName[1];
};

static bool MatchMask(std::string_view name, std::string_view mask)
{
    // Wildcards '*' and '?', case-insensitive.
    if (mask.empty() || mask == "*" || mask == "*.*")
        return true;

    size_t n = 0, m = 0, starM = std::string_view::npos, starN = 0;
    while (n < name.size())
    {
        if (m < mask.size() && (mask[m] == '?' || std::tolower((unsigned char)mask[m]) == std::tolower((unsigned char)name[n])))
        {
            n++;
            m++;
        }
        else if (m < mask.size() && mask[m] == '*')
        {
            starM = m++;
            starN = n;
        }
        else if (starM != std::string_view::npos)
        {
            m = starM + 1;
            n = ++starN;
        }
        else
        {
            return false;
        }
    }

    while (m < mask.size() && mask[m] == '*')
        m++;

    return m == mask.size();
}

static uint32_t NtQueryDirectoryFile_Impl(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* ioStatus, uint32_t info, uint32_t length, XANSI_STRING* fileMask, uint32_t restartScan)
{
    auto* dir = GetFile(fileHandle);
    if (!dir || !dir->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    std::string mask;
    if (fileMask && fileMask->Buffer.get())
        mask.assign(fileMask->Buffer.get(), fileMask->Length);

    if (!dir->scanStarted || restartScan)
    {
        dir->entries.clear();
        dir->entryIndex = 0;
        std::error_code ec;
        // increment(ec), not a range-for: that throws on an error, and nothing catches it (on iOS the app just ends).
        for (auto it = std::filesystem::directory_iterator(dir->path, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            auto name = it->path().filename().u8string();
            if (MatchMask((const char*)name.c_str(), mask))
                dir->entries.push_back(*it);
        }
        dir->scanStarted = true;
    }

    // One entry per call, like the console's FATX driver.
    if (dir->entryIndex >= dir->entries.size())
    {
        uint32_t status = dir->entries.empty() ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
        if (ioStatus) { ioStatus->Status = status; ioStatus->Information = 0; }
        CompleteIo(eventHandle, apcRoutine, apcContext, ioStatus);
        return status;
    }

    auto& entry = dir->entries[dir->entryIndex++];
    auto nameU8 = entry.path().filename().u8string();
    std::string name((const char*)nameU8.c_str());

    auto* out = (X_FILE_DIRECTORY_INFORMATION*)g_memory.Translate(info);
    uint32_t needed = uint32_t(offsetof(X_FILE_DIRECTORY_INFORMATION, FileName) + name.size());
    if (needed > length)
        name.resize(length - offsetof(X_FILE_DIRECTORY_INFORMATION, FileName));

    X_FILE_NETWORK_OPEN_INFORMATION netInfo{};
    FillNetworkOpenInfo(entry.path(), &netInfo);

    out->NextEntryOffset = 0;
    out->FileIndex = uint32_t(dir->entryIndex - 1);
    out->CreationTime = netInfo.CreationTime;
    out->LastAccessTime = netInfo.LastAccessTime;
    out->LastWriteTime = netInfo.LastWriteTime;
    out->ChangeTime = netInfo.ChangeTime;
    out->EndOfFile = netInfo.EndOfFile;
    out->AllocationSize = netInfo.AllocationSize;
    out->FileAttributes = netInfo.FileAttributes;
    out->FileNameLength = uint32_t(name.size());
    memcpy(out->FileName, name.data(), name.size());

    if (ioStatus) { ioStatus->Status = X_STATUS_SUCCESS; ioStatus->Information = needed; }
    CompleteIo(eventHandle, apcRoutine, apcContext, ioStatus);
    return X_STATUS_SUCCESS;
}

static uint32_t NtFlushBuffersFile_Impl(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatus)
{
    if (auto* file = GetFile(fileHandle))
    {
        std::lock_guard lock(file->mutex);
        file->stream.flush();
    }

    if (ioStatus) { ioStatus->Status = X_STATUS_SUCCESS; ioStatus->Information = 0; }
    return X_STATUS_SUCCESS;
}

static uint32_t ObCreateSymbolicLink_Impl(XANSI_STRING* link, XANSI_STRING* target)
{
    std::string l(link->Buffer.get(), link->Length);
    std::string t(target->Buffer.get(), target->Length);
    LOGFN("[file] symlink \"{}\" -> \"{}\"", l, t);
    FileSystemDR::AddSymbolicLink(l, t);
    return X_STATUS_SUCCESS;
}

static uint32_t ObDeleteSymbolicLink_Impl(XANSI_STRING* link)
{
    FileSystemDR::RemoveSymbolicLink(std::string(link->Buffer.get(), link->Length));
    return X_STATUS_SUCCESS;
}

static uint32_t IoDismountVolumeByFileHandle_Impl(uint32_t handle)
{
    return X_STATUS_SUCCESS;
}

static uint32_t NtDeviceIoControlFile_Impl(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apcContext,
    XIO_STATUS_BLOCK* ioStatus, uint32_t ioControlCode, uint32_t inputBuffer, uint32_t inputLength,
    uint32_t outputBuffer, uint32_t outputLength)
{
    LOGFN_WARNING("[import] NtDeviceIoControlFile code={:08X} in={} out={}", ioControlCode, inputLength, outputLength);

    if (outputBuffer && outputLength)
        memset(g_memory.Translate(outputBuffer), 0, outputLength);

    if (ioStatus) { ioStatus->Status = X_STATUS_SUCCESS; ioStatus->Information = 0; }
    SignalEventHandle(event);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// XEX / loader
// ---------------------------------------------------------------------------------------------

static ModuleInfo g_module;

static uint32_t XexGetModuleHandle_Impl(const char* name, be<uint32_t>* handle)
{
    std::string n = name ? name : "";
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    if (n.empty() || n == "default.xex")
        *handle = g_module.ldrDataEntry;
    else if (n == "xam.xex")
        *handle = g_module.fakeXamHandle;
    else if (n == "xboxkrnl.exe")
        *handle = g_module.fakeKernelHandle;
    else
    {
        LOGFN_WARNING("[import] XexGetModuleHandle(\"{}\"): unknown module", n);
        *handle = 0;
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    return X_STATUS_SUCCESS;
}

static uint32_t XexGetProcedureAddress_Impl(uint32_t module, uint32_t ordinal, be<uint32_t>* out)
{
    const char* moduleName = module == g_module.fakeXamHandle ? "xam" : module == g_module.fakeKernelHandle ? "xboxkrnl" : "?";
    LOGFN_WARNING("[import] XexGetProcedureAddress({}, ordinal 0x{:X}) - not supported yet", moduleName, ordinal);
    *out = 0;
    return X_STATUS_PROCEDURE_NOT_FOUND;
}

// Returns the value of an optional XEX header field (Xenia's GetOptHeader semantics).
static uint32_t RtlImageXexHeaderField_Impl(uint32_t xexHeaderBase, uint32_t key)
{
    uint8_t* header = (uint8_t*)g_memory.Translate(xexHeaderBase);
    uint32_t count = *(be<uint32_t>*)(header + 0x14);
    auto* fields = (be<uint32_t>*)(header + 0x18);

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t fieldKey = fields[i * 2];
        if (fieldKey != key)
            continue;

        uint32_t value = fields[i * 2 + 1];
        switch (key & 0xFF)
        {
            case 0x00: return value;                                        // value stored inline
            case 0x01: return xexHeaderBase + 0x18 + i * 8 + 4;             // pointer to the inline value
            default:   return xexHeaderBase + value;                        // pointer into the header
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------------------------
// Graphics (Vd*): placeholders until the renderer exists. Logged so the boot trace shows them.
// ---------------------------------------------------------------------------------------------

static std::atomic<uint32_t> g_rptrWriteBack;
static std::atomic<uint32_t> g_interruptCallback;
static std::atomic<uint32_t> g_interruptUserData;
static std::atomic<uint32_t> g_swapCount;

static uint32_t VdInitializeEngines_Impl() { Trace("VdInitializeEngines"); return 1; }
static void VdShutdownEngines_Impl() { Trace("VdShutdownEngines"); }
static void VdInitializeRingBuffer_Impl(uint32_t base, uint32_t sizeLog2) { LOGFN("[gpu] VdInitializeRingBuffer base={:08X} size=2^{}", base, sizeLog2); CommandProcessor::InitializeRingBuffer(base, sizeLog2); }

static void VdEnableRingBufferRPtrWriteBack_Impl(uint32_t address, uint32_t blockLog2)
{
    LOGFN("[gpu] VdEnableRingBufferRPtrWriteBack address={:08X}", address);
    g_rptrWriteBack = address;
    CommandProcessor::EnableReadPointerWriteBack(address, blockLog2);
}

static void VdSetGraphicsInterruptCallback_Impl(uint32_t callback, uint32_t userData)
{
    LOGFN("[gpu] VdSetGraphicsInterruptCallback callback={:08X} data={:08X}", callback, userData);
    g_interruptCallback = callback;
    g_interruptUserData = userData;
    CommandProcessor::SetInterruptCallback(callback, userData);
#ifdef DR_CALL_PROFILER
    CallProfiler::SetDevice(userData);
#endif
}

static void VdSetSystemCommandBufferGpuIdentifierAddress_Impl(uint32_t address) { LOGFN("[gpu] GPU identifier address={:08X}", address); }

struct X_DISPLAY_INFO
{
    be<uint16_t> FrontBufferWidth;
    be<uint16_t> FrontBufferHeight;
    uint8_t FrontBufferColorFormat;
    uint8_t FrontBufferPixelFormat;
    uint8_t Pad[2];
    uint8_t Rest[0x54];
};

static void VdGetCurrentDisplayInformation_Impl(X_DISPLAY_INFO* info)
{
    memset(info, 0, sizeof(*info));
    info->FrontBufferWidth = 1280;
    info->FrontBufferHeight = 720;
}

static void VdGetCurrentDisplayGamma_Impl(be<uint32_t>* type, be<float>* power)
{
    *type = 1;
    *power = 2.22222233f;
}

static uint32_t VdQueryVideoFlags_Impl() { return 0x7; } // widescreen, HD, 720p-capable
static uint32_t VdSetDisplayMode_Impl() { return 0; }
static uint32_t VdIsHSIOTrainingSucceeded_Impl() { return 1; }
static uint32_t VdEnableDisableClockGating_Impl() { return 0; }
static uint32_t VdCallGraphicsNotificationRoutines_Impl() { return 0; }
static uint32_t VdRetrainEDRAMWorker_Impl() { return 0; }

constexpr uint32_t PM4_TYPE2_NOP = 0x80000000;

// D3D asks the kernel to build the scaler setup packets. Like Xenia, fill the space with NOPs
// and report it as used: D3D advances its command pointer by the returned dword count.
static uint32_t VdInitializeScalerCommandBuffer_Impl(uint32_t sourceXY, uint32_t sourceWH, uint32_t scale, uint32_t unk3,
    uint32_t unk4, uint32_t unk5, uint32_t dest, uint32_t destCount)
{
    auto* p = (be<uint32_t>*)g_memory.Translate(dest);
    for (uint32_t i = 0; i < destCount; i++)
        p[i] = PM4_TYPE2_NOP;
    return destCount;
}

static void VdGetSystemCommandBuffer_Impl(uint32_t p0, be<uint32_t>* p1)
{
    if (p0)
    {
        memset(g_memory.Translate(p0), 0, 0x94);
        *(be<uint32_t>*)g_memory.Translate(p0) = 0xBEEF0000;
    }
    if (p1)
        *p1 = 0xBEEF0001;
}

// D3D reserves 64 dwords in the ring buffer for the kernel's swap packet. Fill them with NOPs
// (a real renderer will take the front buffer address from here later).
static uint32_t VdSwap_Impl(uint32_t buffer, uint32_t fetch, uint32_t unk2, uint32_t unk3, uint32_t unk4, be<uint32_t>* frontBuffer)
{
    if (buffer)
    {
        auto* p = (be<uint32_t>*)g_memory.Translate(buffer);
        for (uint32_t i = 0; i < 64; i++)
            p[i] = PM4_TYPE2_NOP;
    }

    CommandProcessor::OnSwap();
    uint32_t n = ++g_swapCount;
    if (n <= 3 || (n & (n - 1)) == 0)
        LOGFN("[gpu] VdSwap #{} (front buffer 0x{:08X})", n, frontBuffer ? uint32_t(*frontBuffer) : 0);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// XAudio render driver (apu/audio.cpp): the game's mixer runs every 256 samples and submits them.
// ---------------------------------------------------------------------------------------------

static uint32_t XAudioRegisterRenderDriverClient_Impl(be<uint32_t>* callback, be<uint32_t>* driver)
{
    LOGFN("[audio] XAudioRegisterRenderDriverClient callback={:08X} context={:08X}", uint32_t(callback[0]), uint32_t(callback[1]));
    *driver = 0x41550000; // any non-zero token
    Audio::RegisterClient(callback[0], callback[1]);
    return X_ERROR_SUCCESS;
}

static uint32_t XAudioUnregisterRenderDriverClient_Impl() { return X_ERROR_SUCCESS; }
static uint32_t XAudioSubmitRenderDriverFrame_Impl(uint32_t driver, void* samples)
{
    Audio::SubmitFrame(samples);
    return X_ERROR_SUCCESS;
}
static uint32_t XAudioGetVoiceCategoryVolume_Impl(uint32_t category, be<float>* volume) { *volume = 1.0f; return X_ERROR_SUCCESS; }

// Contexts come from the APU's context array (apu/xma.cpp): the game numbers them by their offset
// in it for the kick/lock/clear registers.
static uint32_t XMACreateContext_Impl(be<uint32_t>* context)
{
    Trace("XMACreateContext");
    const uint32_t address = Xma::CreateContext();
    *context = address;
    return address != 0 ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}

static uint32_t XMAReleaseContext_Impl(uint32_t context)
{
    if (context)
        Xma::ReleaseContext(context);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// XAM: users, UI, tasks, sessions
// ---------------------------------------------------------------------------------------------

static uint32_t XamUserGetName_Impl(uint32_t userIndex, char* buffer, uint32_t size)
{
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;

    const char name[] = "Player";
    strncpy(buffer, name, size);
    if (size)
        buffer[size - 1] = '\0';
    return X_ERROR_SUCCESS;
}

static uint32_t XamUserGetXUID_Impl(uint32_t userIndex, uint32_t type, be<uint64_t>* xuid)
{
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;

    *xuid = 0xB13EBABEBABEBABEull;
    return X_ERROR_SUCCESS;
}

// Completes an XOVERLAPPED the way XAM does for UI calls that finish at once.
static uint32_t CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result)
{
    if (!overlapped)
        return result;

    overlapped->Error = result;
    overlapped->Length = 0;
    SignalEventHandle(overlapped->hEvent);
    return X_ERROR_IO_PENDING;
}

static uint32_t XamShowSigninUI_Impl(uint32_t panes, uint32_t flags)
{
    Trace("XamShowSigninUI");
    XamNotifyEnqueueEvent(9, 1);  // XN_SYS_UI = opened
    XamNotifyEnqueueEvent(9, 0);  // XN_SYS_UI = closed
    return X_ERROR_SUCCESS;
}

static uint32_t XamShowMessageBoxUIEx_Impl(uint32_t userIndex, uint32_t title, uint32_t text, uint32_t buttonCount,
    uint32_t buttons, uint32_t focusButton, uint32_t flags, uint32_t unk, be<uint32_t>* result, XXOVERLAPPED* overlapped)
{
    LOGFN_WARNING("[xam] message box: \"{}\" / \"{}\"", ReadGuestWide(title, 200), ReadGuestWide(text, 400));
    if (result)
        *result = focusButton;
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamShowDirtyDiscErrorUI_Impl(uint32_t userIndex)
{
    LOGFN_ERROR("[xam] the game reported a disc read error (dirty disc). Check the game files.");
    return X_ERROR_SUCCESS;
}

static uint32_t XamShowGamerCardUIForXUID_Impl() { Trace("XamShowGamerCardUIForXUID"); return X_ERROR_SUCCESS; }
static void XNotifyPositionUI_Impl(uint32_t position) { }

static uint32_t XamContentCreate_Impl(uint32_t userIndex, const char* rootName, const XCONTENT_DATA* contentData, uint32_t flags,
    be<uint32_t>* disposition, be<uint32_t>* licenseMask, XXOVERLAPPED* overlapped)
{
    return XamContentCreateEx(userIndex, rootName, contentData, flags, disposition, licenseMask, 0, 0, overlapped);
}

static uint32_t XamUserCreateStatsEnumerator_Impl() { Trace("XamUserCreateStatsEnumerator"); return X_ERROR_FUNCTION_FAILED; }
static uint32_t XamSessionCreateHandle_Impl() { Trace("XamSessionCreateHandle"); return X_ERROR_FUNCTION_FAILED; }
static uint32_t XamSessionRefObjByHandle_Impl() { Trace("XamSessionRefObjByHandle"); return X_ERROR_FUNCTION_FAILED; }

// XamTaskSchedule(callback, context, unk, out_handle): run the callback on its own guest thread.
static uint32_t XamTaskSchedule_Impl(uint32_t callback, uint32_t context, uint32_t unk, be<uint32_t>* handle)
{
    LOGFN("[xam] XamTaskSchedule callback={:08X} context={:08X}", callback, context);
    auto* thread = GuestThread::Start({ callback, context, 0 }, nullptr);
    if (handle)
        *handle = GetKernelHandle(thread);
    return X_STATUS_SUCCESS;
}

static uint32_t XamTaskShouldExit_Impl() { return 0; }
static uint32_t XamTaskCloseHandle_Impl(uint32_t handle) { return X_STATUS_SUCCESS; }
static uint32_t XamGetExecutionId_Impl(be<uint32_t>* info) { Trace("XamGetExecutionId"); if (info) *info = 0; return X_STATUS_UNSUCCESSFUL; }

static void XamLoaderTerminateTitle_Impl()
{
    LOGFN("[xam] the game asked to exit");
    std::_Exit(0);
}

static uint32_t XamLoaderLaunchTitle_Impl(const char* path, uint32_t flags)
{
    LOGFN_ERROR("[xam] XamLoaderLaunchTitle(\"{}\") - launching another title is not supported", path ? path : "");
    return X_STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------------------------
// Misc kernel
// ---------------------------------------------------------------------------------------------

static void HalReturnToFirmware_Impl(uint32_t routine)
{
    LOGFN("[kernel] HalReturnToFirmware({}) - exiting", routine);
    std::_Exit(0);
}

static uint32_t ExRegisterTitleTerminateNotification_Impl() { return 0; }
static uint32_t ObDereferenceObject_Impl(uint32_t object) { return 0; }

static uint32_t RtlNtStatusToDosError_Impl(uint32_t status)
{
    switch (status)
    {
        case X_STATUS_SUCCESS: return 0;
        case X_STATUS_OBJECT_NAME_NOT_FOUND: return 2;    // ERROR_FILE_NOT_FOUND
        case X_STATUS_OBJECT_PATH_NOT_FOUND: return 3;    // ERROR_PATH_NOT_FOUND
        case X_STATUS_NO_SUCH_FILE: return 2;
        case X_STATUS_ACCESS_DENIED: return 5;            // ERROR_ACCESS_DENIED
        case X_STATUS_INVALID_HANDLE: return 6;           // ERROR_INVALID_HANDLE
        case X_STATUS_NO_MEMORY: return 8;                // ERROR_NOT_ENOUGH_MEMORY
        case X_STATUS_END_OF_FILE: return 38;             // ERROR_HANDLE_EOF
        case X_STATUS_NO_MORE_FILES: return 18;           // ERROR_NO_MORE_FILES
        case X_STATUS_OBJECT_NAME_COLLISION: return 183;  // ERROR_ALREADY_EXISTS
        case X_STATUS_INVALID_PARAMETER: return 87;       // ERROR_INVALID_PARAMETER
        case 0x00000102: return 0x102;                    // STATUS_TIMEOUT -> WAIT_TIMEOUT
        case 0x00000103: return 997;                      // STATUS_PENDING -> ERROR_IO_PENDING
        default:
            if ((status & 0xFFFF0000) == 0x80070000)
                return status & 0xFFFF;
            return 317; // ERROR_MR_MID_NOT_FOUND
    }
}

struct X_TIME_FIELDS
{
    be<uint16_t> Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
};

static void RtlTimeToTimeFields_Impl(be<uint64_t>* time, X_TIME_FIELDS* fields)
{
    // 100 ns ticks since 1601-01-01 -> calendar fields.
    int64_t ticks = int64_t(uint64_t(*time));
    int64_t ms = ticks / 10000;
    int64_t days = ms / 86400000;
    int64_t msOfDay = ms % 86400000;

    fields->Weekday = uint16_t((days + 1) % 7); // 1601-01-01 was a Monday
    fields->Hour = uint16_t(msOfDay / 3600000);
    fields->Minute = uint16_t((msOfDay / 60000) % 60);
    fields->Second = uint16_t((msOfDay / 1000) % 60);
    fields->Milliseconds = uint16_t(msOfDay % 1000);

    // Civil-from-days (Howard Hinnant), shifted from 1601 to 1970.
    int64_t z = days - 134774 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    fields->Year = uint16_t(y + (m <= 2));
    fields->Month = uint16_t(m);
    fields->Day = uint16_t(d);
}

static uint32_t RtlTimeFieldsToTime_Impl(X_TIME_FIELDS* fields, be<uint64_t>* time)
{
    int64_t y = fields->Year, m = fields->Month, d = fields->Day;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t daysSince1970 = era * 146097 + doe - 719468;
    int64_t days = daysSince1970 + 134774;

    int64_t ms = days * 86400000 + int64_t(fields->Hour) * 3600000 + int64_t(fields->Minute) * 60000 +
                 int64_t(fields->Second) * 1000 + int64_t(fields->Milliseconds);
    *time = uint64_t(ms * 10000);
    return 1;
}

static void RtlRaiseException_Impl(uint32_t record)
{
    uint32_t code = record ? uint32_t(*(be<uint32_t>*)g_memory.Translate(record)) : 0;
    if (code == 0x406D1388) // MS_VC_EXCEPTION: SetThreadName
        return;

    LOGFN_ERROR("[kernel] RtlRaiseException code={:08X} lr={:08X}", code, uint32_t(GetPPCContext()->lr));
}

static void RtlUnwind_Impl() { LOGFN_ERROR("[kernel] RtlUnwind called (exceptions are not supported)"); }
static void RtlCaptureContext_Impl() { Trace("RtlCaptureContext"); }
static uint32_t C_specific_handler_Impl() { Trace("__C_specific_handler"); return 1; } // ExceptionContinueSearch

static uint32_t KeTryToAcquireSpinLockAtRaisedIrql_Impl(be<uint32_t>* lock)
{
    uint32_t thread = GetPPCContext()->r13.u32;
    std::atomic_ref<uint32_t> ref(*(uint32_t*)lock);
    uint32_t expected = 0;
    return ref.compare_exchange_strong(expected, ByteSwap(thread)) ? 1 : 0;
}

static void KeLockL2_Impl() { }
static void KeUnlockL2_Impl() { }
static void KeEnterCriticalRegion_Impl() { }
static void KeLeaveCriticalRegion_Impl() { }

static void ExTerminateThread_Impl(uint32_t exitCode)
{
    LOGFN("[kernel] ExTerminateThread({})", exitCode);
    // Guest threads end by returning from their start routine; a thread that terminates itself
    // simply parks here until the process exits.
    while (true)
        std::this_thread::sleep_for(std::chrono::hours(1));
}

// XeCrypt / XeKeys: signature checks always "pass", hashing is real.
static void XeCryptSha_Impl(uint32_t in1, uint32_t len1, uint32_t in2, uint32_t len2, uint32_t in3, uint32_t len3, uint32_t out, uint32_t outLen)
{
    // Not security relevant here (save checksums). Use a stable hash so the game sees consistent values.
    XXH3_state_t* state = XXH3_createState();
    XXH3_128bits_reset(state);
    if (in1 && len1) XXH3_128bits_update(state, g_memory.Translate(in1), len1);
    if (in2 && len2) XXH3_128bits_update(state, g_memory.Translate(in2), len2);
    if (in3 && len3) XXH3_128bits_update(state, g_memory.Translate(in3), len3);
    XXH128_hash_t h = XXH3_128bits_digest(state);
    XXH3_freeState(state);

    uint8_t digest[20]{};
    memcpy(digest, &h, 16);
    memcpy(g_memory.Translate(out), digest, std::min<uint32_t>(outLen, 20));
    Trace("XeCryptSha", "xxHash stand-in, not real SHA-1");
}

static uint32_t XeKeysConsolePrivateKeySign_Impl(uint32_t hash, uint32_t signature)
{
    if (signature)
        memset(g_memory.Translate(signature), 0, 0x228);
    return 1;
}

static uint32_t XeKeysConsoleSignatureVerification_Impl(uint32_t hash, uint32_t signature, be<uint32_t>* complianceResult)
{
    if (complianceResult)
        *complianceResult = 0;
    return 1;
}

// STFS (save containers) and network: not available.
DR_STUB_RETURN(StfsCreateDevice, X_STATUS_UNSUCCESSFUL)
DR_STUB_RETURN(StfsControlDevice, X_STATUS_UNSUCCESSFUL)

// Winsock: report "network down". Dead Rising only uses this for dev tools / Xbox Live.
constexpr uint32_t kWsaNetDown = 10050; // WSAENETDOWN
static uint32_t NetDll_WSAStartup_Impl() { Trace("NetDll_WSAStartup"); return kWsaNetDown; }
static uint32_t NetDll_XNetStartup_Impl() { Trace("NetDll_XNetStartup"); return 0; }
static uint32_t NetDll_SocketError_Impl() { return 0xFFFFFFFF; } // SOCKET_ERROR / INVALID_SOCKET
static uint32_t NetDll___WSAFDIsSet_Impl() { return 0; }
static uint32_t NetDll_inet_addr_Impl() { return 0xFFFFFFFF; } // INADDR_NONE

} // namespace dr

void dr::SetModuleInfo(const ModuleInfo& info)
{
    g_module = info;
}

// ---------------------------------------------------------------------------------------------
// Registrations
// ---------------------------------------------------------------------------------------------

using namespace dr;

// printf family
PPC_FUNC(__imp__sprintf) { ctx.r3.u64 = sprintf_Impl(); }
GUEST_FUNCTION_HOOK(__imp__vsprintf, vsprintf_Impl);
GUEST_FUNCTION_HOOK(__imp___vsnprintf, _vsnprintf_Impl);
GUEST_FUNCTION_HOOK(__imp__vswprintf, vswprintf_Impl);
PPC_FUNC(__imp__DbgPrint) { ctx.r3.u64 = DbgPrint_Impl(); }

// Memory
GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory_Impl);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory_Impl);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory_Impl);
GUEST_FUNCTION_HOOK(__imp__MmQueryAllocationSize, MmQueryAllocationSize_Impl);
GUEST_FUNCTION_HOOK(__imp__MmSetAddressProtect, MmSetAddressProtect_Impl);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePool, ExAllocatePool_Impl);
GUEST_FUNCTION_HOOK(__imp__ExFreePool, ExFreePool_Impl);
GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc_Impl);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong_Impl);

// Files
GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile_Impl);
GUEST_FUNCTION_HOOK(__imp__NtDeviceIoControlFile, NtDeviceIoControlFile_Impl);
GUEST_FUNCTION_HOOK(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink_Impl);
GUEST_FUNCTION_HOOK(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink_Impl);
GUEST_FUNCTION_HOOK(__imp__IoDismountVolumeByFileHandle, IoDismountVolumeByFileHandle_Impl);

// XEX
GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle_Impl);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField_Impl);

// Graphics
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines_Impl);
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines_Impl);
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer_Impl);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack_Impl);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback_Impl);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress_Impl);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation_Impl);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma_Impl);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags_Impl);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode_Impl);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded_Impl);
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating_Impl);
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines_Impl);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker_Impl);
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer_Impl);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer_Impl);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap_Impl);

// Audio
GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient_Impl);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient_Impl);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame_Impl);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume_Impl);
GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext_Impl);
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext_Impl);

// XAM
GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName_Impl);
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID_Impl);
GUEST_FUNCTION_HOOK(__imp__XamShowSigninUI, XamShowSigninUI_Impl);
GUEST_FUNCTION_HOOK(__imp__XamShowMessageBoxUIEx, XamShowMessageBoxUIEx_Impl);
GUEST_FUNCTION_HOOK(__imp__XamShowDirtyDiscErrorUI, XamShowDirtyDiscErrorUI_Impl);
GUEST_FUNCTION_HOOK(__imp__XamShowGamerCardUIForXUID, XamShowGamerCardUIForXUID_Impl);
GUEST_FUNCTION_HOOK(__imp__XNotifyPositionUI, XNotifyPositionUI_Impl);
GUEST_FUNCTION_HOOK(__imp__XamContentCreate, XamContentCreate_Impl);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator_Impl);
GUEST_FUNCTION_HOOK(__imp__XamSessionCreateHandle, XamSessionCreateHandle_Impl);
GUEST_FUNCTION_HOOK(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle_Impl);
GUEST_FUNCTION_HOOK(__imp__XamTaskSchedule, XamTaskSchedule_Impl);
GUEST_FUNCTION_HOOK(__imp__XamTaskShouldExit, XamTaskShouldExit_Impl);
GUEST_FUNCTION_HOOK(__imp__XamTaskCloseHandle, XamTaskCloseHandle_Impl);
GUEST_FUNCTION_HOOK(__imp__XamGetExecutionId, XamGetExecutionId_Impl);
GUEST_FUNCTION_HOOK(__imp__XamLoaderTerminateTitle, XamLoaderTerminateTitle_Impl);
GUEST_FUNCTION_HOOK(__imp__XamLoaderLaunchTitle, XamLoaderLaunchTitle_Impl);

// Kernel misc
GUEST_FUNCTION_HOOK(__imp__HalReturnToFirmware, HalReturnToFirmware_Impl);
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification_Impl);
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlRaiseException, RtlRaiseException_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlUnwind, RtlUnwind_Impl);
GUEST_FUNCTION_HOOK(__imp__RtlCaptureContext, RtlCaptureContext_Impl);
GUEST_FUNCTION_HOOK(__imp____C_specific_handler, C_specific_handler_Impl);
GUEST_FUNCTION_HOOK(__imp__KeTryToAcquireSpinLockAtRaisedIrql, KeTryToAcquireSpinLockAtRaisedIrql_Impl);
GUEST_FUNCTION_HOOK(__imp__KeLockL2, KeLockL2_Impl);
GUEST_FUNCTION_HOOK(__imp__KeUnlockL2, KeUnlockL2_Impl);
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion_Impl);
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion_Impl);
GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread_Impl);
GUEST_FUNCTION_HOOK(__imp__XeCryptSha, XeCryptSha_Impl);
GUEST_FUNCTION_HOOK(__imp__XeKeysConsolePrivateKeySign, XeKeysConsolePrivateKeySign_Impl);
GUEST_FUNCTION_HOOK(__imp__XeKeysConsoleSignatureVerification, XeKeysConsoleSignatureVerification_Impl);
GUEST_FUNCTION_HOOK(__imp__StfsCreateDevice, StfsCreateDevice_Stub);
GUEST_FUNCTION_HOOK(__imp__StfsControlDevice, StfsControlDevice_Stub);

// Network
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_listen, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_accept, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_SocketError_Impl);
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_SocketError_Impl);

// The game's own printf-style debug output (sub_82531E68: format in r3, arguments in r4...). Its
// release build prints nothing; ten call sites, among them the sound banks' load failures
// ("rSoundSnd::load:Warning! Memory malloc error or required size is 0.").
PPC_FUNC_IMPL(__imp__sub_82531E68);
PPC_FUNC(sub_82531E68)
{
    GuestArgs args{ &ctx, 0, 1 };
    std::string s = GuestFormat((const char*)g_memory.Translate(ctx.r3.u32), args, false);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    LOGFN("[game] {}", s);
    __imp__sub_82531E68(ctx, base);
}
