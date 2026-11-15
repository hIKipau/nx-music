#include "mini_panel.hpp"
#include "impl/music_player.hpp"
#include "cover.hpp"
#include "shared_font.hpp"

#include <switch.h>
#include <switch/display/buffer_producer.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

extern "C" u64 __nx_vi_layer_id;

namespace nxmusic {
namespace {
constexpr Result frameBusy = MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock);
constexpr Result fenceTimeout = MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);
constexpr int width = 416;
constexpr int height = 176;
constexpr u32 bg = RGBA8_MAXALPHA(15, 28, 37);
constexpr u32 ink = RGBA8_MAXALPHA(235, 245, 250);
constexpr u32 muted = RGBA8_MAXALPHA(155, 180, 190);
constexpr u32 accent = RGBA8_MAXALPHA(75, 221, 201);
constexpr u32 button = RGBA8_MAXALPHA(28, 47, 59);
constexpr u32 selected = RGBA8_MAXALPHA(35, 75, 82);
constexpr u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;

struct Canvas {
    u16* pixels;
    int stride;

    static u16 pack(u32 color)
    {
        return RGB565_FROM_RGB8(color & 255, (color >> 8) & 255, (color >> 16) & 255);
    }

    void rect(int x, int y, int w, int h, u32 color) const
    {
        const auto pixel = pack(color);
        for (int row = y; row < y + h; ++row)
            std::fill_n(pixels + row * stride + x, w, pixel);
    }

    void blend(int x, int y, u32 color, unsigned alpha) const
    {
        if (!alpha || x < 0 || x >= width || y < 0 || y >= height) return;
        auto& target = pixels[y * stride + x];
        if (alpha == 255) {
            target = pack(color);
            return;
        }
        const u32 underColor = RGBA8_MAXALPHA((target >> 11) * 255 / 31,
                                             ((target >> 5) & 63) * 255 / 63,
                                             (target & 31) * 255 / 31);
        u32 result = 0xff000000;
        for (int shift = 0; shift < 24; shift += 8) {
            const unsigned over = (color >> shift) & 255, under = (underColor >> shift) & 255;
            result |= ((over * alpha + under * (255 - alpha)) / 255) << shift;
        }
        target = pack(result);
    }
};

int codepoint(const char*& text)
{
    const unsigned a = static_cast<u8>(*text++);
    if (a < 0x80) return a;
    int count = a >= 0xf0 && a <= 0xf4 ? 3 : a >= 0xe0 && a <= 0xef ? 2 :
                a >= 0xc2 && a <= 0xdf ? 1 : 0;
    if (!count) return '?';
    int value = a & ((1 << (6 - count)) - 1);
    while (count--) {
        const unsigned b = static_cast<u8>(*text);
        if ((b & 0xc0) != 0x80) return '?';
        value = (value << 6) | (b & 63);
        ++text;
    }
    return value <= 0x10ffff ? value : '?';
}

void drawText(const Canvas& canvas, const stbtt_fontinfo& font, const char* text,
              int size, int x, int baseline, int limit, u32 color)
{
    const float scale = stbtt_ScaleForPixelHeight(&font, size);
    while (*text) {
        int cp = codepoint(text);
        if (!stbtt_FindGlyphIndex(&font, cp)) cp = '?';
        int advance;
        stbtt_GetCodepointHMetrics(&font, cp, &advance, nullptr);
        if (x + std::max(1, int(advance * scale + .5f)) > limit) break;
        int w = 0, h = 0, ox = 0, oy = 0;
        auto* glyph = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &w, &h, &ox, &oy);
        if (glyph) {
            for (int y = 0; y < h; ++y)
                for (int col = 0; col < w && x + ox + col < limit; ++col)
                    canvas.blend(x + ox + col, baseline + oy + y, color, glyph[y * w + col]);
            stbtt_FreeBitmap(glyph, nullptr);
        }
        x += std::max(1, int(advance * scale + .5f));
    }
}

struct Panel {
    ViDisplay display{};
    ViLayer layer{};
    NWindow window{};
    Framebuffer fb{};
    stbtt_fontinfo font{};
    bool vi = false, pl = false, mounted = false;
    u64 managedLayer = 0;

    bool open()
    {
        pl = R_SUCCEEDED(plInitialize(hosversionAtLeast(16, 0, 0) ? PlServiceType_User : PlServiceType_System));
        PlFontData data{};
        if (!pl || R_FAILED(getStandardFont(data)) ||
            !data.address || !stbtt_InitFont(&font, static_cast<const u8*>(data.address), 0)) return false;
        vi = R_SUCCEEDED(viInitialize(ViServiceType_Manager));
        if (!vi || R_FAILED(viOpenDefaultDisplay(&display))) return false;
        if (R_FAILED(viCreateManagedLayer(&display, static_cast<ViLayerFlags>(0), 0, &managedLayer)))
            return false;
        const auto previousLayer = __nx_vi_layer_id;
        __nx_vi_layer_id = managedLayer;
        const Result opened = viCreateLayer(&display, &layer);
        __nx_vi_layer_id = previousLayer;
        if (R_FAILED(opened)) return false;

        s32 z = 0;
        if (R_FAILED(viGetZOrderCountMax(&display, &z)) || z <= 0) z = 255;
        if (R_FAILED(viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer)) ||
            R_FAILED(viSetLayerZ(&layer, z)) ||
            R_FAILED(viSetLayerSize(&layer, width, height)) ||
            R_FAILED(viSetLayerPosition(&layer, 36.f, 38.f))) return false;

        // IManagerDisplayService::AddToLayerStack is not wrapped by libnx.
        struct LayerStackArgs { u32 stack; u32 pad; u64 layerId; };
        const LayerStackArgs main{ViLayerStack_Default, 0, layer.layer_id};
        if (R_FAILED(serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, main))) return false;
        const LayerStackArgs lcd{ViLayerStack_Lcd, 0, layer.layer_id};
        serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, lcd);
        return R_SUCCEEDED(nwindowCreateFromLayer(&window, &layer)) &&
               R_SUCCEEDED(framebufferCreate(&fb, &window, width, height, PIXEL_FORMAT_RGB_565, 2)) &&
               R_SUCCEEDED(framebufferMakeLinear(&fb));
    }

    Result present()
    {
        // nwindowDequeueBuffer waits forever if the display stops consuming frames.
        // This panel has one producer thread, so it can use the async Binder call.
        s32 slot = -1;
        NvMultiFence fence{};
        const Result dequeued = bqDequeueBuffer(&window.bq, true, width, height, window.format,
                                                window.usage, &slot, &fence);
        if (R_FAILED(dequeued)) return dequeued;
        if (slot < 0 || slot >= 64 || fence.num_fences > std::size(fence.fences))
            return MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
        window.cur_slot = slot;
        if (static_cast<u32>(slot) >= fb.num_fbs || !(window.slots_configured & (1ULL << slot))) {
            nwindowCancelBuffer(&window, slot, &fence);
            return MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
        }

        const u64 slotBit = 1ULL << slot;
        if (!(window.slots_requested & slotBit)) {
            const Result requested = bqRequestBuffer(&window.bq, slot, nullptr);
            if (R_FAILED(requested)) {
                nwindowCancelBuffer(&window, slot, &fence);
                return requested;
            }
            window.slots_requested |= slotBit;
        }

        const Result ready = nvMultiFenceWait(&fence, 0);
        if (R_FAILED(ready)) {
            const Result cancelled = nwindowCancelBuffer(&window, slot, &fence);
            if (R_FAILED(cancelled)) return cancelled;
            return R_VALUE(ready) == fenceTimeout ? frameBusy : ready;
        }

        // framebufferCreate uses 16-GOB Tegra block-linear tiles.
        // Copy the linear image in 16-byte sectors to the acquired GPU buffer.
        auto* output = static_cast<u8*>(fb.buf) + static_cast<u32>(slot) * fb.fb_size;
        const auto* input = static_cast<const u8*>(fb.buf_linear);
        constexpr u32 blockHeight = 128;
        constexpr u32 gobWidth = 64;
        constexpr u32 gobHeight = 8;
        static_assert(width * sizeof(u16) % 16 == 0);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width * sizeof(u16); x += 16) {
                const u32 block = (y / blockHeight) * fb.stride * blockHeight +
                                  (x / gobWidth) * gobWidth * blockHeight;
                const u32 gob = ((y % blockHeight) / gobHeight) * gobWidth * gobHeight;
                const u32 sector = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
                                   ((x % 32) / 16) * 32 + (y % 2) * 16;
                std::memcpy(output + block + gob + sector, input + y * fb.stride + x, 16);
            }
        }
        armDCacheFlush(output, fb.fb_size);
        const Result queued = nwindowQueueBuffer(&window, slot, nullptr);
        if (R_FAILED(queued)) nwindowCancelBuffer(&window, slot, nullptr);
        return queued;
    }

    bool close()
    {
        if (fb.has_init) framebufferClose(&fb);
        if (nwindowIsValid(&window)) nwindowClose(&window);
        Result layerResult = 0;
        if (managedLayer) {
            layer.layer_id = managedLayer;
            layerResult = viDestroyManagedLayer(&layer);
        }
        const Result displayResult = display.initialized ? viCloseDisplay(&display) : 0;
        if (vi) viExit();
        if (pl) plExit();
        if (mounted) fsdevUnmountDevice("sdmc");
        *this = {};
        // A failed close can leave server-side state. Stop accepting requests
        // instead of allocating another layer or display every time.
        return R_SUCCEEDED(layerResult) && R_SUCCEEDED(displayResult);
    }
};

struct NowPlaying {
    std::array<u8, coverSize * coverSize * 4> cover{};
    std::string path;
    bool hasCover = false, playing = false;
    int volume = -1, focus = 1;

    bool refresh(bool mounted)
    {
        char current[256]{};
        tune::CurrentStats stats{};
        tune::impl::GetCurrentQueueItem(&stats, current, sizeof(current));
        const bool changedTrack = path != current;
        if (changedTrack) {
            path = current;
            hasCover = mounted && !path.empty() && loadCover("sdmc:" + path, cover.data());
        }
        const bool active = !path.empty() && tune::impl::GetStatus();
        const int level = std::clamp(int(std::lround(tune::impl::GetVolume() * 100)), 0, 100);
        const bool dirty = changedTrack || active != playing || level != volume;
        playing = active;
        volume = level;
        return dirty;
    }

    void draw(Panel& panel) const
    {
        auto* pixels = static_cast<u16*>(panel.fb.buf_linear);
        const Canvas c{pixels, int(panel.fb.stride / sizeof(u16))};
        c.rect(0, 0, width, height, bg);
        c.rect(0, 0, width, 3, accent);
        c.rect(15, 16, coverSize, coverSize, selected);
        if (hasCover) {
            for (int y = 0; y < coverSize; ++y)
                for (int x = 0; x < coverSize; ++x) {
                    const auto* p = &cover[(y * coverSize + x) * 4];
                    c.blend(15 + x, 16 + y, RGBA8_MAXALPHA(p[0], p[1], p[2]), p[3]);
                }
        } else {
            c.rect(66, 44, 4, 35, accent);
            c.rect(66, 44, 15, 4, accent);
            c.rect(55, 75, 15, 8, accent);
        }
        drawText(c, panel.font, playing ? "NOW PLAYING" : "NX MUSIC", 15, 126, 34, 397, accent);
        drawText(c, panel.font, title(path).c_str(), 24, 126, 66, 397, ink);
        char level[32];
        std::snprintf(level, sizeof(level), "Music volume: %d%%", volume);
        drawText(c, panel.font, level, 16, 126, 97, 397, muted);

        const char* labels[] = {"Previous", playing ? "Stop" : "Play", "Next"};
        for (int i = 0; i < 3; ++i) {
            const int x = 15 + 130 * i;
            c.rect(x, 121, 126, 29, focus == i ? selected : button);
            if (focus == i) c.rect(x, 121, 126, 2, accent);
            drawText(c, panel.font, labels[i], 17, x + 11, 142, x + 120, ink);
        }
        drawText(c, panel.font, "A  Confirm      ZL / ZR  Volume      B  Close", 15, 15, 166, 403, muted);
    }
};

std::atomic<bool> running{false};
bool started = false;
Thread thread{};
alignas(0x1000) u8 stack[0x6000];
NowPlaying now;

bool pressedChord(const PadState& pad)
{
    return (padGetButtons(&pad) & chord) == chord && (padGetButtonsDown(&pad) & chord);
}

Result activateNpad()
{
    // Match libnx's hidInitializeNpad IPC, but return errors to the worker.
    // The public wrapper aborts the whole process if activation fails.
    const u64 userId = appletGetAppletResourceUserId();
    Service* hid = hidGetServiceSession();
    if (hosversionBefore(5, 0, 0))
        return serviceDispatchIn(hid, 103, userId, .in_send_pid = true);

    u32 revision = 1;
    if (hosversionAtLeast(18, 0, 0)) revision = 5;
    else if (hosversionAtLeast(8, 0, 0)) revision = 3;
    else if (hosversionAtLeast(6, 0, 0)) revision = 2;

    const struct { u32 revision; u32 padding; u64 userId; } request{revision, 0, userId};
    return serviceDispatchIn(hid, 109, request, .in_send_pid = true);
}

void run(void*)
{
    constexpr HidNpadIdType ids[]{HidNpadIdType_No1, HidNpadIdType_Handheld};
    constexpr int maxHidAttempts = 12;
    bool hidReady = false;
    for (int attempt = 0; running && attempt < maxHidAttempts; ++attempt) {
        if (R_SUCCEEDED(hidInitialize())) {
            hidReady = R_SUCCEEDED(activateNpad()) &&
                       R_SUCCEEDED(hidSetSupportedNpadIdType(ids, std::size(ids))) &&
                       R_SUCCEEDED(hidSetSupportedNpadStyleSet(
                           HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt));
            if (hidReady) break;
            hidExit();
        }
        if (running && attempt + 1 < maxHidAttempts) svcSleepThread(400'000'000);
    }
    if (!hidReady) return;
    PadState handheld{}, controller{};
    padInitialize(&handheld, HidNpadIdType_Handheld);
    padInitialize(&controller, HidNpadIdType_No1);
    Panel panel;
    bool open = false, pendingFrame = false;
    u64 lastPoll = 0;
    u64 frameStarted = 0;
    while (running) {
        padUpdate(&controller);
        padUpdate(&handheld);
        const u64 down = padGetButtonsDown(&controller) | padGetButtonsDown(&handheld);
        const bool toggle = pressedChord(controller) || pressedChord(handheld);
        bool dirty = false;
        if (open && (toggle || (down & HidNpadButton_B))) {
            open = false;
            if (!panel.close()) break;
        } else if (!open && toggle) {
            open = panel.open();
            if (open) {
                panel.mounted = R_SUCCEEDED(fsdevMountSdmc());
                now.path.clear();
                now.hasCover = false;
                now.focus = 1;
                now.refresh(panel.mounted);
                lastPoll = armGetSystemTick();
                dirty = true;
                pendingFrame = false;
            } else {
                if (!panel.close()) break;
            }
        } else if (open) {
            const auto tick = armGetSystemTick();
            if (down || armTicksToNs(tick - lastPoll) >= 150'000'000) {
                dirty = now.refresh(panel.mounted);
                lastPoll = tick;
            }
            if (down & HidNpadButton_AnyLeft) {
                now.focus = (now.focus + 2) % 3;
                dirty = true;
            } else if (down & HidNpadButton_AnyRight) {
                now.focus = (now.focus + 1) % 3;
                dirty = true;
            } else if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                const int step = (down & HidNpadButton_ZR ? 5 : 0) - (down & HidNpadButton_ZL ? 5 : 0);
                const int level = std::clamp(now.volume + step, 0, 100);
                if (level != now.volume) tune::impl::SetVolume(level / 100.f);
                dirty |= now.refresh(panel.mounted);
            } else if ((down & HidNpadButton_A) && !now.path.empty()) {
                if (!now.focus) tune::impl::Prev();
                else if (now.focus == 2) tune::impl::Next();
                else if (now.playing) tune::impl::Stop();
                else tune::impl::Play();
                dirty |= now.refresh(panel.mounted);
            }
        }
        if (open && dirty) {
            now.draw(panel);
            if (!pendingFrame) frameStarted = armGetSystemTick();
            pendingFrame = true;
        }
        if (open && pendingFrame) {
            const Result rc = panel.present();
            if (R_SUCCEEDED(rc)) {
                pendingFrame = false;
            } else if (R_VALUE(rc) != frameBusy ||
                       armTicksToNs(armGetSystemTick() - frameStarted) >= 350'000'000) {
                open = false;
                if (!panel.close()) break;
            }
        }
        svcSleepThread(open ? 18'000'000 : 25'000'000);
    }
    if (open) panel.close();
    hidExit();
}
}

void startMiniPanel()
{
    running = true;
    if (R_FAILED(threadCreate(&thread, run, nullptr, stack, sizeof(stack), 0x29, -2))) {
        running = false;
        return;
    }
    started = R_SUCCEEDED(threadStart(&thread));
    if (!started) {
        threadClose(&thread);
        running = false;
    }
}

void stopMiniPanel()
{
    running = false;
    if (!started) return;
    threadWaitForExit(&thread);
    threadClose(&thread);
    started = false;
}
}
