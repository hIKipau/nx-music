#pragma once
#include <cstdint>
#include <string>

namespace nxmusic {
constexpr int coverSize = 96;
std::string title(const std::string& path);
bool loadCover(const std::string& path, std::uint8_t* output);
}
