// The game's archive and path table on the disc image (see disc_files.h).
#include "kamite/disc_files.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"

#include <cctype>
#include <cstring>
#include <functional>

namespace kamite::disc_files
{
    namespace
    {
        bool g_tried = false, g_open = false;
        uint64_t g_afsBase = 0;
        std::vector<uint8_t> g_toc; // the archive's head and table (8 + 8 * count bytes)
        uint32_t g_count = 0;
        std::map<std::string, uint32_t> g_paths;

        uint32_t le32(const std::vector<uint8_t> &b, size_t o)
        {
            return o + 4 > b.size() ? 0u : static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                                               (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
        }

        std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool readImage(uint64_t offset, std::vector<uint8_t> &out, size_t size)
        {
            out.assign(size, 0u);
            return PS2CdImage::instance().read(offset, out.data(), size);
        }

        std::map<std::string, uint32_t> readFst(const std::vector<uint8_t> &buf)
        {
            std::map<std::string, uint32_t> out;
            if (buf.size() < 16 || std::memcmp(buf.data(), "FST", 3) != 0)
                return out;
            const uint32_t stringsOff = le32(buf, 8), entriesOff = le32(buf, 12);
            if (stringsOff > buf.size() || entriesOff > buf.size() || stringsOff > entriesOff)
                return out;
            auto nameAt = [&](uint32_t o) {
                std::string s;
                for (size_t i = stringsOff + o; i < entriesOff && buf[i] != 0; ++i)
                    s.push_back(static_cast<char>(buf[i]));
                return s;
            };
            std::function<void(uint32_t, const std::string &, int)> walk = [&](uint32_t o, const std::string &prefix, int depth) {
                if (depth > 16)
                    return;
                for (;; o += 12u)
                {
                    const size_t at = entriesOff + o;
                    if (at + 12 > buf.size())
                        return;
                    const uint8_t type = buf[at];
                    const uint32_t name = le32(buf, at + 4), value = le32(buf, at + 8);
                    if (type == 3)
                        return;
                    const std::string path = prefix.empty() ? nameAt(name) : prefix + "/" + nameAt(name);
                    if (type == 1)
                        walk(value, path, depth + 1);
                    else if (type == 2)
                        out[lower(path)] = value;
                    else
                        return;
                }
            };
            walk(0, {}, 0);
            return out;
        }

        // The path table: the .FST file in the disc root (found by its extension; there is one).
        bool findFst(PS2CdImage &image, PS2CdImage::Entry &out)
        {
            for (const std::string &name : image.rootFileNames())
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".FST") == 0 && image.find(name, out))
                    return true;
            return false;
        }
    }

    bool open()
    {
        if (g_tried)
            return g_open;
        g_tried = true;
        PS2CdImage &image = PS2CdImage::instance();
        PS2CdImage::Entry afsEntry{}, fstEntry{};
        if (!image.ensureOpenFromIoPaths() || !image.find("_DATA.AFS", afsEntry) || !findFst(image, fstEntry))
            return false;
        g_afsBase = static_cast<uint64_t>(afsEntry.lba) * PS2CdImage::kSectorSize;
        std::vector<uint8_t> head, fst;
        if (!readImage(g_afsBase, head, 8) || std::memcmp(head.data(), "AFS", 3) != 0)
            return false;
        g_count = le32(head, 4);
        if (g_count == 0 || g_count > 100000 || !readImage(g_afsBase, g_toc, 16 + 8u * g_count) ||
            !readImage(static_cast<uint64_t>(fstEntry.lba) * PS2CdImage::kSectorSize, fst, static_cast<size_t>(fstEntry.size)))
            return false;
        g_paths = readFst(fst);
        g_open = !g_paths.empty();
        return g_open;
    }

    std::optional<File> byIndex(uint32_t index)
    {
        if (!g_open || index >= g_count)
            return std::nullopt;
        File f;
        f.index = index;
        f.imageOffset = g_afsBase + le32(g_toc, 8 + 8u * index);
        f.size = le32(g_toc, 12 + 8u * index);
        if (f.size == 0)
            return std::nullopt;
        return f;
    }

    std::optional<File> find(std::string_view path)
    {
        if (!g_open)
            return std::nullopt;
        const std::string key = lower(path);
        if (const auto it = g_paths.find(key); it != g_paths.end())
            return byIndex(it->second);
        if (key.find('/') != std::string::npos)
            return std::nullopt;
        std::optional<uint32_t> only;
        for (const auto &[p, index] : g_paths)
        {
            const size_t slash = p.rfind('/');
            if ((slash == std::string::npos ? p : p.substr(slash + 1)) != key)
                continue;
            if (only)
                return std::nullopt; // two files of that name: the full path is needed
            only = index;
        }
        return only ? byIndex(*only) : std::nullopt;
    }

    const std::map<std::string, uint32_t> &paths()
    {
        return g_paths;
    }

    bool read(const File &file, std::vector<uint8_t> &out)
    {
        return readImage(file.imageOffset, out, file.size);
    }
}
