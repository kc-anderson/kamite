// Character model replacements (see character_mods.h).
//
// The game's files live in _DATA.AFS on the disc: "AFS\0", u32 count, count x (u32 offset, u32 size) relative to the
// archive start, then (u32 offset, u32 size) of CRI's attribute table (count x 48 bytes: name[32], six u16 date
// fields, u32 file size). GODHAND.FST maps paths to archive indices (tools/snr2/snr2disc.py has the layout). A
// replacement no larger than the original is written over the original's bytes, zero-padded to its size; no size or
// offset on the disc changes.
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace kamite::characters
{
    namespace
    {
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        Character describe(const fs::path &folder)
        {
            Character c;
            c.id = utf8(folder.filename());
            IniFile ini;
            if (ini.load(folder / "character.ini"))
            {
                c.name = ini.get("Character", "Name");
                c.credit = ini.get("Character", "Credit");
            }
            if (c.name.empty())
                c.name = c.id;
            return c;
        }
    }

    std::vector<Character> list(const fs::path &exeDir)
    {
        std::vector<Character> out{{"gene", "Gene", "The game's own"}};
        std::error_code error;
        const fs::path root = exeDir / "mods" / "characters";
        std::vector<fs::path> folders;
        for (const auto &entry : fs::directory_iterator(root, error))
            if (entry.is_directory(error))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        for (const fs::path &folder : folders)
            if (lower(utf8(folder.filename())) != "gene")
                out.push_back(describe(folder));
        return out;
    }

    namespace
    {
        // Every character's full-resolution textures (PCSX2 replacement images named after its in-game textures), not
        // only the chosen one's: a save state made as one character and loaded while another is chosen brings that
        // character's model and 16-colour textures back with it, and its images must still be found (Oct 27: Xavier
        // loaded from a state while Gene was chosen showed his 16-colour body, an olive sleeve and olive buttons).
        // The names are hashes of the textures' own pixels and palettes, so another character's images never match.
        void registerTextures(const fs::path &exeDir)
        {
            std::error_code error;
            std::string folders;
            std::vector<fs::path> all;
            for (const auto &entry : fs::directory_iterator(exeDir / "mods" / "characters", error))
                if (entry.is_directory(error) && fs::is_directory(entry.path() / "textures", error))
                    all.push_back(entry.path() / "textures");
            std::sort(all.begin(), all.end());
            for (const fs::path &textures : all)
            {
                const std::u8string native = fs::path(textures).lexically_normal().make_preferred().u8string();
                folders += (folders.empty() ? "" : "|") + std::string(native.begin(), native.end());
                KM_INFO(Boot, "character textures: " << std::string(native.begin(), native.end()));
            }
            if (folders.empty())
                return;
#if defined(_WIN32)
            _putenv_s("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str());
#else
            setenv("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str(), 1);
#endif
        }
    }

    namespace
    {
        HingedBone g_snakeJaw;

        void readSnakeJaw(const fs::path &folder)
        {
            IniFile ini;
            if (!ini.load(folder / "character.ini") || ini.get("SnakeJaw", "Bone").empty())
                return;
            HingedBone h;
            h.bone = std::atoi(ini.get("SnakeJaw", "Bone").c_str());
            h.parent = std::atoi(ini.get("SnakeJaw", "Parent").c_str());
            if (std::sscanf(ini.get("SnakeJaw", "Pivot").c_str(), "%f %f %f", &h.pivot[0], &h.pivot[1], &h.pivot[2]) != 3 ||
                std::sscanf(ini.get("SnakeJaw", "Axis").c_str(), "%f %f %f", &h.axis[0], &h.axis[1], &h.axis[2]) != 3)
                return;
            h.closed = static_cast<float>(std::atof(ini.get("SnakeJaw", "Closed", "0").c_str()) * 3.14159265358979 / 180.0);
            if (h.bone < 0 || h.bone > 31 || h.parent < 0 || h.parent > 31)
                return;
            g_snakeJaw = h;
            KM_INFO(Boot, "character: snake jaw on bone " << h.bone << " under " << h.parent << ", closes " << ini.get("SnakeJaw", "Closed", "0") << " degrees");
        }
    }

    const HingedBone &snakeJaw() { return g_snakeJaw; }

    void install(const fs::path &exeDir)
    {
        registerTextures(exeDir);
        const char *chosen = std::getenv("KAMITE_CHARACTER");
        const std::string id = chosen ? lower(chosen) : std::string();
        if (id.empty() || id == "gene")
            return;
        const fs::path folder = exeDir / "mods" / "characters" / id;
        std::error_code error;
        if (!fs::is_directory(folder, error))
        {
            KM_WARN(Boot, "character '" << id << "': no folder " << utf8(folder) << "; playing as Gene");
            return;
        }
        readSnakeJaw(folder);
        if (!disc_files::open())
        {
            KM_WARN(Boot, "character '" << id << "': the disc image has no file archive / path table; playing as Gene");
            return;
        }

        std::vector<PS2DiscOverlay::Patch> patches;
        int replaced = 0;
        for (const auto &entry : fs::recursive_directory_iterator(folder, error))
        {
            if (!entry.is_regular_file(error))
                continue;
            const std::string rel = lower(utf8(fs::relative(entry.path(), folder, error)));
            const auto found = disc_files::paths().find(rel);
            if (found == disc_files::paths().end())
                continue; // character.ini and anything else that is not a disc file
            const auto file = disc_files::byIndex(found->second);
            if (!file)
                continue;
            const uint32_t index = file->index, size = file->size;
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.empty() || data.size() > size)
            {
                KM_ERROR(Boot, "character '" << id << "': " << rel << " is " << data.size() << " bytes, the game's file "
                                             << size << "; left out (a replacement must not be larger)");
                continue;
            }
            // The sizes in the archive stay as they are: CRI's file system works out where each file starts from the
            // sizes before it, so a changed size moves every later file (the title room DLL then read as garbage, a
            // black screen after the logo). The replacement is padded with zeros to the original size instead.
            const uint32_t newSize = static_cast<uint32_t>(data.size());
            data.resize(size, 0u);
            patches.emplace_back(file->imageOffset, std::move(data));
            KM_INFO(Boot, "character '" << id << "': " << rel << " (AFS " << index << ", " << size << " -> " << newSize << " bytes)");
            ++replaced;
        }
        if (replaced == 0)
        {
            KM_WARN(Boot, "character '" << id << "': no replacement files in " << utf8(folder) << "; playing as Gene");
            return;
        }
        PS2DiscOverlay::add(std::move(patches));
        KM_INFO(Boot, "playing as " << describe(folder).name << " (" << replaced << " files)");
        // Its full-resolution textures were registered with every character's (registerTextures).
    }

    namespace
    {
        uint32_t g_player = 0, g_coat = 0;
        int g_probeFrames = -1;

        uint32_t r32(uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 4u)), 4);
            return v;
        }

        float rf(uint8_t *rdram, uint32_t a)
        {
            float v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 4u)), 4);
            return v;
        }

        void logBone(uint8_t *rdram, const char *who, uint32_t obj, uint32_t index)
        {
            const uint32_t count = rdram[(obj + 0x2B4u) & (PS2_RAM_SIZE - 1u)];
            if (index >= count)
                return;
            const uint32_t bone = r32(rdram, r32(rdram, obj + 0x278u) + 4u * index);
            if (bone == 0)
                return;
            std::ostringstream out;
            out << "[cloth] " << who << " bone " << index << " @0x" << std::hex << bone << " parent 0x" << r32(rdram, bone + 0x148u)
                << " flags 0x" << r32(rdram, bone + 0x154u) << std::dec;
            for (uint32_t base : {0x80u, 0x40u, 0x00u})
            {
                out << " | +0x" << std::hex << base << std::dec << ":";
                for (uint32_t i = 0; i < 16; ++i)
                    out << ' ' << rf(rdram, bone + base + 4u * i);
            }
            out << " | +0x100: " << rf(rdram, bone + 0x100u) << ' ' << rf(rdram, bone + 0x104u) << ' ' << rf(rdram, bone + 0x108u)
                << " | +0x130: " << rf(rdram, bone + 0x130u) << ' ' << rf(rdram, bone + 0x134u) << ' ' << rf(rdram, bone + 0x138u);
            KM_INFO(Game, out.str());
        }

        // Before the instruction at 0x108088 (after SetParent(coat, player, 17)); s1 = the player.
        void coatProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_player = GPR_U32(ctx, 17);
            g_coat = r32(rdram, g_player + 0x68Cu);
            g_probeFrames = 0;
            KM_INFO(Game, "[cloth] player 0x" << std::hex << g_player << " coat 0x" << g_coat << std::dec << " (bones "
                                              << int(rdram[(g_player + 0x2B4u) & (PS2_RAM_SIZE - 1u)]) << " / "
                                              << int(rdram[(g_coat + 0x2B4u) & (PS2_RAM_SIZE - 1u)]) << ")");
        }
    }

    void installProbe()
    {
        ps2xRegisterHostHook("km_coat_probe", &coatProbe);
    }

    void onFrame(uint8_t *rdram)
    {
        if (g_probeFrames < 0 || g_coat == 0)
            return;
        ++g_probeFrames;
        if (g_probeFrames != 90 && g_probeFrames != 900)
            return;
        KM_INFO(Game, "[cloth] frame " << g_probeFrames << " after the coat was attached");
        for (uint32_t b : {0u, 4u, 17u})
            logBone(rdram, "player", g_player, b);
        for (uint32_t b : {0u, 1u, 2u})
            logBone(rdram, "coat", g_coat, b);
        if (g_probeFrames == 900)
            g_probeFrames = -1;
    }
}
