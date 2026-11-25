// Host window and presentation. Rendering of the game's draws comes later (see docs/d3d_map.md):
// for now every guest frame produces one host frame and the window shows live D3D call counts.

#include <stdafx.h>
#include "video.h"
#include "shaders.h"
#include "renderer.h"
#include <hid/input.h>
#include <ui/options_menu.h>
#include <user/config.h>
#include <os/logger.h>
#include <profiler/cpu_sampler.h>

#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <SDL_metal.h>
#include <plume_apple.h>
#endif
#include <plume_render_interface.h>

namespace plume
{
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#ifdef _WIN32
    extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
    // plume_vulkan.cpp (Dead Rising): the Vulkan pipeline cache on disk.
    void SetVulkanPipelineCachePath(const std::string& path);
    bool SaveVulkanPipelineCache(RenderDevice* device);
}

using namespace plume;

namespace
{
    constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
    constexpr uint32_t kSwapChainBuffers = 3; // triple buffering: with two frames in flight, two images let the GPU stall on vsync

    // Two frames in flight: the video thread records and submits frame N while the GPU still draws
    // N - 1, so the replay (and the guest, which waits for it) no longer waits for the GPU. Each
    // slot has its own command list, fence and acquire semaphore; a slot is reused once its last
    // frame has finished (WaitSlot). The renderer keeps its per-frame data by slot as well.
    // DR_FRAMES_IN_FLIGHT=1 waits for each frame after presenting it, as before.
    constexpr uint32_t kFrameSlots = 2;

    struct HostDevice
    {
        std::unique_ptr<RenderInterface> renderInterface;
        std::unique_ptr<RenderDevice> device;
        std::unique_ptr<RenderCommandQueue> queue;
        std::unique_ptr<RenderCommandList> commandLists[kFrameSlots];
        std::unique_ptr<RenderCommandFence> fences[kFrameSlots];
        std::unique_ptr<RenderCommandSemaphore> acquireSemaphores[kFrameSlots];
        bool submitted[kFrameSlots] = {};
        uint32_t frame = 0; // frames submitted; the slot is frame % kFrameSlots
        std::unique_ptr<RenderSwapChain> swapChain;
        std::vector<std::unique_ptr<RenderCommandSemaphore>> releaseSemaphores;
        std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
        const char* apiName = "";
    };

    HostDevice g_host;
    SDL_Window* g_window = nullptr;

    std::mutex g_frameMutex;
    std::condition_variable g_frameReady;
    Video::FrameStats g_pendingStats;
    bool g_framePending = false;

    // Where the video thread's time goes. The game waits for this thread to take each frame, and
    // its "[hitch] ... waiting for the video thread" lines say only how long; a loop over 50 ms
    // (not counting the wait for a frame) logs this breakdown (2026-11-25: stalls of 200-340 ms,
    // in pairs, with no slow replay).
    enum VideoStage { StageEvents, StageInput, StageResize, StageGpuWait, StageAcquire, StageReplay, StageSubmit, StagePresent, StageTitle, StageCount };
    constexpr const char* kVideoStageNames[StageCount] = { "window and device events", "input, rumble and options menu", "swap chain resize",
        "waiting for the GPU", "acquiring the image", "replay", "submit", "present", "title and caches" };
    uint32_t g_stageUs[StageCount];
    std::chrono::steady_clock::time_point g_stageMark;
    uint32_t g_stageEvents, g_stageDeviceEvents, g_stageWindowEvents;

    void MarkStage(VideoStage stage)
    {
        const auto now = std::chrono::steady_clock::now();
        g_stageUs[stage] += uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - g_stageMark).count());
        g_stageMark = now;
    }

    std::atomic<bool> g_initDone{ false };
    std::atomic<bool> g_initOk{ false };
    void (*g_startGame)() = nullptr; // RunOnMainThread: starts the game once the window exists
    bool g_rendererReady = false;

    void CreateFramebuffers()
    {
        g_host.framebuffers.clear();
        for (uint32_t i = 0; i < g_host.swapChain->getTextureCount(); i++)
        {
            const RenderTexture* color = g_host.swapChain->getTexture(i);
            RenderFramebufferDesc desc;
            desc.colorAttachments = &color;
            desc.colorAttachmentsCount = 1;
            desc.depthAttachment = nullptr;
            g_host.framebuffers.push_back(g_host.device->createFramebuffer(desc));
        }
    }

    bool CreateHostDevice()
    {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(g_window, &wmInfo);

#ifdef _WIN32
        RenderWindow renderWindow = wmInfo.info.win.window;

        // D3D12 first, Vulkan as fallback (UnleashedRecomp does the same on Windows).
        // DR_GPU=vulkan forces Vulkan (to test the SPIR-V path).
        const char* gpu = getenv("DR_GPU");
        const bool forceVulkan = gpu != nullptr && (strcmp(gpu, "vulkan") == 0 || strcmp(gpu, "Vulkan") == 0);
        if (!forceVulkan)
        {
            g_host.renderInterface = CreateD3D12Interface();
            g_host.apiName = "D3D12";
        }
        if (g_host.renderInterface == nullptr)
        {
            g_host.renderInterface = CreateVulkanInterface();
            g_host.apiName = "Vulkan";
        }
#elif defined(__APPLE__)
        // Vulkan through MoltenVK, on a CAMetalLayer in the SDL window.
        RenderWindow renderWindow{};
#if TARGET_OS_IPHONE
        renderWindow.window = wmInfo.info.uikit.window;
        for (int retry = 0; retry < 120 && renderWindow.window == nullptr; retry++)
        {
            // UIKit can report no window right after it is shown.
            SDL_PumpEvents();
            SDL_Delay(8);
            if (SDL_GetWindowWMInfo(g_window, &wmInfo))
                renderWindow.window = wmInfo.info.uikit.window;
        }
        renderWindow.view = plume::ensureMetalLayerForIOSWindow(renderWindow.window);
#else
        renderWindow.window = wmInfo.info.cocoa.window;
        renderWindow.view = SDL_Metal_GetLayer(SDL_Metal_CreateView(g_window));
#endif
        g_host.renderInterface = CreateVulkanInterface();
        g_host.apiName = "Vulkan (MoltenVK)";
#else
        RenderWindow renderWindow{};
        g_host.renderInterface = CreateVulkanInterface();
        g_host.apiName = "Vulkan";
#endif
        if (g_host.renderInterface == nullptr)
            return false;

        // Vulkan (and MoltenVK on iOS) makes every pipeline from scratch each run without a cache:
        // _recomp_cache/vulkan_pipelines.bin in the game folder keeps them (SaveVulkanPipelines).
        if (strncmp(g_host.apiName, "Vulkan", 6) == 0)
        {
            std::error_code error;
            const std::filesystem::path folder = Config::Path().parent_path() / "_recomp_cache";
            std::filesystem::create_directories(folder, error);
            SetVulkanPipelineCachePath((folder / "vulkan_pipelines.bin").string());
        }

        g_host.device = g_host.renderInterface->createDevice();
        if (g_host.device == nullptr)
            return false;

        g_host.queue = g_host.device->createCommandQueue(RenderCommandListType::DIRECT);
        g_host.swapChain = g_host.queue->createSwapChain(renderWindow, kSwapChainBuffers, kSwapChainFormat, 2);
        g_host.swapChain->resize();
        for (uint32_t i = 0; i < kFrameSlots; i++)
        {
            g_host.fences[i] = g_host.device->createCommandFence();
            g_host.commandLists[i] = g_host.queue->createCommandList();
            g_host.acquireSemaphores[i] = g_host.device->createCommandSemaphore();
        }
        CreateFramebuffers();
        return true;
    }

    // Waits for the last frame submitted in this slot to finish on the GPU (the time goes into the
    // renderer's log as waiting for the GPU).
    void WaitSlot(uint32_t slot)
    {
        if (!g_host.submitted[slot])
            return;
        const auto start = std::chrono::steady_clock::now();
        g_host.queue->waitForCommandFence(g_host.fences[slot].get());
        g_host.submitted[slot] = false;
        Renderer::AddGpuWait(uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()));
    }

    void WaitAll()
    {
        for (uint32_t i = 0; i < kFrameSlots; i++)
            WaitSlot(i);
    }

    // The renderer calls this before it points a texture descriptor the other frame may read at
    // another texture (renderer.h).
    void WaitOtherSlot()
    {
        WaitSlot((g_host.frame + 1) % kFrameSlots);
    }

    bool SerialFrames()
    {
        static const bool serial = [] {
            const char* value = getenv("DR_FRAMES_IN_FLIGHT");
            return value != nullptr && atoi(value) == 1;
        }();
        return serial;
    }

    void Submitted(uint32_t slot)
    {
        g_host.submitted[slot] = true;
        g_host.frame++;
        if (SerialFrames())
            WaitSlot(slot);
    }

    // Minimized (no swap chain image): the frame is still replayed, without the final picture.
    // Skipping it left the game waiting a second for the video thread on every frame (1 fps until
    // the window came back), and its texture uploads and resolves would have been lost.
    void RenderOffscreen()
    {
        if (!g_rendererReady)
            return;
        const uint32_t slot = g_host.frame % kFrameSlots;
        WaitSlot(slot);
        auto* list = g_host.commandLists[slot].get();
        list->begin();
        Renderer::Render(list, nullptr, 0, 0, slot, WaitOtherSlot);
        list->end();
        const RenderCommandList* lists[] = { list };
        g_host.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, g_host.fences[slot].get());
        Submitted(slot);
        static uint32_t logged;
        if (logged++ % 300 == 0)
            LOGN("[video] window minimized: frames replayed without presenting");
    }

    void RenderFrame(const Video::FrameStats& stats)
    {
        if (g_host.swapChain->needsResize())
        {
            WaitAll(); // the frames in flight use the old swap chain images
            g_host.swapChain->resize();
            CreateFramebuffers();
        }
        MarkStage(StageResize);

        if (g_host.swapChain->isEmpty())
        {
            RenderOffscreen();
            MarkStage(StageReplay);
            return;
        }

        // The slot's last frame must be done: its command list, acquire semaphore and the renderer's
        // data for it are reused.
        const uint32_t slot = g_host.frame % kFrameSlots;
        WaitSlot(slot);
        MarkStage(StageGpuWait);

        uint32_t imageIndex = 0;
        const bool acquired = g_host.swapChain->acquireTexture(g_host.acquireSemaphores[slot].get(), &imageIndex);
        MarkStage(StageAcquire);
        if (!acquired)
        {
            RenderOffscreen();
            MarkStage(StageReplay);
            return;
        }

        auto* list = g_host.commandLists[slot].get();
        list->begin();

        RenderTexture* backBuffer = g_host.swapChain->getTexture(imageIndex);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE));
        const uint32_t width = g_host.swapChain->getWidth();
        const uint32_t height = g_host.swapChain->getHeight();

        // The game's frame, replayed on the host (gpu/renderer.cpp).
        if (!g_rendererReady || !Renderer::Render(list, g_host.framebuffers[imageIndex].get(), width, height, slot, WaitOtherSlot))
        {
            // Nothing to show yet: the shade follows the number of draws, so a running game is
            // visibly different from a stalled one.
            list->setFramebuffer(g_host.framebuffers[imageIndex].get());
            list->setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
            list->setScissors(RenderRect(0, 0, width, height));
            float level = std::min(1.0f, float(stats.draws) / 2000.0f);
            list->clearColor(0, RenderColor(0.05f, 0.05f + 0.25f * level, 0.08f, 1.0f));
        }

        list->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(backBuffer, RenderTextureLayout::PRESENT));
        list->end();

        while (g_host.releaseSemaphores.size() < g_host.swapChain->getTextureCount())
            g_host.releaseSemaphores.emplace_back(g_host.device->createCommandSemaphore());

        MarkStage(StageReplay);

        const RenderCommandList* lists[] = { list };
        RenderCommandSemaphore* wait = g_host.acquireSemaphores[slot].get();
        RenderCommandSemaphore* signal = g_host.releaseSemaphores[imageIndex].get();
        g_host.queue->executeCommandLists(lists, 1, &wait, 1, &signal, 1, g_host.fences[slot].get());
        MarkStage(StageSubmit);
        g_host.swapChain->present(imageIndex, &signal, 1);
        Submitted(slot);
        MarkStage(StagePresent);
    }

    // Vulkan: the pipeline cache goes to disk when pipelines were added (at most every 30 s) and
    // when the window closes.
    void SaveVulkanPipelines(bool now)
    {
        if (g_host.device == nullptr || strncmp(g_host.apiName, "Vulkan", 6) != 0)
            return;
        static uint32_t saved = 0;
        static auto lastSave = std::chrono::steady_clock::now();
        const uint32_t count = Renderer::PipelineCount();
        if (count == saved || (!now && std::chrono::steady_clock::now() - lastSave < std::chrono::seconds(30)))
            return;
        const auto start = std::chrono::steady_clock::now();
        if (SaveVulkanPipelineCache(g_host.device.get()))
            LOGFN("[video] Vulkan pipeline cache saved ({} pipelines, {} ms)", count,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        saved = count;
        lastSave = std::chrono::steady_clock::now();
    }

    void UpdateTitle(const Video::FrameStats& stats, double fps)
    {
        char title[256];
        snprintf(title, sizeof(title),
            "Dead Rising (recompiled) [%s] - frame %u, %.0f fps | draws %u, vertex batches %u, clears %u, resolves %u, tiles %u, textures %u",
            g_host.apiName, stats.frame, fps, stats.draws, stats.vertexBatches, stats.clears, stats.resolves, stats.tiles, stats.textureBinds);
        SDL_SetWindowTitle(g_window, title);

        static uint32_t updates;
        if (updates++ % 10 == 0) // every ~5 s
            LOGFN("[video] {}", title);
    }

    void VideoThread()
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        {
            LOGFN_ERROR("[video] SDL_Init failed: {}", SDL_GetError());
            g_initDone = true;
            return;
        }

#if defined(__APPLE__) && TARGET_OS_IPHONE
        // iPhone / iPad: full screen, landscape.
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        g_window = SDL_CreateWindow("Dead Rising", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
#else
        g_window = SDL_CreateWindow("Dead Rising (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#endif
        if (g_window == nullptr || !CreateHostDevice())
        {
            LOGFN_ERROR("[video] could not create the window or GPU device: {}", SDL_GetError());
            g_initDone = true;
            return;
        }

        LOGFN("[video] window and {} device ready", g_host.apiName);
        Input::Init();
        if (!Shaders::Init(g_host.device.get(), strncmp(g_host.apiName, "Vulkan", 6) == 0))
            LOGN_ERROR("[video] shader cache unavailable - shaders will not be translated");
        else
            g_rendererReady = Renderer::Init(g_host.device.get(), strncmp(g_host.apiName, "Vulkan", 6) == 0, kSwapChainFormat);
        g_initOk = true;
        g_initDone = true;
        if (g_startGame != nullptr)
            g_startGame();

        auto lastTitle = std::chrono::steady_clock::now();
        uint32_t framesSinceTitle = 0;
        CpuSampler::RegisterCurrentThread("video thread"); // DR_CPU_PROFILE=1

        g_stageMark = std::chrono::steady_clock::now();
        while (true)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                g_stageEvents++;
                if (event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED || event.type == SDL_CONTROLLERDEVICEADDED ||
                    event.type == SDL_CONTROLLERDEVICEREMOVED || event.type == SDL_CONTROLLERDEVICEREMAPPED)
                    g_stageDeviceEvents++;
                if (event.type == SDL_WINDOWEVENT)
                {
                    g_stageWindowEvents++;
                    const char* what = event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ? "lost focus"
                        : event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? "got focus"
                        : event.window.event == SDL_WINDOWEVENT_MINIMIZED ? "minimized"
                        : event.window.event == SDL_WINDOWEVENT_RESTORED ? "restored" : nullptr;
                    static uint32_t logged;
                    if (what != nullptr && logged++ < 100)
                        LOGFN("[video] window {}", what);
                }
                if (OptionsMenu::HandleEvent(event))
                    continue;
                Input::HandleEvent(event);
                if (event.type == SDL_QUIT)
                {
                    LOGN("[video] window closed - exiting");
                    SaveVulkanPipelines(true);
                    std::_Exit(0);
                }
            }

            MarkStage(StageEvents);

            static uint32_t videoFrames;
            Input::Update(videoFrames);
            OptionsMenu::Update();

            // Settings the window and swap chain carry (user/config.h).
            static bool fullscreen = false;
            if (Config::Fullscreen != fullscreen)
            {
                fullscreen = Config::Fullscreen;
                SDL_SetWindowFullscreen(g_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (g_host.swapChain != nullptr && g_host.swapChain->isVsyncEnabled() != Config::VSync)
                g_host.swapChain->setVsyncEnabled(Config::VSync);
            MarkStage(StageInput);

            Video::FrameStats stats;
            {
                std::unique_lock lock(g_frameMutex);
                g_frameReady.wait_for(lock, std::chrono::milliseconds(50), [] { return g_framePending; });
                g_stageMark = std::chrono::steady_clock::now(); // waiting for the game is not this thread's time
                if (!g_framePending)
                    continue; // keep the window responsive while the game loads
                stats = g_pendingStats;
                g_framePending = false;
            }

            RenderFrame(stats);
            videoFrames++;
            framesSinceTitle++;

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastTitle).count();
            if (elapsed >= 0.5)
            {
                UpdateTitle(stats, framesSinceTitle / elapsed);
                OptionsMenu::SetFps(framesSinceTitle / elapsed);
                Shaders::Update();
                SaveVulkanPipelines(false);
                lastTitle = now;
                framesSinceTitle = 0;
            }
            MarkStage(StageTitle);

            uint32_t totalUs = 0;
            for (uint32_t us : g_stageUs)
                totalUs += us;
            if (totalUs > 50000)
            {
                static uint32_t logged;
                if (logged++ < 200)
                {
                    std::string stages;
                    for (uint32_t i = 0; i < StageCount; i++)
                        if (g_stageUs[i] >= 1000)
                            stages += fmt::format("{}{} {} ms", stages.empty() ? "" : ", ", kVideoStageNames[i], g_stageUs[i] / 1000);
                    LOGFN("[hitch] video thread, frame {}: {} ms busy ({}); {} SDL events, {} controller connects, {} window events",
                        videoFrames, totalUs / 1000, stages, g_stageEvents, g_stageDeviceEvents, g_stageWindowEvents);
                }
            }
            for (uint32_t& us : g_stageUs)
                us = 0;
            g_stageEvents = g_stageDeviceEvents = g_stageWindowEvents = 0;
        }
    }
}

void Video::RunOnMainThread(void (*startGame)())
{
    g_startGame = startGame;
    VideoThread();
}

bool Video::Init()
{
    std::thread(VideoThread).detach();
    while (!g_initDone)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return g_initOk;
}

void Video::SubmitFrame(const FrameStats& stats)
{
    {
        std::lock_guard lock(g_frameMutex);
        g_pendingStats = stats;
        g_framePending = true;
    }
    g_frameReady.notify_one();
}
