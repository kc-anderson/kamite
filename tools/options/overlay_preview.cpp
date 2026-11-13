// Preview of the OPTIONS overlay without a Windows build: the DLL's own screen-drawing code (cut from
// thirdparty/pcsx2-gs/compat/HostStubs.cpp into screen_snippet.inc) and options_overlay.cpp, drawn with ImGui and
// rasterised in software over the wall. Built and run by overlay_preview.py.
#include "imgui.h"
#include "imgui_internal.h"
#include "pcsx2gs_api.h"
#include "kamite/options_overlay.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    float g_presentRect[4] = {};
    std::mutex g_screenMutex;
    std::vector<Pcsx2GsScreenItem> g_screenItems;
    std::string g_screenStrings;
    uint32_t g_screenStale = 0;
    constexpr uint32_t kScreenStaleLimit = 8;
    struct ScreenFont
    {
        ImFont *font = nullptr;
        float capRatio = 0.7f;
    };
    std::vector<std::pair<std::string, ScreenFont>> g_screenFonts;
    std::vector<std::pair<std::string, ImTextureData *>> g_screenImages;
    // The DLL decodes PNGs with WIC; here overlay_preview.py hands over "<path>.rgba" (width, height, then RGBA rows).
    bool LoadScreenImage(const char *path, std::vector<uint8_t> &rgba, int &width, int &height)
    {
        FILE *f = fopen((std::string(path) + ".rgba").c_str(), "rb");
        if (!f)
            return false;
        uint32_t size[2] = {};
        bool ok = fread(size, 4, 2, f) == 2 && size[0] && size[1];
        if (ok)
        {
            width = int(size[0]), height = int(size[1]);
            rgba.resize(size_t(width) * height * 4);
            ok = fread(rgba.data(), 1, rgba.size(), f) == rgba.size();
        }
        fclose(f);
        return ok;
    }
}
#include "screen_snippet.inc"

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

int main(int argc, char **argv)
{
    // args: wall.rgb W H out.rgb pause(1/0) cursor confirmRow blur shadowAlpha
    const char *wallPath = argv[1];
    const int W = atoi(argv[2]), H = atoi(argv[3]);
    const char *outPath = argv[4];
    std::vector<float> img(size_t(W) * H * 3);
    {
        std::vector<unsigned char> raw(size_t(W) * H * 3);
        FILE *f = fopen(wallPath, "rb");
        if (fread(raw.data(), 1, raw.size(), f) != raw.size())
            return 1;
        fclose(f);
        for (size_t i = 0; i < raw.size(); ++i) img[i] = raw[i] / 255.0f;
    }
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = ImVec2(float(W), float(H));
    io.Fonts->AddFontDefault();
    ImGui::NewFrame();
    g_presentRect[0] = 0; g_presentRect[1] = 0; g_presentRect[2] = float(W); g_presentRect[3] = float(H);

    using namespace kamite::options_overlay;
    // args 5..: pause cursor confirmRow blur shadowAlpha labelFont helpFont title [scrollTrack scrollThumb page open
    // pageCursor pageScroll bracket preview]
    setAssets(argv[10], argv[11], argc > 12 ? argv[12] : "");
    if (argc > 14)
        setScrollBar(argv[13], argv[14]);
    look().blur = atof(argv[8]);
    look().shadowAlpha = atof(argv[9]);
    static const Row pauseRows[] = {{"CONTROLS", "Display controls"}, {"DISPLAY", "Picture and camera."},
        {"INTERFACE", "The HUD, button prompts and gauges."}, {"GAMEPLAY", "Techniques, enemies and new\nways to play."},
        {"SOUND", "Sound mode and volume levels."}, {"LOAD GAME", "Select a save file and\nrestart the game."},
        {"RESET GAME", "Return to title screen"}, {"RESTORE DEFAULTS", "Set every option back to\nits default."}, {"EXIT", ""}};
    static const Row titleRows[] = {pauseRows[0], pauseRows[1], pauseRows[2], pauseRows[3], pauseRows[4], pauseRows[7], pauseRows[8]};

    // Mockup pages (the launcher's options; the real ones come from the settings table).
    static const char *const onOff[] = {"ON", "OFF"};
    static const char *const styles[] = {"CLASSIC", "MODERN"};
    static const char *const timers[] = {"NORMAL", "DOUBLE"};
    static const char *const huds[] = {"16:9", "21:9", "32:9", "FULL WIDTH"};
    static const char *const prompts[] = {"AUTOMATIC", "PLAYSTATION", "XBOX", "NINTENDO", "STEAM DECK", "KEYBOARD"};
    auto opt = [](const char *name, const char *help, int choice = 1, bool sub = false, const char *const *choices = nullptr, int count = 2) {
        PageRow r;
        r.name = name, r.help = help, r.sub = sub, r.choices = choices ? choices : onOff, r.choiceCount = count, r.choice = choice;
        return r;
    };
    auto head = [](const char *name) {
        PageRow r;
        r.name = name, r.header = true;
        return r;
    };
    static const PageRow gameplay[] = {
        head("SPEED & FEED"), opt("60 FPS", "Runs the game at 60 frames a second, its\ntimers kept at the PS2's speed.", 0),
        head("MOVE & GROOVE"), opt("CLICK-STICK DASH", "Clicking the left stick dashes.", 0),
        opt("CONTROL STYLE", "Classic: as on the PS2. Modern: the free\ncamera, the right stick turns it.", 1, false, styles, 2),
        opt("MODERN: AUTO", "Fighting starts brawl mode automatically.", 0, true),
        opt("MOUSE CAMERA", "The mouse turns the free camera.", 0, true),
        opt("BUSTA-CHAN JIVE", "Sprint attacks carry Gene over or through\nthe railings marked for them."),
        head("STYLE SHUFFLE"), opt("MOVESET PRESETS", "Keep up to four sets of techniques and\nswap between them with the d-pad.", 0),
        opt("SIGNAL STYLE SWITCH", "Gene calls out each style as you switch\nto it, and his God Hand flashes.", 0, true),
        opt("BUCK BREAKER", "Choose how to punish stunned enemies\non the Techniques screen."),
        head("FIGHTIN' EXCITEMENT"), opt("CUNT PUNTER", "Ball Buster works on female enemies too."),
        opt("TENSION IS PLACABLE", "R2 again ends the God Hand early and\nkeeps the tension left.", 0),
        opt("OFF THE CUFF", "Unleash the God Hand with any tension.", 1, true),
        opt("WILD CURRY FUNK", "Lets you pick up, carry, and throw chairs."),
        head("ROULETTE WHEEL & REEL"), opt("ROULETTE TIMER", "How long you have to pick a roulette move.", 0, false, timers, 2),
        opt("TARGET SELECT", "Flick the right stick to aim the roulette\nat the next enemy that way."),
        opt("DRY SPINNING", "R1 opens the roulette with no orbs left."),
        head("SKIP POKER"), opt("HARD-ON", "Hard can be picked from the start."),
        head("MISC. & RISK"), opt("BETTER SHOP & CASINO POSES", "The shop and casino girls lean on their\ncounters in poses made for each.")};
    static const PageRow iface[] = {opt("ENEMY LIFE GAUGE", "Show the enemies' health.", 0),
        opt("HUD LAYOUT", "Where the HUD goes on a wide screen.", 0, false, huds, 4),
        opt("BUTTON PROMPTS", "The buttons shown in the game's prompts.", 0, false, prompts, 6),
        opt("SWAY BAR", "A stun gauge under each enemy's health bar.")};
    static const Page pages[] = {{"GAMEPLAY", gameplay, int(std::size(gameplay))}, {"INTERFACE", iface, int(std::size(iface))}};
    View v;
    v.aspect = float(W) / float(H);
    v.pause = atoi(argv[5]) != 0;
    v.rows = v.pause ? pauseRows : titleRows;
    v.rowCount = v.pause ? 9 : 7;
    v.cursor = atoi(argv[6]);
    v.confirmRow = atoi(argv[7]);
    v.confirmYes = false;
    v.question = "Would you really like to reset?";
    const int page = argc > 15 ? atoi(argv[15]) : -1;
    v.page = page >= 0 && page < int(std::size(pages)) ? &pages[page] : nullptr;
    v.inPage = argc > 16 && atoi(argv[16]) != 0;
    v.pageCursor = argc > 17 ? atoi(argv[17]) : 0;
    v.pageScroll = argc > 18 ? atoi(argv[18]) : 0;
    look().bracket = argc > 19 ? atoi(argv[19]) : 0;
    look().preview = argc > 20 ? atoi(argv[20]) : 1;
    std::vector<Pcsx2GsScreenItem> items;
    std::string strings;
    build(v, items, strings);
    g_screenItems = items;
    g_screenStrings = strings;
    g_screenStrings.push_back('\0');
    DrawScreen();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    // textures
    for (ImDrawList *list : dd->CmdLists)
    {
        for (const ImDrawCmd &cmd : list->CmdBuffer)
        {
            ImTextureData *tex = cmd.TexRef._TexData;
            if (!tex || !tex->Pixels) { fprintf(stderr, "no tex data\n"); continue; }
            const int tw = tex->Width, th = tex->Height, bpp = tex->BytesPerPixel;
            auto sample = [&](float u, float v, int ch) -> float {
                float x = u * tw - 0.5f, y = v * th - 0.5f;
                int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
                float fx = x - x0, fy = y - y0;
                auto at = [&](int xx, int yy) -> float {
                    xx = std::clamp(xx, 0, tw - 1); yy = std::clamp(yy, 0, th - 1);
                    const unsigned char *p = (const unsigned char *)tex->Pixels + (size_t(yy) * tw + xx) * bpp;
                    return (bpp == 4 ? p[ch] : (ch == 3 ? p[0] : 255)) / 255.0f;
                };
                return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
            };
            for (unsigned int i = 0; i < cmd.ElemCount; i += 3)
            {
                const ImDrawVert *v3[3];
                for (int k = 0; k < 3; ++k) v3[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                float area = edge(v3[0]->pos.x, v3[0]->pos.y, v3[1]->pos.x, v3[1]->pos.y, v3[2]->pos.x, v3[2]->pos.y);
                if (std::fabs(area) < 1e-9f) continue;
                int minx = std::max(0, (int)std::floor(std::min({v3[0]->pos.x, v3[1]->pos.x, v3[2]->pos.x})));
                int maxx = std::min(W - 1, (int)std::ceil(std::max({v3[0]->pos.x, v3[1]->pos.x, v3[2]->pos.x})));
                int miny = std::max(0, (int)std::floor(std::min({v3[0]->pos.y, v3[1]->pos.y, v3[2]->pos.y})));
                int maxy = std::min(H - 1, (int)std::ceil(std::max({v3[0]->pos.y, v3[1]->pos.y, v3[2]->pos.y})));
                for (int y = miny; y <= maxy; ++y)
                    for (int x = minx; x <= maxx; ++x)
                    {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float w0 = edge(v3[1]->pos.x, v3[1]->pos.y, v3[2]->pos.x, v3[2]->pos.y, px, py) / area;
                        float w1 = edge(v3[2]->pos.x, v3[2]->pos.y, v3[0]->pos.x, v3[0]->pos.y, px, py) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float col[4] = {0, 0, 0, 0};
                        for (int c = 0; c < 4; ++c)
                            for (int k = 0; k < 3; ++k)
                                col[c] += (w0 * (k == 0) + w1 * (k == 1) + w2 * (k == 2)) * ((v3[k]->col >> (8 * c)) & 255) / 255.0f;
                        const float u = w0 * v3[0]->uv.x + w1 * v3[1]->uv.x + w2 * v3[2]->uv.x;
                        const float vv = w0 * v3[0]->uv.y + w1 * v3[1]->uv.y + w2 * v3[2]->uv.y;
                        const float a = col[3] * sample(u, vv, 3);
                        float *d = &img[(size_t(y) * W + x) * 3];
                        for (int c = 0; c < 3; ++c) d[c] = d[c] * (1 - a) + col[c] * sample(u, vv, c) * a;
                    }
            }
        }
    }
    std::vector<unsigned char> out(img.size());
    for (size_t i = 0; i < img.size(); ++i) out[i] = (unsigned char)std::lround(std::clamp(img[i], 0.0f, 1.0f) * 255);
    FILE *f = fopen(outPath, "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    fprintf(stderr, "items %zu, vertices %d\n", items.size(), dd->TotalVtxCount);
    return 0;
}
