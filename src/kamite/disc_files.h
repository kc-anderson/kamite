#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The game's files on the disc image: _DATA.AFS ("AFS\0", u32 count, count x (u32 offset, u32 size) relative to the
// archive start, then CRI's attribute table) and the path table that names them (the .FST in the disc root: lower-case
// path -> archive index; tools/snr2/snr2disc.py has the layout). Shared by the file replacements (character mods)
// and the load-time text patches (menu buttons): both end as PS2DiscOverlay patches, set before the game runs.
namespace kamite::disc_files
{
    struct File
    {
        uint32_t index = 0;       // archive index
        uint64_t imageOffset = 0; // byte offset of the file in the disc image
        uint32_t size = 0;        // its size in the archive's table
    };

    // Opens the archive and its path table once (after PS2Runtime::setIoPaths); false when the image or either file
    // is missing (the game then reads its files some other way and nothing here applies).
    bool open();

    // By full path ("pl/pl00.dat", any case) or, failing that, by file name alone when exactly one file has it.
    std::optional<File> find(std::string_view path);
    // Every path in the table (lower case) -> archive index.
    const std::map<std::string, uint32_t> &paths();
    // The archive's table entry of an index.
    std::optional<File> byIndex(uint32_t index);

    // The file's bytes as on the disc (patches already set are applied, as for every read of the image; a file after
    // an insertion is read where it now sits). imageOffset stays the original offset, which is what patches use.
    bool read(const File &file, std::vector<uint8_t> &out);
    // The archive: its byte offset in the image, its file count, and its attribute table (offset from the archive
    // start, size; 0, 0 when it has none).
    uint64_t archiveBase();
    uint32_t archiveCount();
    std::pair<uint32_t, uint32_t> attributeTable();
}
