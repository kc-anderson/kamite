// The game's archive and path table on the disc image (see disc_files.h).
#include "kamite/disc_files.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"

#include <cctype>
#include <cstring>
#include <fstream>
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
        return readImage(PS2DiscOverlay::toVirtual(file.imageOffset), out, file.size);
    }

    uint64_t archiveBase() { return g_afsBase; }
    uint32_t archiveCount() { return g_count; }
    std::pair<uint32_t, uint32_t> attributeTable()
    {
        if (!g_open)
            return {0u, 0u};
        return {le32(g_toc, 8 + 8u * g_count), le32(g_toc, 12 + 8u * g_count)};
    }

    bool readFromImage(const std::filesystem::path &image, std::string_view path, std::vector<uint8_t> &out)
    {
        std::ifstream in(image, std::ios::binary);
        if (!in)
            return false;
        auto readAt = [&](uint64_t offset, size_t size, std::vector<uint8_t> &buf) {
            buf.assign(size, 0u);
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
            return static_cast<size_t>(in.gcount()) == size;
        };
        constexpr uint64_t kSector = PS2CdImage::kSectorSize;
        // ISO9660: the primary volume descriptor (sector 16) holds the root directory's record at +156.
        std::vector<uint8_t> pvd, root, fst, head, toc;
        if (!readAt(16 * kSector, kSector, pvd) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
            return false;
        const uint32_t rootLba = le32(pvd, 156 + 2), rootSize = le32(pvd, 156 + 10);
        if (rootSize == 0 || rootSize > 1u << 20 || !readAt(rootLba * kSector, rootSize, root))
            return false;
        uint64_t afs = 0, fstAt = 0;
        uint32_t fstSize = 0;
        for (size_t at = 0; at < root.size();)
        {
            const uint8_t length = root[at];
            if (length == 0) // records do not cross sectors: on to the next one
            {
                at = (at / kSector + 1) * kSector;
                continue;
            }
            if (at + 33 > root.size() || at + 33 + root[at + 32] > root.size())
                break;
            std::string name(reinterpret_cast<const char *>(&root[at + 33]), root[at + 32]);
            if (const size_t semi = name.find(';'); semi != std::string::npos)
                name.resize(semi);
            name = lower(name);
            const uint64_t where = static_cast<uint64_t>(le32(root, at + 2)) * kSector;
            if (name == "_data.afs")
                afs = where;
            else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".fst") == 0)
            {
                fstAt = where;
                fstSize = le32(root, at + 10);
            }
            at += length;
        }
        if (afs == 0 || fstAt == 0 || fstSize == 0 || fstSize > 1u << 22 || !readAt(fstAt, fstSize, fst))
            return false;
        const std::map<std::string, uint32_t> names = readFst(fst);
        const auto it = names.find(lower(path));
        if (it == names.end() || !readAt(afs, 8, head) || std::memcmp(head.data(), "AFS", 3) != 0)
            return false;
        const uint32_t count = le32(head, 4);
        if (it->second >= count || !readAt(afs + 8 + 8ull * it->second, 8, toc))
            return false;
        const uint32_t offset = le32(toc, 0), size = le32(toc, 4);
        return size != 0 && size < 1u << 28 && readAt(afs + offset, size, out);
    }
}
