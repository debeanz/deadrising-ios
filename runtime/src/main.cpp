// Dead Rising (Xbox 360) static recompilation runtime - entry point and XEX loader.
// Based on UnleashedRecomp's runtime structure (GPL-3.0).

#include <stdafx.h>
#include <profiler/cpu_sampler.h>
void DumpPlayerTrace(); // gpu/d3d_hooks.cpp: Frank's held item trace
#include <cpu/guest_thread.h>
#include <cpu/ppc_context.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/imports_dr.h>
#include <kernel/io/file_system_dr.h>
#include <gpu/video.h>
#include <apu/xma.h>
#include <user/config.h>
#include <os/logger.h>
#include <dr_shims.h>
#include <file.h>
#include <image.h>
#include <xex.h>

#ifdef DR_CALL_PROFILER
#include <profiler/call_profiler.h>
#endif

Memory g_memory;
Heap g_userHeap;

// ---------------------------------------------------------------------------------------------
// Kernel export table with types (XenonUtils only keeps the names)
// ---------------------------------------------------------------------------------------------

namespace
{
    struct ExportInfo
    {
        uint32_t ordinal;
        const char* name;
        const char* type; // "kFunction" or "kVariable" (stringified: immune to stray macros)

        bool IsVariable() const { return strcmp(type, "kVariable") == 0; }
    };

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { ORDINAL, #NAME, #TYPE } // the .inc lines supply the comma
    const ExportInfo g_kernelExports[] =
    {
#include "xbox/xboxkrnl_table.inc"
    };

    const ExportInfo g_xamExports[] =
    {
#include "xbox/xam_table.inc"
    };
#undef XE_EXPORT

    const ExportInfo* FindExport(std::string_view library, uint32_t ordinal)
    {
        auto search = [&](auto& table) -> const ExportInfo*
        {
            for (auto& e : table)
            {
                if (e.ordinal == ordinal)
                    return &e;
            }
            return nullptr;
        };

        if (library == "xboxkrnl.exe")
            return search(g_kernelExports);
        if (library == "xam.xex")
            return search(g_xamExports);
        return nullptr;
    }

    uint32_t GuestAlloc(size_t size)
    {
        void* p = g_userHeap.Alloc(size);
        memset(p, 0, size);
        return g_memory.MapVirtual(p);
    }

    uint32_t GuestString(std::string_view s)
    {
        uint32_t p = GuestAlloc(s.size() + 1);
        memcpy(g_memory.Translate(p), s.data(), s.size());
        return p;
    }

    template<typename T>
    T& Guest(uint32_t address)
    {
        return *(T*)g_memory.Translate(address);
    }

    // KeTimeStampBundle, refreshed every millisecond like the console's timer interrupt.
    uint32_t g_timeStampBundle = 0;

    void TimeStampThread()
    {
        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            auto now = std::chrono::steady_clock::now();
            uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            int64_t unix100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100;

            Guest<be<uint64_t>>(g_timeStampBundle + 0x00) = ms * 10000;                              // InterruptTime
            Guest<be<uint64_t>>(g_timeStampBundle + 0x08) = uint64_t(unix100ns + 116444736000000000LL); // SystemTime
            Guest<be<uint32_t>>(g_timeStampBundle + 0x10) = uint32_t(ms);                            // TickCount

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Fills the storage behind a kernel variable import.
    uint32_t CreateVariable(std::string_view name, const dr::ModuleInfo& module)
    {
        if (name == "XexExecutableModuleHandle")
        {
            uint32_t p = GuestAlloc(4);
            Guest<be<uint32_t>>(p) = module.ldrDataEntry;
            return p;
        }

        if (name == "KeTimeStampBundle")
        {
            g_timeStampBundle = GuestAlloc(0x18);
            return g_timeStampBundle;
        }

        if (name == "XboxHardwareInfo")
        {
            uint32_t p = GuestAlloc(0x10);
            Guest<be<uint32_t>>(p) = 0x20; // has hard drive
            Guest<uint8_t>(p + 4) = 6;     // hardware threads
            return p;
        }

        if (name == "XboxKrnlVersion" || name == "XboxKrnlBaseVersion")
        {
            uint32_t p = GuestAlloc(8);
            Guest<be<uint16_t>>(p + 0) = 2;
            Guest<be<uint16_t>>(p + 2) = 0;
            Guest<be<uint16_t>>(p + 4) = 17559;
            Guest<be<uint16_t>>(p + 6) = 0;
            return p;
        }

        if (name == "ExLoadedImageName")
            return GuestString("\\Device\\Cdrom0\\default.xex");

        if (name == "ExLoadedCommandLine")
            return GuestString("\"default.xex\"");

        if (name == "ExConsoleGameRegion")
        {
            uint32_t p = GuestAlloc(4);
            Guest<be<uint32_t>>(p) = 0xFFFFFFFF;
            return p;
        }

        if (name == "VdGpuClockInMHz")
        {
            uint32_t p = GuestAlloc(4);
            Guest<be<uint32_t>>(p) = 500;
            return p;
        }

        // Object types, debug data, VdGlobalDevice, ...: zeroed storage is what the console has
        // until something fills it in.
        return GuestAlloc(0x100);
    }

    // Load default.xex into guest memory and wire up its imports. Returns the entry point.
    uint32_t LoadExecutable(const std::filesystem::path& path)
    {
        std::vector<uint8_t> file = LoadFile(path);
        if (file.empty())
        {
            LOGFN_ERROR("Could not read {}", (const char*)path.u8string().c_str());
            return 0;
        }

        // Decrypts and decompresses; also replaces import thunks with no-op code
        // (the recompiled __imp__ functions sit at those addresses).
        Image image = Image::ParseImage(file.data(), file.size());
        if (!image.data)
        {
            LOGFN_ERROR("Could not decode {}", (const char*)path.u8string().c_str());
            return 0;
        }

        // PE headers, then every section at its virtual address. Clamp to the image: Dead Rising's
        // .reloc claims to extend past it (0x829B5274 > 0x82950000), and the recompiled function
        // table starts right at the image end. Relocations are not needed (fixed load address).
        LOGFN("Decoded image: base 0x{:08X}, size 0x{:X}, {} sections", image.base, image.size, image.sections.size());
        memcpy(g_memory.Translate(image.base), image.data.get(), 0x1000);
        for (auto& section : image.sections)
        {
            size_t offset = section.base - image.base;
            if (offset >= image.size || section.name == ".reloc")
                continue; // .reloc: not needed, and the decoded buffer does not fully cover it

            size_t size = std::min<size_t>(section.size, image.size - offset);
            LOGFN("  section {:<8} 0x{:08X} size 0x{:X} (data at image+0x{:X})", section.name, section.base, size,
                (size_t)(section.data - image.data.get()));
            memcpy(g_memory.Translate(section.base), section.data, size);
        }

        LOGFN("Loaded image 0x{:08X}-0x{:08X}, entry 0x{:08X}", image.base, image.base + image.size, image.entry_point);

        // Debugging: DR_IMAGE_DUMP=<file> writes the loaded image (big-endian, from image.base) and
        // exits, for reading the game's constants and tables offline. Game data: keep it private.
        if (const char* dumpPath = getenv("DR_IMAGE_DUMP"))
        {
            if (FILE* out = fopen(dumpPath, "wb"))
            {
                fwrite(g_memory.Translate(image.base), 1, image.size, out);
                fclose(out);
                LOGFN("Image written to {}", dumpPath);
            }
            std::_Exit(0);
        }

        // XEX header copy + loader entry, for RtlImageXexHeaderField and XexExecutableModuleHandle.
        auto* header = reinterpret_cast<const Xex2Header*>(file.data());
        uint32_t headerSize = header->headerSize;

        dr::ModuleInfo module{};
        module.xexHeader = GuestAlloc(headerSize);
        memcpy(g_memory.Translate(module.xexHeader), file.data(), headerSize);

        module.ldrDataEntry = GuestAlloc(0x100);
        Guest<be<uint32_t>>(module.ldrDataEntry + 0x18) = uint32_t(image.base);        // DllBase
        Guest<be<uint32_t>>(module.ldrDataEntry + 0x1C) = uint32_t(image.base);        // ImageBase
        Guest<be<uint32_t>>(module.ldrDataEntry + 0x20) = image.size;                  // SizeOfImage
        Guest<be<uint32_t>>(module.ldrDataEntry + 0x3C) = uint32_t(image.entry_point); // EntryPoint
        Guest<be<uint32_t>>(module.ldrDataEntry + 0x58) = module.xexHeader;            // XexHeaderBase

        module.fakeXamHandle = GuestAlloc(0x100);
        module.fakeKernelHandle = GuestAlloc(0x100);
        dr::SetModuleInfo(module);

        // Imports: type-0 records are address slots, type-1 records are code thunks.
        auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (imports)
        {
            std::vector<std::string_view> names;
            auto* strings = reinterpret_cast<const char*>(imports + 1);
            size_t offset = 0;
            for (size_t i = 0; i < imports->numImports; i++)
            {
                names.emplace_back(strings + offset);
                offset += ((names.back().size() + 1) + 3) & ~3;
            }

            auto* library = (const Xex2ImportLibrary*)((const char*)imports + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
            size_t variables = 0;
            size_t functions = 0;

            // XenonUtils already replaced every code thunk with nops and recorded it as a symbol
            // named "__imp__<Export>". Those slots must be left alone; the rest are address slots.
            std::unordered_map<std::string, uint32_t> thunkByName;
            std::unordered_set<uint32_t> thunkSlots;
            for (auto& symbol : image.symbols)
            {
                if (symbol.name.starts_with("__imp__"))
                {
                    thunkByName[symbol.name.substr(7)] = uint32_t(symbol.address);
                    thunkSlots.insert(uint32_t(symbol.address));
                }
            }

            for (size_t lib = 0; lib < names.size(); lib++)
            {
                auto* descriptors = (const Xex2ImportDescriptor*)(library + 1);

                for (size_t i = 0; i < library->numberOfImports; i++)
                {
                    uint32_t slot = descriptors[i].firstThunk;
                    if (thunkSlots.contains(slot))
                        continue; // code thunk, replaced by the recompiled __imp__ function

                    uint32_t record = *(uint32_t*)g_memory.Translate(slot); // host order (XenonUtils swapped it)
                    uint32_t ordinal = record & 0xFFFF;
                    const ExportInfo* info = FindExport(names[lib], ordinal);
                    uint32_t value = 0;

                    if (info && info->IsVariable())
                    {
                        value = CreateVariable(info->name, module);
                        variables++;
                        LOGFN("Import variable {}!{} -> 0x{:08X}", names[lib], info->name, value);
                    }
                    else if (auto it = info ? thunkByName.find(info->name) : thunkByName.end(); it != thunkByName.end())
                    {
                        value = it->second; // address slot of a function: point it at the thunk
                        functions++;
                    }
                    else
                    {
                        LOGFN_WARNING("Unresolved import {} ordinal 0x{:X} ({})", names[lib], ordinal, info ? info->name : "?");
                    }

                    Guest<be<uint32_t>>(slot) = value;
                }

                library = (const Xex2ImportLibrary*)((const char*)(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
            }

            LOGFN("Resolved {} import variables, {} function address slots, {} code thunks", variables, functions, thunkSlots.size());
        }

        return uint32_t(image.entry_point);
    }

    // Indirect call to an address that was not recompiled.
    PPC_FUNC(MissingFunction)
    {
        LOGFN_ERROR("Call to an address with no recompiled function: ctr=0x{:08X} lr=0x{:08X}", uint32_t(ctx.ctr.u32), uint32_t(ctx.lr));
        std::_Exit(3);
    }

    void FillMissingFunctions()
    {
        size_t filled = 0;
        for (uint64_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
        {
            auto& slot = PPC_LOOKUP_FUNC(g_memory.base, address);
            if (slot == nullptr)
            {
                slot = MissingFunction;
                filled++;
            }
        }
        LOGFN("Function table: {} recompiled functions, {} empty slots trapped", PPC_CODE_SIZE / 4 - filled, filled);
    }

#ifdef _WIN32
    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
    {
        auto* ctx = GetPPCContext();
        uint64_t faultAddress = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
        uint64_t guestFault = (faultAddress >= (uint64_t)g_memory.base && faultAddress < (uint64_t)g_memory.base + PPC_MEMORY_SIZE)
            ? faultAddress - (uint64_t)g_memory.base : faultAddress;

        uint64_t exeBase = (uint64_t)GetModuleHandleW(nullptr);
        LOGFN_ERROR("CRASH: exception 0x{:08X} at host 0x{:X} (DRRecomp.exe+0x{:X}), fault address 0x{:X} (guest 0x{:X})",
            uint32_t(info->ExceptionRecord->ExceptionCode), (uint64_t)info->ExceptionRecord->ExceptionAddress,
            (uint64_t)info->ExceptionRecord->ExceptionAddress - exeBase, faultAddress, guestFault);

        if (ctx)
        {
            LOGFN_ERROR("       guest lr=0x{:08X} r1=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r31=0x{:08X}",
                uint32_t(ctx->lr), ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r31.u32);
            // (The context's registers are not in number order.)
            const PPCRegister* gpr[32] = { &ctx->r0, &ctx->r1, &ctx->r2, &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7,
                &ctx->r8, &ctx->r9, &ctx->r10, &ctx->r11, &ctx->r12, &ctx->r13, &ctx->r14, &ctx->r15, &ctx->r16, &ctx->r17,
                &ctx->r18, &ctx->r19, &ctx->r20, &ctx->r21, &ctx->r22, &ctx->r23, &ctx->r24, &ctx->r25, &ctx->r26, &ctx->r27,
                &ctx->r28, &ctx->r29, &ctx->r30, &ctx->r31 };
            std::string regs;
            for (int i = 0; i < 32; i++)
                regs += fmt::format(" r{}={:08X}", i, gpr[i]->u32);
            LOGFN_ERROR("       guest{}", regs);
        }
        // The call chain: recompiled functions are named after their guest address.
        CpuSampler::LogStack(info->ContextRecord);
        DumpPlayerTrace(); // gpu/d3d_hooks.cpp

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Reads through a null guest pointer (guest page 0, kept inaccessible to catch them) read zero
    // instead of crashing, and are logged: the page is made read-only for the one faulting
    // instruction (trap flag), then inaccessible again. Writes still crash. Frank's actions read his
    // held item while another game thread could clear it (2026-11-25: unit updates run as parallel
    // jobs); a null read there is a zero id, not a crash. DR_NULL_READS=crash turns this off.
    static std::atomic<uint32_t> g_nullSteppers{ 0 };
    static thread_local bool t_nullStep = false;

    LONG WINAPI NullReadHandler(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP && t_nullStep)
        {
            t_nullStep = false;
            DWORD old;
            if (--g_nullSteppers == 0)
                VirtualProtect(g_memory.base, 4096, PAGE_NOACCESS, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2 || info->ExceptionRecord->ExceptionInformation[0] != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        const uint64_t fault = info->ExceptionRecord->ExceptionInformation[1];
        if (fault < (uint64_t)g_memory.base || fault >= (uint64_t)g_memory.base + 4096 || t_nullStep)
            return EXCEPTION_CONTINUE_SEARCH;

        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 20)
        {
            auto* ctx = GetPPCContext();
            LOGFN_WARNING("[null] read of guest 0x{:X} at DRRecomp.exe+0x{:X} (guest lr 0x{:08X}); read as zero",
                fault - (uint64_t)g_memory.base, (uint64_t)info->ExceptionRecord->ExceptionAddress - (uint64_t)GetModuleHandleW(nullptr),
                ctx ? uint32_t(ctx->lr) : 0);
            CpuSampler::LogStack(info->ContextRecord);
            DumpPlayerTrace();
        }
        t_nullStep = true;
        DWORD old;
        if (g_nullSteppers++ == 0)
            VirtualProtect(g_memory.base, 4096, PAGE_READONLY, &old);
        info->ContextRecord->EFlags |= 0x100; // trap after this one instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
}

static int DRMain(int argc, char* argv[]);

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <SDL_main.h>
// iOS: SDL sets up the UIApplication, then calls DRMain on the main thread.
int main(int argc, char* argv[])
{
    return SDL_UIKitRunApp(argc, argv, DRMain);
}
#else
int main(int argc, char* argv[])
{
    return DRMain(argc, argv);
}
#endif

static int DRMain(int argc, char* argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(CrashHandler);
    if (const char* nullReads = getenv("DR_NULL_READS"); nullReads == nullptr || strcmp(nullReads, "crash") != 0)
        AddVectoredExceptionHandler(1, NullReadHandler);
    if (getenv("DR_NULL_TEST") != nullptr && g_memory.base != nullptr)
    {
        // Test of NullReadHandler: two reads of guest 0xC, then exit.
        volatile uint32_t* address = reinterpret_cast<volatile uint32_t*>(g_memory.base + 0xC);
        const uint32_t first = *address;
        const uint32_t second = *address;
        LOGFN("[null] test reads: {} {}", first, second);
        std::_Exit(0);
    }
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS: the game files, the log and the saves live in the app's Documents folder (Files app:
    // On My iPhone > Dead Rising).
    const std::filesystem::path documents = std::filesystem::path(getenv("HOME")) / "Documents";
    std::filesystem::current_path(documents);
#endif
#ifdef __APPLE__
    // MoltenVK compiles with Metal fast math by default, which may fuse the vertex shaders'
    // position math differently per shader; the game's multi-pass skinned meshes need bit-equal
    // depth (the shaders mark it precise / NoContraction). Before Vulkan loads MoltenVK.
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
#endif

    os::logger::Init();
    LOGN("Dead Rising recomp runtime");

    // Game folder: first argument, otherwise the folder that holds default.xex next to us.
    std::filesystem::path gamePath = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (argc <= 1 && std::filesystem::exists(documents / "game" / "default.xex"))
        gamePath = documents / "game";
#endif
    if (!std::filesystem::exists(gamePath / "default.xex"))
    {
        LOGFN_ERROR("default.xex not found in {}. Pass the game folder as the first argument.", (const char*)gamePath.u8string().c_str());
        return 1;
    }

    g_gamePath = gamePath;
    Config::Load(); // recomp_config.toml in the game folder (user/config.h)

    if (g_memory.base == nullptr)
    {
        LOGN_ERROR("Could not reserve 4 GB of guest memory.");
        return 1;
    }

    g_userHeap.Init();
    Xma::Init(); // the APU's context array register, read by the game's audio start-up
    FillMissingFunctions();

#ifdef DR_CALL_PROFILER
    CallProfiler::Start();
#endif

    FileSystemDR::Init(gamePath, GetSavePath(false));
    const std::string gameRoot = (const char*)gamePath.u8string().c_str();
    XamRootCreate("game", gameRoot);
    XamRootCreate("d", gameRoot);

    uint32_t entry = LoadExecutable(gamePath / "default.xex");
    if (entry == 0)
        return 1;

    if (g_timeStampBundle)
        std::thread(TimeStampThread).detach();

#ifdef __APPLE__
    // UIKit needs SDL on the main thread: the video loop keeps it, the game gets its own thread.
    static uint32_t s_entry;
    s_entry = entry;
    LOGFN("Starting guest at 0x{:08X} (own thread)", entry);
    Video::RunOnMainThread([] { GuestThread::Start({ s_entry, 0, 0 }, nullptr); });
    LOGN_ERROR("No window or GPU device.");
    return 1;
#else
    if (!Video::Init())
        LOGN_WARNING("No window or GPU device: running without video output.");

    LOGFN("Starting guest at 0x{:08X}", entry);
    GuestThread::Start({ entry, 0, 0 });
#endif

    LOGN("Guest entry point returned.");
    return 0;
}
