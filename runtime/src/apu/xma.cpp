#include <stdafx.h>
#include "xma.h"

#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <gpu/renderer.h>
#include <gpu/command_processor.h>

#ifdef DR_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#endif

// The context layout and the hardware's behaviour (buffer swap, loops, output ring buffer of
// 256-byte blocks, big-endian 16-bit samples) follow Xenia's XmaContext (BSD licence,
// src/xenia/apu/xma_context.cc). Decoding differs: Xenia feeds single frames to a patched FFmpeg;
// here whole 2 KB packets go to upstream FFmpeg's XMA2 decoder, as vgmstream does.
namespace
{
    constexpr uint32_t kBytesPerPacket = 2048;
    constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
    constexpr uint32_t kBytesPerBlock = 256; // output ring buffer granularity

    // The APU's context array (Xenia allocates the same 320) and register numbers.
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kRegisterContextArray = 0x600; // physical address of context 0
    constexpr uint32_t kRegisterKick = 0x650;          // 10 registers, a bit per context
    constexpr uint32_t kRegisterLock = 0x690;
    constexpr uint32_t kRegisterClear = 0x6A0;
    constexpr uint32_t kRegisterBanks = kContextCount / 32;

    // The 64-byte context as 16 host-order dwords; fields by bit position (Xenia's XMA_CONTEXT_DATA).
    struct Context
    {
        uint32_t d[16];

        void Load(const void* guest)
        {
            const auto* src = static_cast<const uint32_t*>(guest);
            for (int i = 0; i < 16; i++)
                d[i] = __builtin_bswap32(src[i]);
        }
        // Writes back only the bits this pass changed, each dword atomically: the game updates
        // other fields of a running context (input buffers, read offsets, new buffer pointers),
        // and writing the whole context back would undo those updates.
        void Store(void* guest) const // DR_XMA_LEGACY
        {
            auto* dst = static_cast<uint32_t*>(guest);
            for (int i = 0; i < 16; i++)
                dst[i] = __builtin_bswap32(d[i]);
        }

        void StoreChanges(void* guest, const Context& before) const
        {
            auto* words = static_cast<uint32_t*>(guest);
            for (int i = 0; i < 16; i++)
            {
                const uint32_t changed = d[i] ^ before.d[i];
                if (changed == 0)
                    continue;
                std::atomic_ref<uint32_t> word(words[i]);
                uint32_t current = word.load();
                uint32_t desired;
                do
                    desired = __builtin_bswap32((__builtin_bswap32(current) & ~changed) | (d[i] & changed));
                while (!word.compare_exchange_weak(current, desired));
            }
        }

        uint32_t Get(int dword, int bit, int bits) const { return (d[dword] >> bit) & ((1u << bits) - 1); }
        void Set(int dword, int bit, int bits, uint32_t value)
        {
            const uint32_t mask = ((1u << bits) - 1) << bit;
            d[dword] = (d[dword] & ~mask) | ((value << bit) & mask);
        }

        uint32_t PacketCount(int buffer) const { return Get(buffer, 0, 12); }
        uint32_t LoopCount() const { return Get(0, 12, 8); }
        void SetLoopCount(uint32_t v) { Set(0, 12, 8, v); }
        bool InputValid(int buffer) const { return Get(0, 20 + buffer, 1) != 0; }
        void SetInputValid(int buffer, bool v) { Set(0, 20 + buffer, 1, v); }
        uint32_t OutputBlocks() const { return Get(0, 22, 5); }
        uint32_t OutputWrite() const { return Get(0, 27, 5); }
        void SetOutputWrite(uint32_t v) { Set(0, 27, 5, v); }
        uint32_t SampleRateId() const { return Get(1, 27, 2); }
        bool Stereo() const { return Get(1, 29, 1) != 0; }
        bool OutputValid() const { return Get(1, 31, 1) != 0; }
        void SetOutputValid(bool v) { Set(1, 31, 1, v); }
        uint32_t ReadOffset() const { return Get(2, 0, 26); }
        void SetReadOffset(uint32_t v) { Set(2, 0, 26, v); }
        uint32_t LoopStart() const { return Get(3, 0, 26); }
        uint32_t LoopEnd() const { return Get(4, 0, 26); }
        uint32_t CurrentBuffer() const { return Get(4, 31, 1); }
        void SetCurrentBuffer(uint32_t v) { Set(4, 31, 1, v); }
        uint32_t InputPointer(int buffer) const { return d[5 + buffer]; }
        uint32_t OutputPointer() const { return d[7]; }
        uint32_t OutputRead() const { return Get(9, 0, 5); }
    };

    // XMA packet header: frame count (6 bits), first frame offset in bits after the 32-bit header
    // (15 bits), metadata (3), packets to skip to this stream's next packet (8).
    uint32_t PacketFrameOffset(const uint8_t* packet)
    {
        return ((((packet[0] & 3u) << 13) | (uint32_t(packet[1]) << 5) | (packet[2] >> 3))) + 32;
    }
    uint32_t PacketSkip(const uint8_t* packet) { return packet[3]; }

    uint32_t SampleRate(uint32_t id)
    {
        static constexpr uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
        return kRates[id & 3];
    }

    struct HostContext
    {
        std::mutex mutex;                 // held while the worker services it; lock/clear/release wait on it
        std::atomic<bool> allocated{ false };
        std::atomic<bool> enabled{ false }; // kicked and not locked since
        std::atomic<bool> kickPending{ false }; // kicked, no pass since
        bool swapped = false; // the decoder moved on to the other input buffer itself (same stream)
        uint32_t guest = 0;
        std::vector<uint8_t> pending; // decoded big-endian samples not written to the ring yet
        uint32_t streamInput = 0, streamChannels = 0, streamRate = 0;
        // Debugging (DR_XMA_STREAMS): what the current stream decoded.
        uint32_t streamPackets = 0, streamErrors = 0, streamSamples = 0; float streamPeak = 0.0f;
#ifdef DR_HAVE_FFMPEG
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
#endif
    };

    // Logged every minute: register traffic, and the cases servicing every context with a valid
    // output (as this runtime did before) got wrong: stopped contexts still holding work, and
    // contexts the game pointed at new buffers while a pass was decoding them.
    struct Stats
    {
        std::atomic<uint32_t> kicks{ 0 }, locks{ 0 }, clears{ 0 }, stoppedWithWork{ 0 }, reprogrammed{ 0 }, lockedUnserviced{ 0 };
    } g_stats;

    std::mutex g_allocationMutex;
    std::array<HostContext, kContextCount> g_contexts;
    uint32_t g_arrayGuest = 0; // guest address of context 0
    std::atomic<bool> g_running{ false };

    // Debugging: DR_XMA_LEGACY=1 services every context with a valid output whether or not the game
    // kicked it, and writes whole contexts back (how this runtime worked before the registers were
    // modelled), to compare the two.
    const bool g_legacy = getenv("DR_XMA_LEGACY") != nullptr;

    // A physical buffer of 'size' bytes the context points at (0 means not programmed yet).
    bool PhysicalRange(uint32_t address, uint32_t size)
    {
        return address != 0 && uint64_t(address & 0x1FFFFFFFu) + size <= 0x20000000ull;
    }

#ifdef DR_HAVE_FFMPEG
    void CloseDecoder(HostContext& host)
    {
        if (host.codec != nullptr)
            avcodec_free_context(&host.codec);
        if (host.packet != nullptr)
            av_packet_free(&host.packet);
        if (host.frame != nullptr)
            av_frame_free(&host.frame);
    }

    bool OpenDecoder(HostContext& host, uint32_t channels, uint32_t rate)
    {
        CloseDecoder(host);
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (codec == nullptr)
            return false;
        host.codec = avcodec_alloc_context3(codec);
        host.packet = av_packet_alloc();
        host.frame = av_frame_alloc();
        if (host.codec == nullptr || host.packet == nullptr || host.frame == nullptr)
        {
            CloseDecoder(host); // out of memory: the context stays silent and is tried again later
            return false;
        }
        host.codec->sample_rate = int(rate);
        av_channel_layout_default(&host.codec->ch_layout, int(channels));
        // XMA2WAVEFORMATEX-style extradata (34 bytes): one stream of 'channels' channels.
        host.codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        if (host.codec->extradata == nullptr)
        {
            CloseDecoder(host);
            return false;
        }
        host.codec->extradata_size = 34;
        host.codec->extradata[0] = 1; // NumStreams (little-endian 16-bit)
        const uint32_t channelMask = channels == 2 ? 3 : 4;
        memcpy(host.codec->extradata + 2, &channelMask, 4);
        host.codec->extradata[30] = 4; // EncoderVersion-ish byte some builds check (vgmstream sets 4)
        if (avcodec_open2(host.codec, codec, nullptr) < 0)
        {
            CloseDecoder(host);
            return false;
        }
        host.streamChannels = channels;
        host.streamRate = rate;
        return true;
    }

    // Decodes one 2 KB packet; appends big-endian 16-bit interleaved samples to host.pending.
    void DecodePacket(HostContext& host, const uint8_t* data)
    {
        host.packet->data = const_cast<uint8_t*>(data);
        host.packet->size = int(kBytesPerPacket);
        host.streamPackets++;
        if (avcodec_send_packet(host.codec, host.packet) < 0)
        {
            host.streamErrors++;
            return;
        }
        while (avcodec_receive_frame(host.codec, host.frame) == 0)
        {
            const uint32_t channels = uint32_t(host.frame->ch_layout.nb_channels);
            const uint32_t samples = uint32_t(host.frame->nb_samples);
            host.streamSamples += samples;
            const size_t base = host.pending.size();
            host.pending.resize(base + size_t(samples) * channels * 2);
            uint8_t* out = host.pending.data() + base;
            for (uint32_t i = 0; i < samples; i++)
                for (uint32_t c = 0; c < channels; c++)
                {
                    const float v = std::clamp(reinterpret_cast<const float*>(host.frame->data[c])[i], -1.0f, 1.0f);
                    host.streamPeak = std::max(host.streamPeak, std::abs(v));
                    const int16_t s = int16_t(v * 32767.0f);
                    *out++ = uint8_t(uint16_t(s) >> 8);
                    *out++ = uint8_t(s & 0xFF);
                }
        }
    }
#endif

    // One pass over a kicked context, as the hardware does: decode while the output ring has room
    // and input is valid.
    void Service(HostContext& host)
    {
        void* guest = g_memory.Translate(host.guest);
        Context before;
        before.Load(guest);
        Context ctx = before;
        if (!ctx.OutputValid())
            return;
        if (!ctx.InputValid(0) && !ctx.InputValid(1))
        {
            ctx.SetOutputValid(false);
            if (g_legacy)
                ctx.Store(guest);
            else
                ctx.StoreChanges(guest, before);
            return;
        }

        // Never write through a context that is not fully programmed: its ring would land on
        // whatever the game keeps at physical address 0 (or elsewhere it no longer owns).
        if (!g_legacy && !PhysicalRange(ctx.OutputPointer(), ctx.OutputBlocks() * kBytesPerBlock))
        {
            static std::atomic<uint32_t> logged{ 0 };
            if (logged++ < 8)
                LOGFN_WARNING("[xma] context {:08X}: output buffer {:08X} x{} blocks not usable, skipped",
                    host.guest, ctx.OutputPointer(), ctx.OutputBlocks());
            return;
        }

#ifdef DR_HAVE_FFMPEG
        const uint32_t channels = ctx.Stereo() ? 2 : 1;
        const uint32_t rate = SampleRate(ctx.SampleRateId());
        if (host.codec == nullptr || host.streamChannels != channels || host.streamRate != rate)
        {
            if (!OpenDecoder(host, channels, rate))
                return;
            host.pending.clear();
        }

        // A new stream (input buffer 0 reprogrammed and read from its start): fresh decoder state.
        const uint32_t input = ctx.InputPointer(ctx.CurrentBuffer());
        if (input != host.streamInput && ctx.ReadOffset() <= 64 && !host.swapped)
        {
            avcodec_flush_buffers(host.codec);
            host.pending.clear();
        }
        static const bool logStreams = getenv("DR_XMA_STREAMS") != nullptr;
        if (logStreams && input != host.streamInput)
        {
            if (host.streamInput != 0)
                LOGFN("[xma] stream {:08X} on {:08X}: {} packets, {} errors, {} samples ({:.2f} s at {} Hz), peak {:.3f}", host.streamInput, host.guest,
                    host.streamPackets, host.streamErrors, host.streamSamples, double(host.streamSamples) / std::max(1u, rate), rate, host.streamPeak);
            host.streamPackets = host.streamErrors = host.streamSamples = 0;
            host.streamPeak = 0.0f;
        }
        host.streamInput = input;
        host.swapped = false;

        uint8_t* ring = static_cast<uint8_t*>(g_memory.Translate(ctx.OutputPointer() | 0xA0000000u));
        const uint32_t capacity = ctx.OutputBlocks() * kBytesPerBlock;
        uint32_t write = ctx.OutputWrite() * kBytesPerBlock;
        const uint32_t read = ctx.OutputRead() * kBytesPerBlock;
        if (capacity == 0)
            return;
        uint32_t used = (write + capacity - read) % capacity;
        bool wrote = false;

        auto flushPending = [&]
        {
            while (host.pending.size() >= kBytesPerBlock && capacity - used >= kBytesPerBlock)
            {
                Renderer::CheckGuestWrite("xma output", ctx.OutputPointer() + write, kBytesPerBlock);
                memcpy(ring + write, host.pending.data(), kBytesPerBlock);
                host.pending.erase(host.pending.begin(), host.pending.begin() + kBytesPerBlock);
                write = (write + kBytesPerBlock) % capacity;
                used += kBytesPerBlock;
                wrote = true;
                if (write == read)
                    break; // full
            }
        };

        flushPending();
        while (capacity - used >= kBytesPerBlock && !(wrote && write == read))
        {
            const int buffer = int(ctx.CurrentBuffer());
            if (!ctx.InputValid(buffer))
            {
                if (!ctx.InputValid(buffer ^ 1))
                    break;
                ctx.SetCurrentBuffer(uint32_t(buffer ^ 1));
                ctx.SetReadOffset(0);
                continue;
            }

            // Loops: back to the loop start once the read offset passes the loop end.
            if (ctx.LoopCount() > 0 && ctx.LoopStart() < ctx.LoopEnd() && ctx.ReadOffset() >= ctx.LoopEnd())
            {
                ctx.SetReadOffset(ctx.LoopStart());
                if (ctx.LoopCount() < 255)
                    ctx.SetLoopCount(ctx.LoopCount() - 1);
                avcodec_flush_buffers(host.codec);
            }

            const uint32_t packets = ctx.PacketCount(buffer);
            const uint32_t packetIndex = ctx.ReadOffset() / kBitsPerPacket;
            if (!g_legacy && !PhysicalRange(ctx.InputPointer(buffer), packets * kBytesPerPacket))
            {
                static std::atomic<uint32_t> logged{ 0 };
                if (logged++ < 8)
                    LOGFN_WARNING("[xma] context {:08X}: input buffer {} at {:08X} x{} packets not usable",
                        host.guest, buffer, ctx.InputPointer(buffer), packets);
                ctx.SetInputValid(buffer, false);
                continue;
            }
            if (packetIndex >= packets)
            {
                // This buffer is used up: the game refills it while the other one plays. A packet
                // skip past its end carries into the next buffer: in multi-stream files (cutscene
                // 5.1: six mono streams in one interleaved block) each stream's next packet is
                // that many packets into the next buffer, not its first packet (which belongs to
                // stream 0). Starting every stream at packet 0 desynchronised five of the six and
                // stalled the game's whole cutscene mix (silent dialog, then everything late).
                const uint32_t carry = packetIndex - packets;
                static const bool logCarry = getenv("DR_XMA_STREAMS") != nullptr;
                if (logCarry && carry != 0)
                {
                    static std::atomic<uint32_t> logged{ 0 };
                    if (logged++ < 64)
                        LOGFN("[xma] context {:08X}: buffer {} done, next packet {} into the other", host.guest, buffer, carry);
                }
                ctx.SetInputValid(buffer, false);
                ctx.SetCurrentBuffer(uint32_t(buffer ^ 1));
                ctx.SetReadOffset(carry * kBitsPerPacket);
                host.swapped = true;
                if (!ctx.InputValid(buffer ^ 1))
                    break;
                continue;
            }

            const uint8_t* data = static_cast<const uint8_t*>(g_memory.Translate(ctx.InputPointer(buffer) | 0xA0000000u)) +
                size_t(packetIndex) * kBytesPerPacket;
            DecodePacket(host, data);

            // Next packet of this stream; the read offset points at its first frame, as on the console.
            const uint32_t next = packetIndex + 1 + PacketSkip(data);
            if (next < packets)
            {
                const uint8_t* nextPacket = data + size_t(next - packetIndex) * kBytesPerPacket;
                ctx.SetReadOffset(next * kBitsPerPacket + std::min(PacketFrameOffset(nextPacket), kBitsPerPacket - 1));
            }
            else
                ctx.SetReadOffset(next * kBitsPerPacket);
            flushPending();
        }

        ctx.SetOutputWrite(write / kBytesPerBlock);
        if (wrote && write == read)
            ctx.SetOutputValid(false); // full: the game re-validates it after reading
        Context now;
        now.Load(guest);
        if (now.d[5] != before.d[5] || now.d[6] != before.d[6] || now.d[7] != before.d[7])
            g_stats.reprogrammed++;
        if (g_legacy)
            ctx.Store(guest);
        else
            ctx.StoreChanges(guest, before);
#else
        (void)host;
#endif
    }

    // Puts a context back to its state after XMACreateContext: no decoder state, nothing pending.
    void ResetHost(HostContext& host)
    {
        host.pending.clear();
        host.streamInput = 0;
        host.swapped = false;
#ifdef DR_HAVE_FFMPEG
        if (host.codec != nullptr)
            avcodec_flush_buffers(host.codec);
#endif
    }

    void Worker()
    {
        auto lastLog = std::chrono::steady_clock::now();
        while (g_running)
        {
            if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(30))
            {
                lastLog = std::chrono::steady_clock::now();
                LOGFN("[xma] last 30 s: {} kicks, {} locks ({} before a pass after the kick), {} clears; {} contexts stopped with work queued, {} repointed during a pass",
                    g_stats.kicks.exchange(0), g_stats.locks.exchange(0), g_stats.lockedUnserviced.exchange(0), g_stats.clears.exchange(0),
                    g_stats.stoppedWithWork.exchange(0), g_stats.reprogrammed.exchange(0));
            }
            for (HostContext& host : g_contexts)
            {
                if (!host.allocated || !(host.enabled || g_legacy))
                    continue;
                std::lock_guard lock(host.mutex);
                if (host.allocated && (host.enabled || g_legacy))
                {
                    Service(host);
                    host.kickPending = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Applies a kick/lock/clear bit mask to the 32 contexts of one register.
    template <typename F>
    void ForEachContext(uint32_t bank, uint32_t mask, F&& f)
    {
        for (uint32_t bit = 0; mask != 0 && bit < 32; bit++, mask >>= 1)
            if (mask & 1)
            {
                const uint32_t index = bank * 32 + bit;
                if (index < kContextCount && g_contexts[index].allocated)
                    f(g_contexts[index]);
            }
    }
}

namespace Xma
{
    bool Available()
    {
#ifdef DR_HAVE_FFMPEG
        return avcodec_find_decoder(AV_CODEC_ID_XMA2) != nullptr;
#else
        return false;
#endif
    }

    void Init()
    {
        void* array = g_userHeap.AllocPhysical(kContextCount * kContextSize, 256);
        memset(array, 0, kContextCount * kContextSize);
        g_arrayGuest = g_memory.MapVirtual(array);
        for (uint32_t i = 0; i < kContextCount; i++)
            g_contexts[i].guest = g_arrayGuest + i * kContextSize;

        // The game reads the register once (lwbrx: little-endian) and numbers its contexts by
        // (MmGetPhysicalAddress(context) - this) / 64 for the kick/lock/clear bits.
        const uint32_t physical = g_arrayGuest >= 0xA0000000 ? (g_arrayGuest & 0x1FFFFFFFu) : g_arrayGuest;
        *static_cast<uint32_t*>(g_memory.Translate(0x7FEA0000u + kRegisterContextArray * 4)) = physical;
        LOGFN("[xma] {} contexts at {:08X} (physical {:08X})", kContextCount, g_arrayGuest, physical);

        bool expected = false;
        if (Available() && g_running.compare_exchange_strong(expected, true))
        {
            std::thread(Worker).detach();
            LOGFN("[xma] decoder thread started");
        }
    }

    uint32_t CreateContext()
    {
        std::lock_guard allocation(g_allocationMutex);
        for (HostContext& host : g_contexts)
        {
            if (host.allocated)
                continue;
            std::lock_guard lock(host.mutex);
            memset(g_memory.Translate(host.guest), 0, kContextSize);
            ResetHost(host);
            host.enabled = false;
            host.allocated = true;
            return host.guest;
        }
        LOGFN_WARNING("[xma] all {} contexts in use", kContextCount);
        return 0;
    }

    void ReleaseContext(uint32_t guestContext)
    {
        const uint32_t index = (guestContext - g_arrayGuest) / kContextSize;
        if (guestContext < g_arrayGuest || index >= kContextCount)
            return;
        std::lock_guard allocation(g_allocationMutex);
        HostContext& host = g_contexts[index];
        std::lock_guard lock(host.mutex);
        host.enabled = false;
        host.allocated = false;
        ResetHost(host);
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 16)
            LOGFN("[xma] register {:03X} = {:08X}", index, value);

        if (index >= kRegisterKick && index < kRegisterKick + kRegisterBanks)
        {
            ForEachContext(index - kRegisterKick, value, [](HostContext& host)
            {
                g_stats.kicks++;
                host.kickPending = true;
                // Debugging: DR_XMA_STARTS=1 logs each kick of a stopped context (a sound starting).
                static const bool logStarts = getenv("DR_XMA_STARTS") != nullptr;
                if (logStarts && !host.enabled)
                {
                    static const auto start = std::chrono::steady_clock::now();
                    Context ctx;
                    ctx.Load(g_memory.Translate(host.guest));
                    LOGFN("[xma] start {:08X} input {:08X} at {} ms", host.guest, ctx.InputPointer(0),
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
                }
                host.enabled = true;
            });
        }
        else if (index >= kRegisterLock && index < kRegisterLock + kRegisterBanks)
        {
            // Waits for a pass in progress: once the store returns the game may reprogram it.
            ForEachContext(index - kRegisterLock, value, [](HostContext& host)
            {
                std::lock_guard lock(host.mutex);
                if (host.kickPending)
                    g_stats.lockedUnserviced++;
                host.enabled = false;
                g_stats.locks++;
                Context ctx;
                ctx.Load(g_memory.Translate(host.guest));
                if (ctx.OutputValid() && (ctx.InputValid(0) || ctx.InputValid(1)))
                    g_stats.stoppedWithWork++;
            });
        }
        else if (index >= kRegisterClear && index < kRegisterClear + kRegisterBanks)
        {
            // As Xenia's XmaContext::Clear: no valid buffers, ring offsets back to 0.
            ForEachContext(index - kRegisterClear, value, [](HostContext& host)
            {
                std::lock_guard lock(host.mutex);
                g_stats.clears++;
                void* guest = g_memory.Translate(host.guest);
                Context before;
                before.Load(guest);
                Context ctx = before;
                ctx.SetInputValid(0, false);
                ctx.SetInputValid(1, false);
                ctx.SetOutputValid(false);
                ctx.SetOutputWrite(0);
                ctx.d[9] &= ~0x1Fu; // output read offset
                ctx.StoreChanges(guest, before);
                ResetHost(host);
            });
        }
    }
}

// Stores the recompiled code marks as MMIO (ppc_detail.h). The APU's registers are little-endian
// (the game writes them with stwbrx), so the register value is the byte-swapped store value.
void DrMmioWrite32(uint32_t address, uint32_t value)
{
    if ((address & 0xFFFF0000u) == 0x7FEA0000u)
        Xma::WriteRegister((address & 0xFFFFu) >> 2, __builtin_bswap32(value));
    else if ((address & 0xFFFF0000u) == 0x7FC80000u)
        CommandProcessor::OnRegisterWrite(address);
}
