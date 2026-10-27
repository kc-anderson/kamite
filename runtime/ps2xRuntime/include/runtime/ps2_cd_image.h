#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class PS2CdImage
{
public:
    struct Entry
    {
        uint32_t lba = 0;
        uint64_t size = 0;
        bool directory = false;
    };

    static constexpr uint32_t kSectorSize = 2048u;

    static PS2CdImage &instance();

    bool open(const std::filesystem::path &imagePath);
    void close();
    bool isOpen() const;
    bool ensureOpenFromIoPaths();

    bool find(std::string_view ps2Path, Entry &out) const;
    // Names of the files (not directories) in the image's root directory, upper case.
    std::vector<std::string> rootFileNames() const;
    bool read(uint64_t offset, void *destination, size_t size) const;
    bool readSectors(uint32_t lba, uint32_t count, void *destination) const;
    uint64_t sizeBytes() const;
    std::filesystem::path path() const;

    static std::string normalizeKey(std::string_view ps2Path);

private:
    PS2CdImage() = default;
    bool parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);
    bool readUnlocked(uint64_t offset, void *destination, size_t size) const;

    mutable std::mutex m_mutex;
    std::FILE *m_file = nullptr;
    std::filesystem::path m_path;
    uint64_t m_size = 0;
    bool m_openAttempted = false;
    std::unordered_map<std::string, Entry> m_entries;
};

// Byte patches over the disc image, applied to every read of it (PS2CdImage, the EE sceCdRead stub, the IOP's image
// reads). Set once before the game starts (file replacement mods: a replacement written over its own slot in an
// archive, plus the archive's size fields); empty = the disc as it is.
class PS2DiscOverlay
{
public:
    using Patch = std::pair<uint64_t, std::vector<uint8_t>>; // image byte offset, bytes

    static void set(std::vector<Patch> patches);
    // Adds patches to the ones already set (several sources: file replacements, text patches). Before the game runs.
    static void add(std::vector<Patch> patches);
    static bool active();
    // Overlays the patches on [offset, offset + size) of the image, already read into destination.
    static void apply(uint64_t offset, void *destination, size_t size);

    // Bytes inserted into the image at an offset of the original image (whole sectors; a file replacement larger than
    // its slot): every original byte at or after it is read that much later, so the image the game sees grows. Patches
    // stay in original offsets (they apply to the original bytes wherever those now sit). Before the game runs.
    static void insert(uint64_t originalOffset, std::vector<uint8_t> bytes);
    static uint64_t insertedBytes();
    // Where an original byte now sits in the image the game sees.
    static uint64_t toVirtual(uint64_t originalOffset);
    // Reads [offset, offset + size) of the image the game sees: original bytes through readOriginal(original offset,
    // destination, size) -> bytes read, with the patches applied, and the inserted bytes. Returns the bytes read.
    static size_t read(uint64_t offset, void *destination, size_t size,
                       const std::function<size_t(uint64_t, void *, size_t)> &readOriginal);
};

