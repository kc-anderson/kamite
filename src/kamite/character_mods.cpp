// Character model replacements (see character_mods.h).
//
// The game's files live in _DATA.AFS on the disc: "AFS\0", u32 count, count x (u32 offset, u32 size) relative to the
// archive start, then (u32 offset, u32 size) of CRI's attribute table (count x 48 bytes: name[32], six u16 date
// fields, u32 file size). GODHAND.FST maps paths to archive indices (tools/snr2/snr2disc.py has the layout). A
// replacement no larger than the original is written over the original's bytes, zero-padded to its size; no size or
// offset on the disc changes. A larger one grows the image the game sees (growFile). Either way the image file itself
// is never written.
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
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
        Hair g_hair;

        bool vec3(const std::string &text, float out[3]) { return std::sscanf(text.c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3; }

        void readHair(const fs::path &folder)
        {
            IniFile ini;
            if (!ini.load(folder / "character.ini") || ini.get("Hair", "BackBone").empty())
                return;
            Hair h;
            auto strand = [&](const char *prefix, HairStrand &s, bool hinge) {
                const std::string p(prefix);
                s.bone = std::atoi(ini.get("Hair", p + "Bone", "-1").c_str());
                s.length = static_cast<float>(std::atof(ini.get("Hair", p + "Length", "0.2").c_str()));
                s.hinge = hinge;
                if (s.bone < 0 || s.bone > 31 || !vec3(ini.get("Hair", p + "Pivot"), s.pivot) || !vec3(ini.get("Hair", p + "Dir"), s.dir))
                    s.bone = -1;
            };
            strand("Back", h.back, false);
            strand("Side", h.side, true);
            auto num = [&](const char *key, float &v) {
                const std::string t = ini.get("Hair", key);
                if (!t.empty())
                    v = static_cast<float>(std::atof(t.c_str()));
            };
            num("Stiffness", h.stiffness);
            num("Damping", h.damping);
            num("Gravity", h.gravity);
            num("Wind", h.wind);
            num("BackForward", h.backForward);
            num("BackBack", h.backBack);
            num("BackSide", h.backSide);
            num("SideSwing", h.sideSwing);
            g_hair = h;
            KM_INFO(Boot, "character: hair on bones " << h.back.bone << " and " << h.side.bone);
        }
    }

    namespace
    {
        std::map<std::string, std::pair<uint32_t, uint32_t>> g_replaced; // disc path -> (game's size, replacement's)

        void putLe32(std::vector<uint8_t> &b, size_t o, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                b[o + i] = static_cast<uint8_t>(v >> (8 * i));
        }
        std::vector<uint8_t> le32Bytes(uint32_t v)
        {
            std::vector<uint8_t> b(4);
            putLe32(b, 0, v);
            return b;
        }
        // ISO9660's both-endian u32: little-endian, then big-endian.
        std::vector<uint8_t> bothEndian(uint32_t v)
        {
            std::vector<uint8_t> b(8);
            putLe32(b, 0, v);
            for (int i = 0; i < 4; ++i)
                b[4 + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
            return b;
        }
        uint32_t readLe32(uint64_t imageOffset)
        {
            uint8_t b[4] = {};
            PS2CdImage::instance().read(imageOffset, b, 4);
            return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
                   (static_cast<uint32_t>(b[3]) << 24);
        }

        struct IsoRecord
        {
            uint64_t at = 0; // the record's byte offset in the image
            uint32_t lba = 0, size = 0;
            bool directory = false;
            std::string name;
        };
        // Every directory record of the image (files and directories, not "." / ".."), by walking from the root.
        bool isoRecords(std::vector<IsoRecord> &out, std::vector<uint32_t> &directoryLbas)
        {
            constexpr uint32_t kSector = PS2CdImage::kSectorSize;
            std::vector<uint8_t> pvd(kSector);
            if (!PS2CdImage::instance().read(16ull * kSector, pvd.data(), kSector) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
                return false;
            auto u32at = [](const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); };
            std::function<bool(uint32_t, uint32_t, int)> walk = [&](uint32_t lba, uint32_t size, int depth) {
                if (depth > 8 || size > 16u * 1024u * 1024u)
                    return false;
                directoryLbas.push_back(lba);
                std::vector<uint8_t> dir(size);
                if (!PS2CdImage::instance().read(static_cast<uint64_t>(lba) * kSector, dir.data(), size))
                    return false;
                for (uint32_t o = 0; o < size;)
                {
                    const uint8_t len = dir[o];
                    if (len == 0)
                    {
                        o = (o / kSector + 1) * kSector; // records do not cross sectors
                        continue;
                    }
                    if (o + len > size || len < 34)
                        return false;
                    const uint8_t nameLen = dir[o + 32];
                    const bool self = nameLen == 1 && (dir[o + 33] == 0 || dir[o + 33] == 1);
                    if (!self)
                    {
                        IsoRecord r;
                        r.at = static_cast<uint64_t>(lba) * kSector + o;
                        r.lba = u32at(&dir[o + 2]);
                        r.size = u32at(&dir[o + 10]);
                        r.directory = (dir[o + 25] & 2u) != 0u;
                        r.name.assign(reinterpret_cast<const char *>(&dir[o + 33]), nameLen);
                        out.push_back(r);
                        if (r.directory && !walk(r.lba, r.size, depth + 1))
                            return false;
                    }
                    o += len;
                }
                return true;
            };
            return walk(u32at(&pvd[156 + 2]), u32at(&pvd[156 + 10]), 0);
        }
    }

    // A replacement larger than the game's file. The archive (CRI AFS) is read by CRI's file system, which places each
    // file after the sectors of the ones before it (so a bigger size alone shifted every later file: the title DLL
    // read as garbage). Instead the image the game sees grows: the replacement's extra sectors are inserted right
    // after its slot (PS2DiscOverlay::insert), every later byte of the image moves back by them, and every table that
    // says where those bytes are says so too: the archive's table (this file's size, the later files' offsets, the
    // attribute table's offset and this file's entry in it), the ISO directory (the archive's size, the extents of
    // files after it) and the volume size. Nothing of the disc is written; the game's own loader reads the larger file.
    // Refused (false) when the archive is not laid out as expected or a directory lies after the slot.
    bool growFile(const disc_files::File &file, const std::string &rel, std::vector<uint8_t> data, std::vector<PS2DiscOverlay::Patch> &patches)
    {
        constexpr uint32_t kSector = PS2CdImage::kSectorSize;
        if (PS2DiscOverlay::insertedBytes() != 0u)
        {
            KM_WARN(Boot, "character: only one replacement may be larger than the game's file; " << rel << " is not");
            return false;
        }
        const uint64_t base = disc_files::archiveBase();
        const uint32_t count = disc_files::archiveCount();
        const uint32_t offset = static_cast<uint32_t>(file.imageOffset - base);
        const uint32_t oldSectors = (file.size + kSector - 1u) / kSector;
        const uint32_t newSize = static_cast<uint32_t>(data.size());
        const uint32_t newSectors = (newSize + kSector - 1u) / kSector;
        const uint32_t grow = (newSectors - oldSectors) * kSector;
        const uint32_t slotEnd = offset + oldSectors * kSector; // archive-relative: where the insertion goes
        if (base % kSector != 0u || offset % kSector != 0u)
            return false;
        // The archive packs its files back to back: the next file must start right at the slot's end.
        uint32_t next = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto other = disc_files::byIndex(i);
            if (!other || i == file.index)
                continue;
            const uint32_t o = static_cast<uint32_t>(other->imageOffset - base);
            if (o > offset && o < slotEnd)
                return false;
            if (o >= slotEnd)
                next = std::min(next, o);
        }
        const auto [attrOffset, attrSize] = disc_files::attributeTable();
        if (attrOffset >= slotEnd)
            next = std::min(next, attrOffset);
        if (next != UINT32_MAX && next != slotEnd)
        {
            KM_WARN(Boot, "character: the archive does not continue right after " << rel << " (next at 0x" << std::hex << next
                                                                                << ", slot ends 0x" << slotEnd << std::dec << ")");
            return false;
        }
        // The ISO directory: the archive's record, and every extent after the insertion point.
        std::vector<IsoRecord> records;
        std::vector<uint32_t> directories;
        if (!isoRecords(records, directories))
            return false;
        const uint64_t at = base + slotEnd; // image offset of the insertion
        const IsoRecord *archive = nullptr;
        for (const IsoRecord &r : records)
            if (!r.directory && static_cast<uint64_t>(r.lba) * kSector == base)
                archive = &r;
        if (!archive || base + archive->size < at)
            return false;
        for (uint32_t lba : directories)
            if (static_cast<uint64_t>(lba) * kSector >= at)
            {
                KM_WARN(Boot, "character: a directory of the disc lies after " << rel << "; the archive is not grown");
                return false;
            }

        // The archive's table: this file's size, later files' offsets, the attribute table's offset and entry.
        patches.emplace_back(base + 12u + 8u * file.index, le32Bytes(newSize));
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto other = disc_files::byIndex(i);
            if (other && i != file.index && other->imageOffset - base >= slotEnd)
                patches.emplace_back(base + 8u + 8u * i, le32Bytes(static_cast<uint32_t>(other->imageOffset - base) + grow));
        }
        if (attrOffset != 0u && attrSize >= 48u * count)
        {
            if (attrOffset >= slotEnd)
                patches.emplace_back(base + 8u + 8u * count, le32Bytes(attrOffset + grow));
            const uint64_t sizeField = base + attrOffset + 48u * file.index + 44u;
            if (readLe32(sizeField) == file.size)
                patches.emplace_back(sizeField, le32Bytes(newSize));
            else
                KM_WARN(Boot, "character: " << rel << "'s attribute entry does not hold its size; left as it is");
        }
        // The ISO: the archive's size, later extents, the volume size.
        patches.emplace_back(archive->at + 10u, bothEndian(archive->size + grow));
        for (const IsoRecord &r : records)
            if (&r != archive && static_cast<uint64_t>(r.lba) * kSector >= at)
                patches.emplace_back(r.at + 2u, bothEndian(r.lba + grow / kSector));
        patches.emplace_back(16ull * kSector + 80u, bothEndian(readLe32(16ull * kSector + 80u) + grow / kSector));

        // The bytes: the slot gets the head, the insertion the rest (zero-padded to whole sectors).
        const size_t head = static_cast<size_t>(oldSectors) * kSector;
        std::vector<uint8_t> tail(data.begin() + static_cast<std::ptrdiff_t>(std::min(head, data.size())), data.end());
        tail.resize(grow, 0u);
        data.resize(head, 0u);
        patches.emplace_back(file.imageOffset, std::move(data));
        PS2DiscOverlay::insert(at, std::move(tail));
        g_replaced[rel] = {file.size, newSize};
        return true;
    }

    const std::map<std::string, std::pair<uint32_t, uint32_t>> &replacedFiles() { return g_replaced; }

    const Hair &hair() { return g_hair; }

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
        readHair(folder);
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
            if (data.empty())
                continue;
            if (data.size() > size)
            {
                if (growFile(*file, rel, std::move(data), patches))
                {
                    KM_INFO(Boot, "character '" << id << "': " << rel << " (AFS " << index << ", " << size << " -> "
                                                << g_replaced[rel].second << " bytes, the archive grown to fit)");
                    ++replaced;
                }
                else
                    KM_ERROR(Boot, "character '" << id << "': " << rel << " is larger than the game's file and the archive "
                                                 << "could not be grown safely; left out");
                continue;
            }
            g_replaced[rel] = {size, static_cast<uint32_t>(data.size())};
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
        if (PS2DiscOverlay::insertedBytes() != 0u)
            PS2CdImage::instance().open(PS2CdImage::instance().path()); // its directory again, as the game now sees it
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

    namespace
    {
        // The replaced file a cDvd read names (a1), or none.
        const std::pair<const std::string, std::pair<uint32_t, uint32_t>> *replacedRead(const uint8_t *rdram, R5900Context *ctx)
        {
            if (g_replaced.empty())
                return nullptr;
            std::string path;
            for (uint32_t a = GPR_U32(ctx, 5) & 0x01FFFFFFu, i = 0; i < 128u && a + i < PS2_RAM_SIZE && rdram[a + i]; ++i)
            {
                char c = static_cast<char>(std::tolower(rdram[a + i]));
                path.push_back(c == '\\' ? '/' : c);
            }
            while (!path.empty() && (path[0] == '/' || path[0] == '.'))
                path.erase(0, 1);
            const auto it = std::find_if(g_replaced.begin(), g_replaced.end(), [&](const auto &kv) {
                return path.size() >= kv.first.size() && path.compare(path.size() - kv.first.size(), kv.first.size(), kv.first) == 0;
            });
            return it == g_replaced.end() ? nullptr : &*it;
        }

        // The player's files are read (cDvd_ReadToBuffer, func_0030D5C8 at 0x30D98C / 0x30D9D4) into two buffers the
        // game allocates from heap A (0x754200) through 0x30EE08 (a0 size, a1 alignment, a2 cHeap): 0x1E6800 bytes for
        // pl/pl00.dat at 0x30D7A8 (the largest costume, pl11, is 973 sectors) and 0x19800 for pl/pl01.dat at
        // 0x30D7C8; the pointers sit at 0x752C38 / 0x752C3C. A replacement larger than its buffer gets a larger one
        // (whole sectors), so the read does not run past it. cHeap {+0 first block, +4 size}; block {+8 next,
        // +0xC free bytes after it}.
        struct PlayerBuffer
        {
            uint32_t ra;
            const char *file;
            uint32_t gameSize;
        };
        constexpr PlayerBuffer kPlayerBuffers[] = {{0x0030D7B0u, "pl/pl00.dat", 0x1E6800u}, {0x0030D7D0u, "pl/pl01.dat", 0x19800u}};

        void heapRoom(uint8_t *rdram, uint32_t heap, uint32_t &largest, uint32_t &total)
        {
            largest = total = 0u;
            uint32_t block = r32(rdram, heap) & 0x01FFFFFFu;
            for (uint32_t n = 0; block && block + 0x14u <= PS2_RAM_SIZE && n < 4096u; ++n)
            {
                const uint32_t freeBytes = r32(rdram, block + 0xCu);
                largest = std::max(largest, freeBytes);
                total += freeBytes;
                block = r32(rdram, block + 8u) & 0x01FFFFFFu;
            }
        }

        void onHeapAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_replaced.empty())
                return;
            const uint32_t ra = GPR_U32(ctx, 31), asked = GPR_U32(ctx, 4);
            for (const PlayerBuffer &b : kPlayerBuffers)
            {
                if (ra != b.ra || asked != b.gameSize)
                    continue;
                const auto it = g_replaced.find(b.file);
                if (it == g_replaced.end())
                    return;
                const uint32_t need = (it->second.second + 0x7FFu) & ~0x7FFu;
                if (need <= asked)
                    return;
                ctx->r[4] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[4], 1), static_cast<int64_t>(static_cast<int32_t>(need)));
                const uint32_t heap = GPR_U32(ctx, 6) & 0x01FFFFFFu;
                uint32_t largest = 0u, total = 0u;
                if (heap != 0u && heap + 8u <= PS2_RAM_SIZE)
                    heapRoom(rdram, heap, largest, total);
                if (largest >= need + 0x40u)
                    KM_INFO(Game, "character: " << b.file << " buffer " << asked << " -> " << need << " bytes (heap 0x" << std::hex << heap
                                                << std::dec << ": largest free block " << largest << ", " << total << " free, before it)");
                else
                    KM_WARN(Game, "character: " << b.file << " needs a " << need << "-byte buffer; heap 0x" << std::hex << heap << std::dec
                                                << " has " << largest << " in its largest free block (" << total << " free)");
                return;
            }
        }
    }

    namespace
    {
        // cDvd_ReadToBuffer(a0 cDvd, a1 path, a2 buffer, a3 id, t0 size or 0): a replaced file read into a buffer the
        // caller made. A heap block's header sits 0x20 before its data (+0x10 used bytes): a buffer smaller than the
        // replacement is an error worth knowing at once (the read would run past it).
        void onReadToBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const auto *it = replacedRead(rdram, ctx);
            if (!it)
                return;
            const uint32_t buffer = GPR_U32(ctx, 6) & 0x01FFFFFFu, asked = GPR_U32(ctx, 8);
            const uint32_t used = buffer >= 0x20u ? r32(rdram, buffer - 0x20u + 0x10u) : 0u;
            if (asked != 0u && asked < it->second.second)
                KM_WARN(Game, "character: " << it->first << " read into a buffer with " << asked << " bytes asked; the replacement is "
                                            << it->second.second << " (cut short)");
            else if (used != 0u && used < it->second.second)
                KM_ERROR(Game, "character: " << it->first << " (" << it->second.second << " bytes) read into a buffer of " << used
                                             << " bytes (ra 0x" << std::hex << GPR_U32(ctx, 31) << std::dec << "): too small");
            else
                KM_INFO(Game, "character: " << it->first << " read into a buffer (ra 0x" << std::hex << GPR_U32(ctx, 31) << std::dec << ")");
        }
    }

    void installLoadWatch()
    {
        FunctionHooks::instance().observeEntry(0x0030EE08u, "character_player_buffer", &onHeapAlloc);
        FunctionHooks::instance().observeEntry(0x00200B88u, "character_load_watch_buffer", &onReadToBuffer);
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
