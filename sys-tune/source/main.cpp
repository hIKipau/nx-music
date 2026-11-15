#include "impl/music_player.hpp"
#include "config/config.hpp"
#include "mini_panel.hpp"
#include "pm/pm.hpp"
#include "sdmc/sdmc.hpp"
#include "tune_service.hpp"

namespace {
bool smReady = false;
bool fsReady = false;
bool sdReady = false;
}

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_nv_transfermem_size = 0x15000;

void __libnx_initheap() {
    // The overlay and audio worker share this permanently reserved heap.
    // The overlay's cover decoder has a separate allocation budget.
    alignas(0x1000) static char innerHeap[2 * 1024 * 1024];
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = innerHeap;
    fake_heap_end = innerHeap + sizeof(innerHeap);
}

void __appInit() {
    smReady = R_SUCCEEDED(smInitialize());
}

void __appExit() {
    if (sdReady) sdmc::Close();
    if (fsReady) fsExit();
    if (smReady) smExit();
}
}

namespace {
alignas(0x1000) u8 gpioStack[0x1000];
alignas(0x1000) u8 policyStack[0x1000];
alignas(0x1000) u8 audioStack[0x6000];

bool start(Thread& thread, ThreadFunc fn, void* arg, void* stack, size_t size) {
    if (R_FAILED(threadCreate(&thread, fn, arg, stack, size, 0x20, -2))) return false;
    if (R_SUCCEEDED(threadStart(&thread))) return true;
    threadClose(&thread);
    return false;
}

void join(Thread& thread) {
    threadWaitForExit(&thread);
    threadClose(&thread);
}
}

int main() {
    if (!smReady || R_FAILED(setsysInitialize())) return 1;
    SetSysFirmwareVersion version{};
    const Result firmware = setsysGetFirmwareVersion(&version);
    setsysExit();
    if (R_FAILED(firmware)) return 1;
    hosversionSet(MAKEHOSVERSION(version.major, version.minor, version.micro));

    fsReady = R_SUCCEEDED(fsInitialize());
    sdReady = fsReady && R_SUCCEEDED(sdmc::Open());
    if (!sdReady) return 1;

    // The inherited per-game controls are opt-in. Standard playback uses
    // NX Music's own audio session and can run without process management.
    const bool titlePolicy = config::get_title_policy() && R_SUCCEEDED(pm::Initialize());
    if (R_FAILED(tune::impl::Initialize(titlePolicy))) {
        if (titlePolicy) pm::Exit();
        return 1;
    }

    Thread audio{};
    if (!start(audio, tune::impl::TuneThreadFunc, nullptr, audioStack, sizeof(audioStack))) {
        tune::impl::CloseAudio();
        if (titlePolicy) pm::Exit();
        return 1;
    }

    const bool serverReady = R_SUCCEEDED(tune::InitializeServer());
    if (serverReady) {
        GpioPadSession headphone{};
        const bool gpioReady = R_SUCCEEDED(gpioInitialize());
        const bool headphoneReady = gpioReady && R_SUCCEEDED(gpioOpenSession(&headphone, GpioPadName(0x15)));
        Thread gpio{}, policy{};
        const bool gpioStarted = headphoneReady &&
            start(gpio, tune::impl::GpioThreadFunc, &headphone, gpioStack, sizeof(gpioStack));
        const bool policyStarted = titlePolicy &&
            start(policy, tune::impl::PmdmntThreadFunc, nullptr, policyStack, sizeof(policyStack));

        nxmusic::startMiniPanel();
        tune::LoopProcess();
        tune::ExitServer();
        nxmusic::stopMiniPanel();
        tune::impl::Exit();

        if (gpioStarted) join(gpio);
        if (policyStarted) join(policy);
        if (headphoneReady) gpioPadClose(&headphone);
        if (gpioReady) gpioExit();
    } else {
        tune::impl::Exit();
    }

    join(audio);
    tune::impl::CloseAudio();
    if (titlePolicy) pm::Exit();
    return serverReady ? 0 : 1;
}
