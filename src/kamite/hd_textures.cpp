// Full-resolution versions of the textures the port changes. See hd_textures.h.
#include "kamite/hd_textures.h"

#include "kamite/log.h"

// raylib's PNG reader/writer (stb_image, already in the exe through the runtime's window). Its global ::Image is a
// different type from hd_textures::Image.
#include "raylib.h"

// xxHash, header only: PCSX2's copy (the same XXH3-64 its texture cache hashes with).
#define XXH_INLINE_ALL
#include "../../thirdparty/pcsx2/3rdparty/include/xxhash.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kamite::hd_textures
{
    namespace
    {
        bool g_enabled = false;
        fs::path g_generated, g_exeDir;
        std::unordered_map<std::string, fs::path> g_pack; // lower-case stem -> file

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.lexically_normal().make_preferred().u8string();
            return std::string(s.begin(), s.end());
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        void setEnv(const char *name, const std::string &value)
        {
#if defined(_WIN32)
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        // GS memory addressing (GS manual; runtime/gs ps2_gs_psmct32.h / ps2_gs_psmt8.h use the same tables).
        constexpr uint8_t kBlock32[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
        constexpr uint8_t kColumn32[8][8] = {
            {0, 1, 4, 5, 8, 9, 12, 13},         {2, 3, 6, 7, 10, 11, 14, 15},       {16, 17, 20, 21, 24, 25, 28, 29},
            {18, 19, 22, 23, 26, 27, 30, 31},   {32, 33, 36, 37, 40, 41, 44, 45},   {34, 35, 38, 39, 42, 43, 46, 47},
            {48, 49, 52, 53, 56, 57, 60, 61},   {50, 51, 54, 55, 58, 59, 62, 63}};

        int clutSlot(int index) // CSM1: 16x16 CT32 with bits 3 and 4 of the index swapped
        {
            return (index & 0xE7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1);
        }

        int log2i(int v)
        {
            int n = 0;
            while ((1 << (n + 1)) <= v)
                ++n;
            return n;
        }
    }

    void install(const fs::path &exeDir)
    {
        g_exeDir = exeDir;
        const char *textures = std::getenv("PS2X_PCSX2_TEXTURES");
        if (textures && *textures == '0')
        {
            KM_INFO(Boot, "hd textures: texture replacements are off; the port's changed textures draw at the PS2's resolution");
            return;
        }
        std::error_code ec;
        g_generated = exeDir / "cache" / "hd-textures";
        fs::create_directories(g_generated, ec);
        if (!fs::is_directory(g_generated, ec))
        {
            KM_WARN(Boot, "hd textures: could not create " << utf8(g_generated));
            return;
        }
        // The port's images are written again each start; old ones (another binding, another version) go.
        for (const auto &entry : fs::directory_iterator(g_generated, ec))
            if (entry.is_regular_file(ec))
                fs::remove(entry.path(), ec);
        g_enabled = true;
        const char *extra = std::getenv("PS2X_PCSX2_TEXTURE_EXTRA");
        std::string folders = extra ? extra : "";
        folders += (folders.empty() ? "" : "|") + utf8(g_generated);
        setEnv("PS2X_PCSX2_TEXTURE_EXTRA", folders);

        const char *dir = std::getenv("PS2X_PCSX2_TEXTURE_DIR");
        const fs::path pack = dir && *dir ? fs::path(reinterpret_cast<const char8_t *>(dir)) / "replacements" : fs::path();
        size_t files = 0;
        if (!pack.empty() && fs::is_directory(pack, ec))
        {
            for (auto it = fs::recursive_directory_iterator(pack, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string ext = lower(utf8(it->path().extension()));
                if (ext != ".png" && ext != ".dds")
                    continue;
                g_pack.emplace(lower(utf8(it->path().stem())), it->path());
                ++files;
            }
        }
        KM_INFO(Boot, "hd textures: " << files << " images in the texture pack" << (files ? "" : " (none: no pack set or found)")
                                      << "; the port's own in " << utf8(g_generated));
    }

    bool enabled()
    {
        return g_enabled;
    }

    const fs::path &exeDirectory()
    {
        return g_exeDir;
    }

    fs::path packFile(const std::string &name)
    {
        const auto it = g_pack.find(lower(name));
        return it == g_pack.end() ? fs::path() : it->second;
    }

    const fs::path &generatedDir()
    {
        return g_generated;
    }

    std::string name8(const uint8_t *payload, int ctWidth, int ctHeight, const uint8_t *clut, int width, int height)
    {
        // GS memory at TBP 0 as the PSMCT32 upload leaves it (TBW = ctWidth / 64).
        const int ctPages = std::max(1, ctWidth / 64);
        std::vector<uint8_t> mem(static_cast<size_t>(ctWidth) * static_cast<size_t>(ctHeight) * 4u);
        for (int y = 0; y < ctHeight; ++y)
        {
            for (int x = 0; x < ctWidth; ++x)
            {
                const int page = (y / 32) * ctPages + x / 64;
                const int block = kBlock32[(y % 32) / 8][(x % 64) / 8];
                const int column = kColumn32[y % 8][x % 8];
                std::memcpy(&mem[static_cast<size_t>((page * 32 + block) * 256 + column * 4)], payload + (static_cast<size_t>(y) * ctWidth + x) * 4u, 4u);
            }
        }
        // PCSX2's block hash (GSTextureCache HashTextureLevel, whole blocks in block order): PSMT8 blocks are 16x16
        // texels, pages 128x64, TBW = width / 64.
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        const int pagesX = std::max(1, width / 128);
        for (int by = 0; by < height / 16; ++by)
        {
            for (int bx = 0; bx < width / 16; ++bx)
            {
                const int page = (by / 4) * pagesX + bx / 8;
                const size_t block = static_cast<size_t>(page * 32 + kBlock32[by % 4][bx % 8]);
                if ((block + 1u) * 256u <= mem.size())
                    XXH3_64bits_update(&state, &mem[block * 256u], 256u);
            }
        }
        const uint64_t texHash = XXH3_64bits_digest(&state);
        uint8_t entries[1024];
        for (int k = 0; k < 256; ++k)
            std::memcpy(entries + k * 4, clut + clutSlot(k) * 4, 4u);
        const uint64_t clutHash = XXH3_64bits(entries, sizeof(entries));
        const unsigned bits = 0x13u | (static_cast<unsigned>(log2i(width)) << 6) | (static_cast<unsigned>(log2i(height)) << 10) | 0x4000u;
        char name[64];
        std::snprintf(name, sizeof(name), "%llx-%llx-%08x", static_cast<unsigned long long>(texHash),
                      static_cast<unsigned long long>(clutHash), bits);
        return name;
    }

    bool loadPng(const fs::path &path, Image &out)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            return false;
        ::Image image = LoadImageFromMemory(".png", bytes.data(), static_cast<int>(bytes.size()));
        if (!image.data)
            return false;
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        out.width = image.width;
        out.height = image.height;
        const auto *data = static_cast<const uint8_t *>(image.data);
        out.rgba.assign(data, data + static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u);
        UnloadImage(image);
        return out.width > 0 && out.height > 0;
    }

    bool savePng(const fs::path &path, const Image &image)
    {
        ::Image raw{};
        raw.data = const_cast<uint8_t *>(image.rgba.data());
        raw.width = image.width;
        raw.height = image.height;
        raw.mipmaps = 1;
        raw.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        int size = 0;
        unsigned char *png = ExportImageToMemory(raw, ".png", &size);
        if (!png || size <= 0)
            return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(png), size);
        MemFree(png);
        return static_cast<bool>(out);
    }
}
