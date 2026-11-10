#include "kamite/launcher_steam.h"
#include "kamite/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace kamite::launcher::steam
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char *kAppName = "God Hand: Rearmed";

        // The library art (K's SteamGridDB picks, downloaded when the button is pressed, never shipped; README
        // credits their makers). A dead link is a one-line swap here. The logo and the icon are ours
        // (resources/images/steam_logo.png: the launcher's layers flattened; steam_icon.png: ghr.ico's 256 px frame).
        // The icon is a PNG of its own: Steam reads GHR.exe's PNG-compressed icon frames with a black background.
        struct Art
        {
            const char *name;   // for the popup: "Couldn't download the <name>."
            const char *suffix; // Steam's grid file: <appid><suffix>.<ext>
            const char *url;
        };
        constexpr Art kArt[] = {
            {"cover", "p", "https://cdn2.steamgriddb.com/grid/991c548e1dbad52115bb48b158407334.png"}, // 600x900 (K: stand-in)
            {"wide cover", "", "https://cdn2.steamgriddb.com/grid/78434f3b63efa331e51d1e41dd47cfb0.png"}, // 920x430
            {"hero banner", "_hero", "https://cdn2.steamgriddb.com/hero/7cff4ce87d7f2179d73e7959bf213529.jpg"}, // 3840x1240
        };

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        // ---- The platform's pieces (Linux / Mac: add theirs here; without them Steam reads as not installed) ----
#if defined(_WIN32)
        std::wstring wide(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        std::wstring registryText(const wchar_t *name)
        {
            wchar_t buffer[1024];
            DWORD size = sizeof(buffer);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return buffer;
        }

        fs::path steamRoot()
        {
            const std::wstring root = registryText(L"SteamPath");
            return root.empty() ? fs::path() : fs::path(root).make_preferred();
        }

        fs::path steamExe(const fs::path &root)
        {
            const std::wstring exe = registryText(L"SteamExe");
            return exe.empty() ? root / "steam.exe" : fs::path(exe).make_preferred();
        }

        fs::path thisExe()
        {
            std::wstring path(32768, L'\0');
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            path.resize(n);
            return fs::path(path);
        }

        bool steamRunning()
        {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool found = false;
            for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
                found = _wcsicmp(entry.szExeFile, L"steam.exe") == 0;
            CloseHandle(snapshot);
            return found;
        }

        void runSteam(const fs::path &exe, const wchar_t *arguments)
        {
            ShellExecuteW(nullptr, L"open", exe.c_str(), arguments, exe.parent_path().c_str(), SW_SHOWNORMAL);
        }

        bool download(const char *url, std::vector<uint8_t> &out)
        {
            out.clear();
            const std::wstring address = wide(url);
            wchar_t host[256] = {}, path[2048] = {};
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.lpszHostName = host;
            parts.dwHostNameLength = 256;
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = 2048;
            if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts))
                return false;
            HINTERNET session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr) // before Windows 8.1
                session = WinHttpOpen(L"GHR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr)
                return false;
            WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
            bool ok = false;
            if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0))
            {
                if (HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0))
                {
                    DWORD status = 0, size = sizeof(status);
                    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(request, nullptr) &&
                        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                            &size, WINHTTP_NO_HEADER_INDEX) &&
                        status == 200)
                    {
                        ok = true;
                        for (;;)
                        {
                            DWORD available = 0, read = 0;
                            if (!WinHttpQueryDataAvailable(request, &available))
                            {
                                ok = false;
                                break;
                            }
                            if (available == 0)
                                break;
                            const size_t at = out.size();
                            if (at + available > (64u << 20)) // no library picture is that big
                            {
                                ok = false;
                                break;
                            }
                            out.resize(at + available);
                            if (!WinHttpReadData(request, out.data() + at, available, &read))
                            {
                                ok = false;
                                break;
                            }
                            out.resize(at + read);
                        }
                    }
                    WinHttpCloseHandle(request);
                }
                WinHttpCloseHandle(connection);
            }
            WinHttpCloseHandle(session);
            return ok;
        }

        bool samePath(std::string a, std::string b)
        {
            for (std::string *s : {&a, &b})
                for (char &c : *s)
                    c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a == b;
        }
#else
        fs::path steamRoot() { return {}; }
        fs::path steamExe(const fs::path &) { return {}; }
        fs::path thisExe() { return {}; }
        bool steamRunning() { return false; }
        void runSteam(const fs::path &, const wchar_t *) {}
        bool download(const char *, std::vector<uint8_t> &) { return false; }
        bool samePath(const std::string &a, const std::string &b) { return a == b; }
#endif

        // ---- Steam's files ----

        bool readFile(const fs::path &path, std::vector<uint8_t> &out)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return true;
        }

        bool writeFile(const fs::path &path, const void *data, size_t size)
        {
            const fs::path temp = fs::path(path).concat(".ghr-tmp");
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file || !file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size)))
                    return false;
            }
            std::error_code error;
            fs::rename(temp, path, error);
            if (error)
                fs::remove(temp, error);
            return !error;
        }

        // config/loginusers.vdf (text KeyValues): the account marked MostRecent (the newest Timestamp otherwise),
        // as its account id (the userdata folder's name).
        std::string recentAccount(const fs::path &root)
        {
            std::vector<uint8_t> data;
            if (!readFile(root / "config" / "loginusers.vdf", data))
                return {};
            std::vector<std::string> tokens; // quoted strings; "{" and "}" as themselves
            for (size_t i = 0; i < data.size(); ++i)
            {
                const char c = static_cast<char>(data[i]);
                if (c == '{' || c == '}')
                    tokens.emplace_back(1, c);
                else if (c == '"')
                {
                    std::string text;
                    for (++i; i < data.size() && data[i] != '"'; ++i)
                    {
                        if (data[i] == '\\' && i + 1 < data.size())
                            ++i;
                        text += static_cast<char>(data[i]);
                    }
                    tokens.push_back(text);
                }
            }
            std::string best;
            bool bestRecent = false;
            unsigned long long bestTime = 0;
            int depth = 0;
            std::string user;
            bool recent = false;
            unsigned long long time = 0;
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i] == "{")
                    ++depth;
                else if (tokens[i] == "}")
                {
                    if (depth == 2 && !user.empty() && (best.empty() || (recent && !bestRecent) || (recent == bestRecent && time > bestTime)))
                    {
                        best = user;
                        bestRecent = recent;
                        bestTime = time;
                    }
                    --depth;
                }
                else if (depth == 1 && i + 1 < tokens.size() && tokens[i + 1] == "{")
                {
                    user = tokens[i];
                    recent = false;
                    time = 0;
                }
                else if (depth == 2 && i + 1 < tokens.size() && tokens[i + 1] != "{" && tokens[i + 1] != "}")
                {
                    std::string key = tokens[i];
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (key == "mostrecent")
                        recent = tokens[i + 1] == "1";
                    else if (key == "timestamp")
                        time = std::strtoull(tokens[i + 1].c_str(), nullptr, 10);
                    ++i;
                }
            }
            const unsigned long long id64 = std::strtoull(best.c_str(), nullptr, 10);
            constexpr unsigned long long kIndividualBase = 76561197960265728ull;
            if (id64 <= kIndividualBase)
                return {};
            return std::to_string(id64 - kIndividualBase);
        }

        // Binary KeyValues (shortcuts.vdf): a type byte, a key (C string), the value; 0x08 ends a map. Every node is
        // kept as read, so whatever Steam or other tools stored round-trips; an unknown type stops the read and
        // nothing is written.
        struct Node
        {
            uint8_t type = 0;
            std::string key;
            std::string text;         // 1: string
            std::vector<uint8_t> raw; // 2 / 3 / 4 / 6: four bytes; 7 / 10: eight (little-endian)
            std::vector<Node> kids;   // 0: map
        };

        bool readText(const std::vector<uint8_t> &d, size_t &p, std::string &out)
        {
            const size_t start = p;
            while (p < d.size() && d[p] != 0)
                ++p;
            if (p >= d.size())
                return false;
            out.assign(reinterpret_cast<const char *>(d.data()) + start, p - start);
            ++p;
            return true;
        }

        bool readMap(const std::vector<uint8_t> &d, size_t &p, std::vector<Node> &out, int depth)
        {
            if (depth > 32)
                return false;
            while (p < d.size())
            {
                Node node;
                node.type = d[p++];
                if (node.type == 0x08)
                    return true;
                if (!readText(d, p, node.key))
                    return false;
                size_t bytes = 0;
                switch (node.type)
                {
                case 0:
                    if (!readMap(d, p, node.kids, depth + 1))
                        return false;
                    break;
                case 1:
                    if (!readText(d, p, node.text))
                        return false;
                    break;
                case 2:
                case 3:
                case 4:
                case 6:
                    bytes = 4;
                    break;
                case 7:
                case 10:
                    bytes = 8;
                    break;
                default:
                    return false;
                }
                if (bytes != 0)
                {
                    if (p + bytes > d.size())
                        return false;
                    node.raw.assign(d.begin() + static_cast<std::ptrdiff_t>(p), d.begin() + static_cast<std::ptrdiff_t>(p + bytes));
                    p += bytes;
                }
                out.push_back(std::move(node));
            }
            return false; // ran out before the map's end
        }

        void writeMap(std::vector<uint8_t> &out, const std::vector<Node> &nodes)
        {
            for (const Node &node : nodes)
            {
                out.push_back(node.type);
                out.insert(out.end(), node.key.begin(), node.key.end());
                out.push_back(0);
                if (node.type == 0)
                    writeMap(out, node.kids);
                else if (node.type == 1)
                {
                    out.insert(out.end(), node.text.begin(), node.text.end());
                    out.push_back(0);
                }
                else
                    out.insert(out.end(), node.raw.begin(), node.raw.end());
            }
            out.push_back(0x08);
        }

        bool sameKey(const std::string &a, const char *b)
        {
            const size_t n = std::strlen(b);
            if (a.size() != n)
                return false;
            for (size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        Node *find(std::vector<Node> &nodes, const char *key)
        {
            for (Node &node : nodes)
                if (sameKey(node.key, key))
                    return &node;
            return nullptr;
        }

        std::string unquoted(std::string text)
        {
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            return text;
        }

        void setText(std::vector<Node> &entry, const char *key, const std::string &value)
        {
            if (Node *node = find(entry, key); node != nullptr && node->type == 1)
            {
                node->text = value;
                return;
            }
            Node node;
            node.type = 1;
            node.key = key;
            node.text = value;
            entry.push_back(std::move(node));
        }

        void setNumber(std::vector<Node> &entry, const char *key, uint32_t value)
        {
            Node node;
            node.type = 2;
            node.key = key;
            node.raw = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
                        static_cast<uint8_t>(value >> 24)};
            if (Node *existing = find(entry, key); existing != nullptr)
                *existing = std::move(node);
            else
                entry.push_back(std::move(node));
        }

        uint32_t numberOf(const Node &node)
        {
            return node.raw.size() == 4 ? static_cast<uint32_t>(node.raw[0] | node.raw[1] << 8 | node.raw[2] << 16 | node.raw[3] << 24) : 0u;
        }

        uint32_t crc32(const std::string &text)
        {
            uint32_t crc = 0xFFFFFFFFu;
            for (unsigned char c : text)
            {
                crc ^= c;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        // Our entry in the shortcut list: the one starting this GHR.exe, else one named God Hand: Rearmed (the
        // folder was moved: Update re-points it).
        Node *ourEntry(std::vector<Node> &shortcuts, const std::string &exe)
        {
            Node *named = nullptr;
            for (Node &entry : shortcuts)
            {
                if (entry.type != 0)
                    continue;
                if (const Node *target = find(entry.kids, "Exe"); target != nullptr && samePath(unquoted(target->text), exe))
                    return &entry;
                if (const Node *name = find(entry.kids, "AppName"); named == nullptr && name != nullptr && name->text == kAppName)
                    named = &entry;
            }
            return named;
        }

        struct Account
        {
            fs::path root;   // Steam's folder
            fs::path config; // userdata/<account>/config
        };

        bool locate(Account &account)
        {
            account.root = steamRoot();
            std::error_code error;
            if (account.root.empty() || !fs::is_directory(account.root, error))
                return false;
            const std::string id = recentAccount(account.root);
            if (id.empty() || !fs::is_directory(account.root / "userdata" / id, error))
                return false;
            account.config = account.root / "userdata" / id / "config";
            return true;
        }

        // shortcuts.vdf read into the "shortcuts" map's entries. A missing file is an empty list; a file in a format
        // we don't know is false.
        bool readShortcuts(const fs::path &file, std::vector<Node> &root)
        {
            root.clear();
            std::vector<uint8_t> data;
            std::error_code error;
            if (!fs::exists(file, error) || (readFile(file, data) && data.empty()))
            {
                Node shortcuts;
                shortcuts.key = "shortcuts";
                root.push_back(std::move(shortcuts));
                return true;
            }
            size_t p = 0;
            if (data.empty() || !readMap(data, p, root, 0))
                return false;
            if (find(root, "shortcuts") == nullptr)
            {
                Node shortcuts;
                shortcuts.key = "shortcuts";
                root.push_back(std::move(shortcuts));
            }
            return find(root, "shortcuts")->type == 0;
        }

        // Adds our entry to the list read by readShortcuts, or re-points the one there (true): GHR.exe, its folder,
        // its icon; Steam's id for it in appid.
        bool addEntry(std::vector<Node> &root, const fs::path &exePath, const fs::path &grid, uint32_t &appid)
        {
            std::vector<Node> &shortcuts = find(root, "shortcuts")->kids;
            const std::string exe = utf8(exePath);
            const std::string exeQuoted = "\"" + exe + "\"";
            Node *entry = ourEntry(shortcuts, exe);
            const bool update = entry != nullptr;
            if (entry == nullptr)
            {
                int index = 0;
                while (find(shortcuts, std::to_string(index).c_str()) != nullptr)
                    ++index;
                Node fresh;
                fresh.key = std::to_string(index);
                shortcuts.push_back(std::move(fresh));
                entry = &shortcuts.back();
            }
            // Steam's id for a shortcut: kept once it has one (its art, play time and collections hang on it).
            appid = 0;
            if (const Node *id = find(entry->kids, "appid"); id != nullptr && id->type == 2)
                appid = numberOf(*id);
            if (appid == 0)
            {
                appid = crc32(exeQuoted + kAppName) | 0x80000000u;
                setNumber(entry->kids, "appid", appid);
            }
            setText(entry->kids, "AppName", kAppName);
            setText(entry->kids, "Exe", exeQuoted);
            setText(entry->kids, "StartDir", "\"" + utf8(exePath.parent_path()) + "\"");
            setText(entry->kids, "icon", utf8(grid / (std::to_string(appid) + "_icon.png"))); // written with the art
            if (!update)
            {
                setText(entry->kids, "ShortcutPath", "");
                setText(entry->kids, "LaunchOptions", "");
                setNumber(entry->kids, "IsHidden", 0);
                setNumber(entry->kids, "AllowDesktopConfig", 1);
                setNumber(entry->kids, "AllowOverlay", 1);
                setNumber(entry->kids, "OpenVR", 0);
                setNumber(entry->kids, "Devkit", 0);
                setText(entry->kids, "DevkitGameID", "");
                setNumber(entry->kids, "DevkitOverrideAppID", 0);
                setNumber(entry->kids, "LastPlayTime", 0);
                setText(entry->kids, "FlatpakAppID", "");
                Node tags;
                tags.key = "tags";
                entry->kids.push_back(std::move(tags));
            }
            return update;
        }
    }

    bool running() { return steamRunning(); }

    Status probe()
    {
        Status status;
        Account account;
        if (!locate(account))
            return status;
        status.available = true;
        std::vector<Node> root;
        if (readShortcuts(account.config / "shortcuts.vdf", root))
            status.added = ourEntry(find(root, "shortcuts")->kids, utf8(thisExe())) != nullptr;
        return status;
    }

    Result addOrUpdate(const fs::path &resources, std::atomic<int> &stage)
    {
        Result result;
        const fs::path exePath = thisExe();
        const auto done = [&](bool ok, std::string message) {
            result.ok = ok;
            result.message = std::move(message);
            stage = static_cast<int>(Stage::Done);
            KM_INFO(Boot, "launcher: steam: " << (ok ? "" : "failed: ") << result.message);
            return result;
        };
        Account account;
        if (!locate(account))
            return done(false, "Steam couldn't be found on this PC.");

        // The art first, so Steam is closed only for the moment the files are written.
        stage = static_cast<int>(Stage::Downloading);
        std::vector<uint8_t> art[std::size(kArt)];
        bool got[std::size(kArt)] = {};
        for (size_t i = 0; i < std::size(kArt); ++i)
        {
            got[i] = download(kArt[i].url, art[i]) && art[i].size() > 8 &&
                     ((art[i][0] == 0x89 && art[i][1] == 'P' && art[i][2] == 'N' && art[i][3] == 'G') ||
                      (art[i][0] == 0xFF && art[i][1] == 0xD8 && art[i][2] == 0xFF));
            KM_INFO(Boot, "launcher: steam: " << kArt[i].name << (got[i] ? " downloaded (" : " not downloaded (") << art[i].size() << " bytes)");
        }

        const bool wasRunning = steamRunning();
        const fs::path steam = steamExe(account.root);
        if (wasRunning)
        {
            stage = static_cast<int>(Stage::ClosingSteam);
            runSteam(steam, L"-shutdown");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (steamRunning() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (steamRunning())
                return done(false, "Steam didn't close, so nothing was changed.\nClose Steam yourself and try again.");
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // its last writes
        }
        const auto restart = [&] {
            if (wasRunning)
            {
                stage = static_cast<int>(Stage::StartingSteam);
                runSteam(steam, nullptr);
            }
        };

        stage = static_cast<int>(Stage::Writing);
        const fs::path list = account.config / "shortcuts.vdf";
        std::vector<Node> root;
        if (!readShortcuts(list, root))
        {
            restart();
            return done(false, "Steam's shortcut list couldn't be read, so nothing was changed.");
        }
        const fs::path grid = account.config / "grid";
        uint32_t appid = 0;
        const bool update = addEntry(root, exePath, grid, appid);

        std::error_code error;
        fs::create_directories(account.config, error);
        std::vector<uint8_t> current;
        if (fs::exists(list, error) && readFile(list, current))
            writeFile(fs::path(list).concat(".ghr-backup"), current.data(), current.size()); // the list as it was
        std::vector<uint8_t> bytes;
        writeMap(bytes, root);
        if (!writeFile(list, bytes.data(), bytes.size()))
        {
            restart();
            return done(false, "Steam's shortcut list couldn't be written, so nothing was changed.");
        }

        // The art: grid/<appid><suffix>.<ext>. A picture that didn't download keeps the one from last time.
        fs::create_directories(grid, error);
        const std::string id = std::to_string(appid);
        std::string missing;
        for (size_t i = 0; i < std::size(kArt); ++i)
        {
            if (!got[i])
            {
                missing += std::string("\nThe ") + kArt[i].name + " couldn't be downloaded.";
                continue;
            }
            const char *ext = art[i][0] == 0x89 ? ".png" : ".jpg";
            for (const char *old : {".png", ".jpg", ".jpeg"})
                if (std::strcmp(old, ext) != 0)
                    fs::remove(grid / (id + kArt[i].suffix + old), error);
            if (!writeFile(grid / (id + kArt[i].suffix + ext), art[i].data(), art[i].size()))
                missing += std::string("\nThe ") + kArt[i].name + " couldn't be saved.";
        }
        for (const char *ours : {"_logo", "_icon"})
        {
            std::vector<uint8_t> image;
            if (readFile(resources / "images" / (std::string("steam") + ours + ".png"), image) && !image.empty())
                writeFile(grid / (id + ours + ".png"), image.data(), image.size());
        }

        restart();
        std::string message = update ? "Steam entry updated." : "Added to Steam.";
        message += missing;
        if (!missing.empty())
            message += "\nUpdate the Steam entry later to try again.";
        if (!wasRunning)
            message += "\nIt shows up the next time Steam starts.";
        return done(true, message);
    }
}
