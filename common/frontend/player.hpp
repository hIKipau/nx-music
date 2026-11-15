#pragma once
#include <array>
#include <string>
#include <switch.h>
#include "cover.hpp"

namespace nxmusic {
constexpr const char* musicPath = "/switch/nx-music/music";
constexpr u32 libraryLimit = 300;

bool isLocalTrack(const char* path);
Result connect(std::string& status);

enum Control { Previous, Center, Next };
struct Player {
    std::string path;
    std::string playbackError;
    std::array<u8, coverSize * coverSize * 4> cover{};
    bool hasCover = false;
    bool playing = false;
    int volume = 100;
    Result changeVolume(int step) const;
    Result refresh();
    Result use(Control control) const;
    const char* centerLabel() const { return playing ? "Stop" : "Play"; }
};
}
