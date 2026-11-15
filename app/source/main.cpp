#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_ttf.h>
#include <switch.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "player.hpp"
#include "shared_font.hpp"
#include "tune.h"

namespace {
constexpr int width = 1280;
constexpr int height = 720;
constexpr int visibleRows = 8;
constexpr SDL_Color ink{231, 243, 248, 255};
constexpr SDL_Color muted{151, 176, 185, 255};
constexpr SDL_Color accent{75, 221, 201, 255};
constexpr SDL_Color panel{19, 36, 45, 255};
constexpr SDL_Color selected{33, 76, 82, 255};

std::vector<std::string> scan()
{
    std::vector<std::string> tracks;
    std::error_code error;
    auto dir = std::filesystem::recursive_directory_iterator(nxmusic::musicPath,
                       std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && dir != end && tracks.size() < nxmusic::libraryLimit) {
        const auto name = dir->path().string();
        std::error_code entryError;
        if (name.size() < 256 && nxmusic::isLocalTrack(name.c_str()) && dir->is_regular_file(entryError))
            tracks.push_back(name);
        dir.increment(error);
    }
    std::sort(tracks.begin(), tracks.end());
    return tracks;
}

void fill(SDL_Renderer* r, SDL_Color color, int x, int y, int w, int h)
{
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    const SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

void text(SDL_Renderer* r, TTF_Font* font, std::string value, SDL_Color color, int x, int y, int limit = 0)
{
    int measured = 0;
    if (limit && TTF_SizeUTF8(font, value.c_str(), &measured, nullptr) == 0 && measured > limit) {
        while (!value.empty()) {
            auto end = value.size() - 1;
            while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
                --end;
            value.resize(end);
            if (TTF_SizeUTF8(font, (value + "…").c_str(), &measured, nullptr) != 0 || measured <= limit)
                break;
        }
        value += "…";
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
        TTF_RenderUTF8_Blended(font, value.c_str(), color), SDL_FreeSurface);
    if (!surface)
        return;
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
        SDL_CreateTextureFromSurface(r, surface.get()), SDL_DestroyTexture);
    const SDL_Rect rect{x, y, surface->w, surface->h};
    if (texture)
        SDL_RenderCopy(r, texture.get(), nullptr, &rect);
}

struct Screen {
    nxmusic::Player player;
    std::vector<std::string> tracks = scan();
    std::size_t row = 0;
    int control = nxmusic::Center;
    bool controls = false;
    Result connection = 0;
    std::string status;

    void rescan()
    {
        const std::string before = tracks.empty() ? "" : tracks[row];
        tracks = scan();
        const auto match = std::find(tracks.begin(), tracks.end(), before);
        row = match == tracks.end() ? 0 : match - tracks.begin();
    }

    Result startTrack() const
    {
        // Pause before replacing the queue so that selecting a track cannot
        // briefly play the first file while the new queue is being populated.
        Result rc = tunePause();
        if (R_FAILED(rc)) return rc;
        rc = tuneClearQueue();
        if (R_FAILED(rc)) return rc;
        u32 index = 0;
        u32 requested = UINT32_MAX;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            rc = tuneEnqueue(tracks[i].c_str(), TuneEnqueueType_Back);
            if (i == row && R_FAILED(rc))
                return rc;
            if (R_SUCCEEDED(rc)) {
                if (i == row)
                    requested = index;
                ++index;
            }
        }
        if (requested == UINT32_MAX)
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        return tuneSelect(requested);
    }

    void draw(SDL_Renderer* r, TTF_Font* body, TTF_Font* titleFont) const
    {
        fill(r, {10, 21, 28, 255}, 0, 0, width, height);
        text(r, titleFont, "NX Music", ink, 48, 24);
        text(r, body, "Press + and − together to open or close the overlay", muted, 275, 39, 730);
        text(r, body, "LOCAL LIBRARY", accent, 1030, 39);

        fill(r, panel, 48, 91, 1184, 177);
        fill(r, accent, 48, 91, 4, 177);
        if (R_SUCCEEDED(connection))
            text(r, body, "Music volume: " + std::to_string(player.volume) + "%", accent, 959, 204);
        if (player.hasCover) {
            std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cover(
                SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                  nxmusic::coverSize, nxmusic::coverSize), SDL_DestroyTexture);
            const SDL_Rect dest{68, 111, 136, 136};
            if (cover && SDL_UpdateTexture(cover.get(), nullptr, player.cover.data(), nxmusic::coverSize * 4) == 0)
                SDL_RenderCopy(r, cover.get(), nullptr, &dest);
        } else {
            fill(r, selected, 68, 111, 136, 136);
            text(r, titleFont, "♫", accent, 121, 153);
        }

        text(r, body, player.playing ? "NOW PLAYING" : "LOCAL PLAYBACK", accent, 226, 105);
        text(r, titleFont, nxmusic::title(player.path), ink, 226, 132, 969);
        const char* labels[] = {"Previous", player.path.empty() ? "Play" : player.centerLabel(), "Next"};
        for (int i = 0; i < 3; ++i) {
            const int x = 226 + i * 174;
            fill(r, controls && control == i ? selected : SDL_Color{12, 25, 32, 255}, x, 193, 160, 46);
            if (controls && control == i)
                fill(r, accent, x, 193, 160, 2);
            text(r, body, labels[i], ink, x + 15, 202);
        }

        text(r, body, "FILES  /  " + std::to_string(tracks.size()), accent, 48, 282);
        if (tracks.empty())
            text(r, body, "Copy MP3, FLAC or WAV to /switch/nx-music/music/", muted, 58, 342);
        const auto first = row / visibleRows * visibleRows;
        for (auto i = first; i < std::min(first + visibleRows, tracks.size()); ++i) {
            const int y = 319 + static_cast<int>(i - first) * 38;
            const bool active = tracks[i] == player.path;
            fill(r, !controls && row == i ? selected : panel, 48, y, 1184, 34);
            if (!controls && row == i)
                fill(r, accent, 48, y, 4, 34);
            text(r, body, std::to_string(i + 1), muted, 60, y + 2);
            text(r, body, nxmusic::title(tracks[i]), active ? accent : ink, 115, y + 2, 980);
            if (active)
                text(r, body, player.playing ? "Playing" : "Stopped", accent, 1123, y + 2);
        }
        const auto& message = status.empty() ? player.playbackError : status;
        if (!message.empty())
            text(r, body, message, accent, 48, 635, 1184);
        text(r, body, "D-pad  Select     A  Confirm     L / R  Skip     ZL / ZR  Volume     X  Rescan     B  Exit",
             muted, 48, 675);
        SDL_RenderPresent(r);
    }
};

TTF_Font* openFont(const PlFontData& font, int size)
{
    SDL_RWops* buffer = SDL_RWFromConstMem(font.address, static_cast<int>(font.size));
    return buffer ? TTF_OpenFontRW(buffer, 1, size) : nullptr;
}
}

int main()
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 1;

    constexpr HidNpadIdType ids[] = {HidNpadIdType_No1, HidNpadIdType_Handheld};
    hidInitializeNpad();
    if (R_FAILED(hidSetSupportedNpadIdType(ids, 2)) ||
        R_FAILED(hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard))) {
        SDL_Log("NX Music could not configure controller input");
        SDL_Quit();
        return 1;
    }
    PadState pad{};
    padInitializeDefault(&pad);

    if (TTF_Init() != 0) {
        SDL_Quit();
        return 1;
    }
    const bool fontReady = R_SUCCEEDED(plInitialize(PlServiceType_User));
    PlFontData font{};
    SDL_Window* window = SDL_CreateWindow("NX Music", 0, 0, width, height, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer && window)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer)
        SDL_RenderSetLogicalSize(renderer, width, height);
    if (fontReady)
        nxmusic::getStandardFont(font);
    TTF_Font* body = font.address ? openFont(font, 22) : nullptr;
    TTF_Font* headline = font.address ? openFont(font, 36) : nullptr;
    if (!renderer || !body || !headline) {
        if (body) TTF_CloseFont(body);
        if (headline) TTF_CloseFont(headline);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (fontReady) plExit();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Screen screen;
    bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (focused)
        screen.connection = nxmusic::connect(screen.status);
    bool dirty = true;
    auto lastPoll = armGetSystemTick();
    bool quitting = false;
    while (!quitting && appletMainLoop()) {
        const bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
        if (!inFocus) {
            if (focused)
                tuneExit();
            focused = false;
            svcSleepThread(50'000'000);
            continue;
        }
        if (!focused) {
            focused = true;
            screen.connection = nxmusic::connect(screen.status);
            dirty = true;
        }

        // Controller input comes from libnx. Drain SDL's window/input queue.
        SDL_PumpEvents();
        SDL_Event event;
        for (int i = 0; i < 64 && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0; ++i) {
            if (event.type == SDL_QUIT)
                quitting = true;
            else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT)
                SDL_free(event.drop.file);
#if SDL_VERSION_ATLEAST(2, 0, 22)
            else if (event.type == SDL_TEXTEDITING_EXT)
                SDL_free(event.editExt.text);
#endif
        }
        if (quitting)
            break;
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        constexpr u64 panelChord = HidNpadButton_Plus | HidNpadButton_Minus;
        if ((padGetButtons(&pad) & panelChord) == panelChord)
            down = 0;
        if (down & HidNpadButton_B)
            break;
        Result action = 0;
        if (down & HidNpadButton_X) {
            screen.rescan();
            if (R_FAILED(screen.connection))
                screen.connection = nxmusic::connect(screen.status);
            if (R_SUCCEEDED(screen.connection))
                screen.status.clear();
        } else if (down & HidNpadButton_AnyUp) {
            if (screen.row > 0 && !screen.controls)
                --screen.row;
            else
                screen.controls = true;
        } else if (down & HidNpadButton_AnyDown) {
            if (screen.controls)
                screen.controls = false;
            else if (screen.row + 1 < screen.tracks.size())
                ++screen.row;
        } else if (screen.controls && (down & HidNpadButton_AnyLeft)) {
            screen.control = (screen.control + 2) % 3;
        } else if (screen.controls && (down & HidNpadButton_AnyRight)) {
            screen.control = (screen.control + 1) % 3;
        } else if (R_SUCCEEDED(screen.connection)) {
            if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                const int step = (down & HidNpadButton_ZR ? 5 : 0) - (down & HidNpadButton_ZL ? 5 : 0);
                action = screen.player.changeVolume(step);
                screen.status = R_FAILED(action) ? "Could not change the music volume." : "";
            } else if (down & HidNpadButton_L)
                action = screen.player.use(nxmusic::Previous);
            else if (down & HidNpadButton_R)
                action = screen.player.use(nxmusic::Next);
            else if (down & HidNpadButton_A) {
                if (screen.controls && !screen.player.path.empty())
                    action = screen.player.use(static_cast<nxmusic::Control>(screen.control));
                else if (!screen.tracks.empty() && (!screen.controls || screen.control == nxmusic::Center))
                    action = screen.startTrack();
                char message[80];
                std::snprintf(message, sizeof(message), "Playback command failed (0x%X).", action);
                screen.status = R_FAILED(action) ? message : "";
            }
        }
        if (down)
            dirty = true;

        if (R_SUCCEEDED(screen.connection) && (down || armTicksToNs(armGetSystemTick() - lastPoll) >= 150'000'000)) {
            const auto path = screen.player.path;
            const auto playbackError = screen.player.playbackError;
            const auto playing = screen.player.playing;
            const auto volume = screen.player.volume;
            screen.connection = screen.player.refresh();
            if (R_FAILED(screen.connection)) {
                tuneExit();
                char message[96];
                std::snprintf(message, sizeof(message), "Background module disconnected (0x%X). Press X to retry.",
                              screen.connection);
                screen.status = message;
                dirty = true;
            }
            dirty |= path != screen.player.path || playing != screen.player.playing || volume != screen.player.volume ||
                     playbackError != screen.player.playbackError;
            lastPoll = armGetSystemTick();
        }
        if (dirty) {
            screen.draw(renderer, body, headline);
            dirty = false;
        }
        svcSleepThread(12'000'000);
    }
    tuneExit();
    TTF_CloseFont(headline);
    TTF_CloseFont(body);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (fontReady) plExit();
    TTF_Quit();
    SDL_Quit();
}
