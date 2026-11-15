#include "player.hpp"
#include "tune.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace nxmusic {
bool isLocalTrack(const char* path)
{
    const char* ext = std::strrchr(path, '.');
    return ext && (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".flac") ||
                   !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".wave"));
}

Result connect(std::string& status)
{
    Result smResult = 0;
    Result rc = tuneInitialize(&smResult);
    char message[160];
    if (R_FAILED(rc)) {
        const char* reason = R_VALUE(rc) == KERNELRESULT(OutOfResource)
            ? "IPC session limit" : "NX Music port unavailable";
        if (R_FAILED(smResult) && smResult != rc)
            std::snprintf(message, sizeof(message), "%s (0x%X), SM (0x%X). Press X to retry.", reason, rc, smResult);
        else
            std::snprintf(message, sizeof(message), "%s (0x%X). Press X to retry.", reason, rc);
        status = message;
        return rc;
    }

    u32 version = 0;
    rc = tuneGetApiVersion(&version);
    if (R_SUCCEEDED(rc) && version == TUNE_API_VERSION) {
        status.clear();
        return 0;
    }
    tuneExit();
    if (R_FAILED(rc)) {
        std::snprintf(message, sizeof(message), "Background module did not answer (0x%X). Press X to retry.", rc);
    } else {
        std::snprintf(message, sizeof(message), "Background module API %u, app expects %u. Update both files and reboot.",
                      version, static_cast<unsigned>(TUNE_API_VERSION));
        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }
    status = message;
    return rc;
}

Result Player::refresh()
{
    bool active = false;
    const Result rc = tuneGetStatus(&active);
    if (R_FAILED(rc))
        return rc;
    char current[256]{};
    TuneCurrentStats stats{};
    tuneGetCurrentQueueItem(current, sizeof(current), &stats);
    if (path != current) {
        path = current;
        hasCover = loadCover(path, cover.data());
    }
    float gain = 0.f;
    if (R_SUCCEEDED(tuneGetVolume(&gain))) volume = std::clamp(int(std::lround(gain * 100)), 0, 100);
    playing = active && !path.empty();

    char failedPath[256]{};
    TunePlaybackError error{};
    if (R_SUCCEEDED(tuneGetPlaybackError(&error, failedPath, sizeof(failedPath)))) {
        playbackError.clear();
        if (R_FAILED(error.result)) {
            const char* reason;
            switch (error.stage) {
                case TunePlaybackErrorStage_File: reason = "Could not open track"; break;
                case TunePlaybackErrorStage_Format: reason = "Could not read track format"; break;
                case TunePlaybackErrorStage_Decode: reason = "Could not decode track"; break;
                case TunePlaybackErrorStage_AudioStart: reason = "Could not start audio output"; break;
                case TunePlaybackErrorStage_AudioSubmit: reason = "Could not submit audio"; break;
                case TunePlaybackErrorStage_AudioWait: reason = "Audio output stalled"; break;
                default: reason = "Playback failed"; break;
            }
            char message[96];
            std::snprintf(message, sizeof(message), "%s (0x%X)", reason, error.result);
            playbackError = message;
            if (*failedPath) {
                playbackError += " - ";
                playbackError += title(failedPath);
            }
        }
    }
    return 0;
}

Result Player::changeVolume(int step) const
{
    const int next = std::clamp(volume + step, 0, 100);
    return next == volume ? 0 : tuneSetVolume(next / 100.f);
}

Result Player::use(Control control) const
{
    if (path.empty())
        return 0;
    switch (control) {
        case Previous: return tunePrev();
        case Center: return playing ? tuneStop() : tunePlay();
        case Next: return tuneNext();
    }
    return 0;
}
}
