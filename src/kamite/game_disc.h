#pragma once

// The player's own disc image (USA, SLUS-21503): found, checked and its boot ELF copied out for the runtime.
// ADAPTED from the Urban Reign port's src/urban/game_disc.h (serial, boot ELF, hash and the disc file list are the game's).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kamite
{
    // The game (USA), SLUS-21503. Values read from the disc image with port_prep/game-shell/_probe/iso_probe.py.
    inline constexpr const char *kSerial = "SLUS-21503";
    inline constexpr const char *kGameTitle = "Kamite";
    inline constexpr const char *kBootElfPath = "SLUS_215.03";
    inline constexpr const char *kBootElfSha256 = "1742f95bef65bdb2aa57b7a77df4ac7619a092e9646e1ea325bc32ec8a64f3cd";
    // SYSTEM.CNF is "BOOT2 = cdrom0:\SLUS_215.03;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n" (57 bytes).
    inline constexpr const char *kSystemCnfBoot = "cdrom0:\\SLUS_215.03;1";

    struct DiscFile
    {
        const char *path;   // ISO9660 path (PS2CdImage::normalizeKey accepts '/' or '\\', no ";1")
        uint64_t size;      // size on the SLUS-21503 disc; a different size is reported, not fatal
        bool required;      // missing = this is not a usable disc image (USA, SLUS-21503)
        const char *what;
    };

    // Root: SYSTEM.CNF, SLUS_215.03, MODULES/ (IOPRP310.IMG, IRX.IMS), _DATA.AFS, 0FLISTDV.DIR, GODHAND.FST.
    inline constexpr DiscFile kDiscFiles[] = {
        {"SYSTEM.CNF", 57u, true, "boot configuration"},
        {"SLUS_215.03", 3607008u, true, "boot ELF"},
        {"MODULES/IRX.IMS", 314304u, true, "IRX module archive"},
        {"MODULES/IOPRP310.IMG", 278353u, true, "IOP replacement (reboot) image"},
        {"_DATA.AFS", 1541885952u, true, "CRI AFS archive (1445 entries; the 81 SNR2 .rel DLLs are entries 706-1428)"},
        {"GODHAND.FST", 32944u, true, "file table"},
        {"0FLISTDV.DIR", 23u, false, "file list (\"WOLVES.FST\\r\\n_DATA.AFS\\r\\n\")"},
    };
    inline constexpr uint32_t kDataAfsEntries = 1445u;

    struct DiscCheckResult
    {
        bool ok = false;
        std::string error;
        std::filesystem::path bootElfPath;
    };

    // The first *.iso in the search roots whose SYSTEM.CNF boots cdrom0:\SLUS_215.03;1 (empty path if none).
    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots);
    // Mounts the image (PS2CdImage singleton), checks SYSTEM.CNF, the disc files above and the boot ELF's SHA-256, and
    // writes the boot ELF to cacheDir/SLUS_215.03 for PS2Runtime::loadELF.
    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &image, const std::filesystem::path &cacheDir);
}
