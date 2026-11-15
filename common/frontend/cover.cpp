#include "cover.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>

#ifdef NX_MUSIC_SYSMODULE
namespace {
constexpr std::size_t imageBudget = 512 * 1024;
// Only the mini panel thread decodes covers in the sysmodule.
std::size_t imageBytes = 0;

struct alignas(std::max_align_t) ImageAllocation {
    std::size_t size;
};

void* allocateImage(std::size_t size)
{
    if (size > imageBudget - sizeof(ImageAllocation))
        return nullptr;
    const auto bytes = size + sizeof(ImageAllocation);
    if (bytes > imageBudget - imageBytes)
        return nullptr;
    auto* allocation = static_cast<ImageAllocation*>(std::malloc(bytes));
    if (!allocation)
        return nullptr;
    allocation->size = size;
    imageBytes += bytes;
    return allocation + 1;
}

void freeImage(void* pointer)
{
    if (!pointer)
        return;
    auto* allocation = static_cast<ImageAllocation*>(pointer) - 1;
    imageBytes -= allocation->size + sizeof(ImageAllocation);
    std::free(allocation);
}

void* resizeImage(void* pointer, std::size_t size)
{
    if (!size) {
        freeImage(pointer);
        return nullptr;
    }
    auto* resized = allocateImage(size);
    if (resized && pointer) {
        const auto* allocation = static_cast<const ImageAllocation*>(pointer) - 1;
        std::memcpy(resized, pointer, std::min(allocation->size, size));
        freeImage(pointer);
    }
    return resized;
}
}

#define STBI_MALLOC(size) allocateImage(size)
#define STBI_REALLOC(pointer, size) resizeImage(pointer, size)
#define STBI_FREE(pointer) freeImage(pointer)
#define STBI_MAX_DIMENSIONS 256
#else
#define STBI_MAX_DIMENSIONS 768
#endif

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace nxmusic {
namespace {
using Byte = std::uint8_t;
using U32 = std::uint32_t;
#ifdef NX_MUSIC_SYSMODULE
constexpr long maxPicture = 512 * 1024;
#else
constexpr long maxPicture = 8 * 1024 * 1024;
#endif
constexpr long maxTag = 32 * 1024 * 1024;
constexpr int maxTagEntries = 4096;
struct Picture { long start = 0, end = 0; };

bool read(FILE* file, void* bytes, std::size_t len)
{
    return std::fread(bytes, 1, len, file) == len;
}

U32 be(const Byte* bytes, int n = 4)
{
    U32 value = 0;
    while (n--) value = (value << 8) | *bytes++;
    return value;
}

U32 syncSafe(const Byte* bytes)
{
    U32 value = 0;
    for (int i = 0; i < 4; ++i) {
        if (bytes[i] & 128) return UINT32_MAX;
        value = (value << 7) | bytes[i];
    }
    return value;
}

struct Slice {
    FILE* file;
    long end;
    long pos;
    long start;
};
int imageRead(void* raw, char* data, int size)
{
    auto& s = *static_cast<Slice*>(raw);
    const auto count = std::fread(data, 1, std::min<long>(size, s.end - s.pos), s.file);
    s.pos += count;
    return static_cast<int>(count);
}
void imageSkip(void* raw, int amount)
{
    auto& s = *static_cast<Slice*>(raw);
    const long next = std::clamp(s.pos + amount, s.start, s.end);
    if (!std::fseek(s.file, next, SEEK_SET)) s.pos = next;
}
int imageEof(void* raw)
{
    auto& s = *static_cast<Slice*>(raw);
    return s.pos >= s.end || std::feof(s.file);
}

bool decode(FILE* file, Picture part, Byte* output)
{
    if (part.start < 0 || part.end <= part.start || part.end - part.start > maxPicture ||
        std::fseek(file, part.start, SEEK_SET)) return false;
    Slice s{file, part.end, part.start, part.start};
    stbi_io_callbacks callbacks{imageRead, imageSkip, imageEof};
    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load_from_callbacks(&callbacks, &s, &w, &h, &channels, 4);
    if (!pixels) return false;
    const int side = std::min(w, h);
    for (int y = 0; y < coverSize; ++y) {
        for (int x = 0; x < coverSize; ++x) {
            const int row = (h - side) / 2 + y * side / coverSize;
            const int col = (w - side) / 2 + x * side / coverSize;
            std::memcpy(output + (y * coverSize + x) * 4, pixels + (row * w + col) * 4, 4);
        }
    }
    stbi_image_free(pixels);
    return true;
}

bool skipString(FILE* file, long end, int unit)
{
    const long start = std::ftell(file);
    if (start < 0) return false;
    // Bound work on a malformed tag that has no terminator.
    end = std::min(end, start + 8192);
    for (long pos = start; pos + unit <= end; pos += unit) {
        const int a = std::fgetc(file);
        const int b = unit == 2 ? std::fgetc(file) : 0;
        if (a == EOF || b == EOF) break;
        if (!a && !b) return true;
    }
    return false;
}

bool id3(FILE* file, long start, long fileEnd, Byte* output)
{
    Byte h[10]{};
    if (fileEnd - start < 10 || std::fseek(file, start, SEEK_SET) || !read(file, h, 10) ||
        std::memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4 || (h[5] & 128)) return false;
    const int version = h[3];
    const long size = syncSafe(h + 6);
    long pos = start + 10;
    const long end = pos + size;
    if (size > maxTag || end > fileEnd) return false;
    if (h[5] & 64) {
        if (version == 2 || size < 4 || !read(file, h, 4)) return false;
        const long ext = version == 4 ? syncSafe(h) : 4L + be(h);
        if (ext < 4 || ext > end - pos) return false;
        pos += ext;
    }
    const int frameHeader = version == 2 ? 6 : 10;
    Picture fallback;
    for (int entry = 0; entry < maxTagEntries && pos + frameHeader <= end; ++entry) {
        if (std::fseek(file, pos, SEEK_SET) || !read(file, h, frameHeader) || !h[0]) break;
        const long length = version == 4 ? syncSafe(h + 4) :
                            version == 3 ? be(h + 4) : be(h + 3, 3);
        const long body = pos + frameHeader;
        if (length <= 0 || length > end - body) break;
        pos = body + length;
        if (length < 6 || std::memcmp(h, version == 2 ? "PIC" : "APIC", version == 2 ? 3 : 4) ||
            (version != 2 && h[9])) continue;
        const int encoding = std::fgetc(file);
        if (encoding < 0 || encoding > 3) continue;
        if (version == 2) {
            if (std::fseek(file, 3, SEEK_CUR)) continue;
        } else if (!skipString(file, pos, 1)) continue;
        if (std::ftell(file) >= pos) continue;
        const int type = std::fgetc(file);
        if (!skipString(file, pos, (encoding == 1 || encoding == 2) ? 2 : 1)) continue;
        const Picture picture{std::ftell(file), pos};
        if (type == 3 && decode(file, picture, output)) return true;
        if (!fallback.end && type != 3) fallback = picture;
    }
    return fallback.end && decode(file, fallback, output);
}

bool flac(FILE* file, long fileEnd, Byte* output)
{
    long pos = 4;
    Picture fallback;
    for (int entry = 0; entry < maxTagEntries && pos + 4 <= fileEnd && pos <= maxTag; ++entry) {
        Byte header[4], n[4];
        if (std::fseek(file, pos, SEEK_SET) || !read(file, header, 4)) break;
        const long body = pos + 4;
        const long length = be(header + 1, 3);
        pos = body + length;
        if (pos > fileEnd) break;
        if ((header[0] & 127) == 6 && length >= 32 && read(file, n, 4)) {
            const auto type = be(n);
            const auto skipField = [&] {
                const auto at = std::ftell(file);
                if (at < 0 || pos - at < 4 || !read(file, n, 4)) return false;
                const long count = be(n);
                return count <= pos - at - 4 && !std::fseek(file, count, SEEK_CUR);
            };
            if (skipField() && skipField() && pos - std::ftell(file) >= 20 &&
                !std::fseek(file, 16, SEEK_CUR) && read(file, n, 4)) {
                const long at = std::ftell(file), len = be(n);
                if (at >= 0 && len <= pos - at) {
                    Picture picture{at, at + len};
                    if (type == 3 && decode(file, picture, output)) return true;
                    if (!fallback.end && type != 3) fallback = picture;
                }
            }
        }
        if (header[0] & 128) break;
    }
    return fallback.end && decode(file, fallback, output);
}

long fileSize(FILE* f)
{
    return std::fseek(f, 0, SEEK_END) ? -1 : std::ftell(f);
}
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
}

std::string title(const std::string& path)
{
    if (path.empty()) return "No track selected";
    const auto slash = path.rfind('/');
    auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

bool loadCover(const std::string& path, Byte* output)
{
    if (path.empty()) return false;
    {
        const File file(std::fopen(path.c_str(), "rb"), std::fclose);
        Byte magic[4]{};
        if (file && read(file.get(), magic, 4)) {
            const long end = fileSize(file.get());
            if (!std::memcmp(magic, "ID3", 3) && id3(file.get(), 0, end, output)) return true;
            if (!std::memcmp(magic, "fLaC", 4) && flac(file.get(), end, output)) return true;
        }
    }
    const auto stem = path.substr(0, path.rfind('.'));
    const auto directory = path.substr(0, path.rfind('/') + 1);
    for (const auto& file : {stem + ".png", stem + ".jpg", stem + ".jpeg",
                            directory + "cover.png", directory + "cover.jpg", directory + "cover.jpeg"}) {
        const File image(std::fopen(file.c_str(), "rb"), std::fclose);
        if (image && decode(image.get(), {0, fileSize(image.get())}, output)) return true;
    }
    return false;
}
}
