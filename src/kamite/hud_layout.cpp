// Widescreen HUD layouts (see hud_layout.h). Hooks in recomp/kamite.toml (flag "hud2d", always on: the menu
// detection runs at 4:3 too; the narrowing only when wide):
//
//   Gp_draw_line/poly3/poly4/polyFT4/polyG4 and the three sprite drawers (0x15B138..0x15DDF8): after each converts
//   its vertices to GS coordinates (x = (screen x + 0x800 - width/2) * 16, the screen width at 0x3C1180), the x of
//   the n vertices at $s0+0x10.. is narrowed. The same instruction also ends their 3D path, where $s0 is a counter
//   (0); only a $s0 inside the stack frame is the 2D vertex array. func_0015C410 (strips: health gauge, roulette dial)
//   is hooked per vertex ($a3 before it is stored).
//
//   func_002A53A8 (text glyphs and the button icons inside text): its two corners at 0x2A5704, before the offset.
//
//   cIDBase_trans (0x2AAC28, every cIDBase layout): layouts drawn by the in-game HUD widgets (isHudCaller, by the
//   caller's return address) are groups that keep to the screen edges; all other 2D (menus, pause and status
//   screens, text windows) is narrowed toward the screen centre, so it looks as on the PS2 in the middle 4:3.
//
//   cCockPlBar, cCockRader and cCockRoulette trans: the whole widget is one HUD group, including their own gauge,
//   blip and dial draws and every layout they draw.
//
// F6 writes every 2D primitive of the next frame to the log (drawer, caller, group, anchor, x before and after).
//
// A HUD group's anchor comes from its x extent in the previous frame (before narrowing): narrower than 12% of the
// screen = its own centre (button prompts over things in the 3D view); else by the third its centre is in: the
// screen's left edge, centre or right edge.
#include "kamite/hud_layout.h"
#include "runtime/ps2_hotkeys.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/roulette_bar.h"
#include "kamite/style_presets.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_aspect.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace kamite::hud
{
    namespace
    {
        enum class Layout
        {
            Anchored,
            Centered,
        };

        constexpr uint32_t kScreenWidth = 0x3C1180u;
        constexpr uint32_t kScreenYOffset = 0x586B80u;
        constexpr uint32_t kSubScrPointer = game::SubScr::kPointer; // D_003BD930: the cSubScr (pause menu and its screens)
        // The tutorial pop-up: cCockTutorial at the game object D_003BD6E8 + 0x1A20, its state byte at +0xB8 (1..4 while
        // its panel is drawn, ForwardIfStateActive_147460); decomp src/cod/001bf8c8.c.
        constexpr uint32_t kGameObject = 0x3BD6E8u, kTutorialState = 0x1A20u + 0xB8u;
        // The jukebox (the shop's radio): its layout is drawn by Entry_runHookAndDraw (cIDBase_trans returning to
        // 0x1F5EE4) over the live scene with an invisible backdrop bigger than the screen (texture 0x3800, alpha 0):
        // an in-world screen like the tutorial pop-up, not a menu (counted as one, the picture went 4:3, Oct 25).
        constexpr uint32_t kJukeBoxTransReturn = 0x001F5EE4u;
        constexpr uint32_t kPlayerBar = 0xF0000001u, kRadar = 0xF0000002u, kRoulette = 0xF0000003u;

        namespace ent = game::IDBaseEnt;
        namespace idb = game::IDBase;

        Layout g_layout = Layout::Anchored;
        std::atomic<int> g_layoutRequest{0}; // F2 menu: 1 = switch the layout at the next frame (game thread applies)
        std::atomic<int> g_layoutShown{0};   // 0 = 16:9 (anchored), 1 = 4:3, for the F2 menu

        // Menus and screens (the pause menu, options, the roulette setup...) are drawn for 4:3 and partly through
        // drawers the 2D narrowing does not reach, so their layers cannot be lined up by narrowing. A frame that
        // draws a full-screen background inside a non-HUD layout is a menu frame: then nothing is narrowed and the
        // picture is shown 4:3 (pillarboxed), or stretched with KAMITE_MENUS=stretch ([Video] MenuLayout).
        enum class MenuMode
        {
            Pillarbox,
            Stretch,
        };
        MenuMode g_menuMode = MenuMode::Pillarbox;
        std::atomic<int> g_menuModeRequest{-1}; // F2 menu: 0 = 4:3, 1 = stretch, applied at the next frame
        bool g_menuSeen = false;  // a menu background this frame
        int g_menuHold = 0;       // frames the menu state lasts without one (covers a few frames of transitions)
        bool g_menuAspect = false; // the picture is forced to 4:3 now

        void setMenuAspect(bool on)
        {
            if (on == g_menuAspect)
                return;
            g_menuAspect = on;
            ps2x::gs::setAspect(on ? ps2x::gs::Aspect::Force4_3 : ps2x::gs::Aspect::R4_3);
        }

        bool menuNow() { return g_menuSeen || g_menuHold > 0; }
        bool g_wide = false;
        float g_k = 1.0f;
        float g_edgeShare = 0.0f; // HUD edges: the 16:9 area's edges, as a share of the half width from the centre
        uint64_t g_frame = 0;

        struct Extent
        {
            float x0, x1;
        };
        std::unordered_map<uint32_t, Extent> g_seen, g_last; // group -> x extent this frame / last frame
        // The last extent each group had outside menus: a group drawn again after a menu or pop-up keeps its anchor
        // (without it the first frame back fell back to the centre, the HUD's 4:3 place).
        std::unordered_map<uint32_t, Extent> g_known;

        struct Context
        {
            bool active = false;
            uint32_t key = 0;
            float anchor = 0.0f;
        };
        Context g_ctx; // what the draws use

        enum class Region
        {
            None,
            Forced,  // everything inside is one group
            Inherit, // own draws take the group of the layout entry drawn last
        };
        Region g_region = Region::None;
        Context g_regionCtx, g_lastEntry;
        bool g_inIdTrans = false;
        uint32_t g_idCaller = 0u; // the return address of the cIDBase_trans being drawn
        uint32_t g_curLayout = 0u; // the cIDBase being drawn (every caller)

        struct Loose
        {
            uint32_t prims = 0;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        };
        std::unordered_map<uint32_t, Loose> g_loose; // anchored: draws outside any group, by caller

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & (PS2_RAM_SIZE - 1u)), sizeof(value));
            return value;
        }

        void write32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & (PS2_RAM_SIZE - 1u)), &value, sizeof(value));
        }

        // The tutorial pop-up is up (and no sub screen is open over it).
        bool g_subScreenOpen = false;
        bool tutorialUp(const uint8_t *rdram)
        {
            const uint32_t game = read32(rdram, kGameObject);
            if (game < 0x100000u || game >= PS2_RAM_SIZE - 0x2000u)
                return false;
            const int8_t state = static_cast<int8_t>(rdram[(game + kTutorialState) & (PS2_RAM_SIZE - 1u)]);
            return state > 0 && state < 5;
        }

        float screenWidth(const uint8_t *rdram)
        {
            const int32_t width = static_cast<int32_t>(read32(rdram, kScreenWidth));
            return width > 0 && width < 4096 ? static_cast<float>(width) : 640.0f;
        }

        // GS x (12.4 fixed, with the 2048 - width/2 offset) <-> screen x.
        int32_t gsOffset(const uint8_t *rdram)
        {
            const int32_t width = static_cast<int32_t>(read32(rdram, kScreenWidth));
            return (0x800 - width / 2) * 16;
        }

        // Small groups (button prompts, icons, often placed on things in the 3D view) keep their own centre. Others
        // are narrowed toward the screen edge or centre of the third they are in: every group in one third gets the
        // same mapping, so layouts drawn over each other (a menu's background and its highlight) stay lined up.
        // The HUD's edges: at 16:9 the screen's edges; wider screens keep the HUD at the edges of a 16:9 area in the
        // middle; narrower than 16:9 (and the 4:3 layout) puts the HUD where the PS2 had it.
        float anchorFromExtent(float x0, float x1, float width)
        {
            const float span = x1 - x0, centre = 0.5f * (x0 + x1);
            if (span < 0.12f * width)
                return centre;
            if (g_layout != Layout::Anchored || g_edgeShare <= 0.0f)
                return 0.5f * width;
            const float left = 0.5f * width * (1.0f - g_edgeShare);
            // A group that starts near one screen edge belongs to it, however far it reaches: the boss bar's portrait
            // sits at the left edge (an edge draw, never narrowed, so not in the extent) and its bar and background
            // run so far right that their centre is in the middle third, which put them at the 4:3 position while the
            // portrait stayed at the screen's edge (Thinso, Oct 27).
            const bool hugsLeft = x0 < 0.15f * width, hugsRight = x1 > 0.85f * width;
            if (hugsLeft != hugsRight)
                return hugsLeft ? left : width - left;
            if (centre < width / 3.0f)
                return left;
            if (centre > 2.0f * width / 3.0f)
                return width - left;
            return 0.5f * width;
        }

        float anchorFor(uint32_t key, float fallback, float width)
        {
            auto last = g_last.find(key);
            if (last != g_last.end())
                return anchorFromExtent(last->second.x0, last->second.x1, width);
            auto seen = g_seen.find(key);
            if (seen != g_seen.end())
                return anchorFromExtent(seen->second.x0, seen->second.x1, width);
            auto known = g_known.find(key);
            if (known != g_known.end())
                return anchorFromExtent(known->second.x0, known->second.x1, width);
            return fallback;
        }

        void extend(uint32_t key, float x0, float x1)
        {
            auto [it, fresh] = g_seen.try_emplace(key, Extent{x0, x1});
            if (!fresh)
            {
                it->second.x0 = std::min(it->second.x0, x0);
                it->second.x1 = std::max(it->second.x1, x1);
            }
        }

        void noteLoose(uint32_t caller, float x0, float x1, float y0, float y1)
        {
            Loose &loose = g_loose[caller];
            ++loose.prims;
            loose.x0 = std::min(loose.x0, x0);
            loose.x1 = std::max(loose.x1, x1);
            loose.y0 = std::min(loose.y0, y0);
            loose.y1 = std::max(loose.y1, y1);
        }

        // F6: every 2D primitive of one frame written to the log (which drawer, caller, group, anchor, x before/after).
        bool g_dumpArmed = false, g_dumping = false;
        uint32_t g_entry = 0; // the cIDBase entry being drawn
        std::vector<std::string> g_dump;

        void dump(const char *line)
        {
            if (g_dumping && g_dump.size() < 3000u)
                g_dump.emplace_back(line);
        }

        void dumpPrim(const char *tag, uint32_t caller, float x0, float x1, float y0, float y1, const char *what, float anchor)
        {
            if (!g_dumping || g_dump.size() >= 3000u)
                return;
            char line[200];
            const float n0 = std::isnan(anchor) ? x0 : anchor + (x0 - anchor) * g_k;
            const float n1 = std::isnan(anchor) ? x1 : anchor + (x1 - anchor) * g_k;
            std::snprintf(line, sizeof(line), "%-6s ra %08X grp %08X ent %08X  x %6.1f..%6.1f -> %6.1f..%6.1f  y %6.1f..%6.1f  %s a=%.1f",
                          tag, caller, g_ctx.active ? g_ctx.key : 0u, g_inIdTrans ? g_entry : 0u, x0, x1, n0, n1, y0, y1, what,
                          std::isnan(anchor) ? -1.0f : anchor);
            g_dump.emplace_back(line);
        }

        // n vertices, x at address + i*16 (y at +4), in GS units: screen*16 + offset (offset 0 for the font drawer).
        void narrowVertices(uint8_t *rdram, uint32_t address, int n, uint32_t caller, const char *tag, bool withOffset = true)
        {
            const float width = screenWidth(rdram);
            const int32_t offset = withOffset ? gsOffset(rdram) : 0;
            const int32_t yOffset = withOffset ? static_cast<int32_t>(read32(rdram, kScreenYOffset)) * 16 : 0;
            float xs[4], x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int i = 0; i < n; ++i)
            {
                xs[i] = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i)) - offset) / 16.0f;
                const float y = static_cast<float>(static_cast<int32_t>(read32(rdram, address + 16u * i + 4u)) - yOffset) / 16.0f;
                x0 = std::min(x0, xs[i]);
                x1 = std::max(x1, xs[i]);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
            if (g_inIdTrans)
                roulette_bar::onDraw(g_curLayout, g_entry, x0, x1, !withOffset);
            const float none = std::nanf("");
            if (n >= 2 && x1 - x0 >= 0.9f * width)
            {
                // Full height too: letterbox bars in cutscenes are full width but thin.
                const float height = 448.0f; // the game's 2D screen (0..448, NTSC)
                if (g_inIdTrans && !g_ctx.active && !g_menuSeen && y1 - y0 >= 0.8f * height &&
                    g_idCaller != kJukeBoxTransReturn && (g_subScreenOpen || !tutorialUp(rdram)))
                {
                    g_menuSeen = true; // a menu's background (fades are drawn outside any layout)
                    if (g_wide && g_menuMode == MenuMode::Pillarbox)
                        setMenuAspect(true);
                }
                dumpPrim(tag, caller, x0, x1, y0, y1, "fullwidth", none);
                return; // full-screen quad: backgrounds, fades
            }
            if (!g_wide || menuNow())
            {
                dumpPrim(tag, caller, x0, x1, y0, y1, "menu", none);
                return;
            }
            // Screen-edge overlays (the low-health red corners): drawn from the screen's edge, they belong to the edge,
            // so they stay stretched with it; narrowed they left angled stripes short of the corners.
            if (x0 <= 1.0f || x1 >= width - 1.0f)
            {
                dumpPrim(tag, caller, x0, x1, y0, y1, "edge", none);
                return;
            }
            float anchor;
            if (g_ctx.active)
            {
                extend(g_ctx.key, x0, x1);
                anchor = g_ctx.anchor;
            }
            else
            {
                if (!g_inIdTrans)
                    noteLoose(caller, x0, x1, y0, y1);
                anchor = 0.5f * width; // everything else: the PS2's 4:3 layout, in the middle
            }
            dumpPrim(tag, caller, x0, x1, y0, y1, g_ctx.active ? "hud" : "centred", anchor);
            (void)none;
            for (int i = 0; i < n; ++i)
            {
                const float x = anchor + (xs[i] - anchor) * g_k;
                write32(rdram, address + 16u * i, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 16.0f)) + offset));
            }
        }

        // After the vertex loop of a Gp_draw_* function: n vertices at $s0+0x10, the caller's $ra saved at sp+raSlot.
        template <int N, uint32_t RaSlot>
        void gpVertices(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t sp = GPR_U32(ctx, 29), s0 = GPR_U32(ctx, 16);
            const int32_t delta = static_cast<int32_t>(s0 - sp);
            if (delta < -0x20 || delta > 0x100)
                return; // the 3D path
            narrowVertices(rdram, s0 + 0x10u, N, read32(rdram, sp + RaSlot), N == 2 ? "sprite" : N == 3 ? "poly3" : "quad");
        }

        // func_002A53A8 (text glyphs and inline button icons) at 0x2A5704: its two corners converted (screen*16, the
        // offset not added yet) at sp+0x20 and sp+0x30; caller's $ra at sp+0x40.
        void fontSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t sp = GPR_U32(ctx, 29);
            narrowVertices(rdram, sp + 0x20u, 2, read32(rdram, sp + 0x40u), "glyph", false);
        }

        // func_0015C410 before "sw $a3,0x40($sp)": one strip vertex's GS x in $a3.
        void stripVertex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_wide || menuNow())
                return;
            const float width = screenWidth(rdram);
            const int32_t offset = gsOffset(rdram);
            const float xs = static_cast<float>(GPR_S32(ctx, 7) - offset) / 16.0f;
            const uint32_t caller = read32(rdram, GPR_U32(ctx, 29) + 0x70u);
            float anchor;
            if (g_ctx.active)
            {
                extend(g_ctx.key, xs, xs);
                anchor = g_ctx.anchor;
            }
            else
            {
                if (!g_inIdTrans)
                    noteLoose(caller, xs, xs, 0.0f, 0.0f);
                anchor = 0.5f * width;
            }
            dumpPrim("strip", caller, xs, xs, 0.0f, 0.0f, g_ctx.active ? "hud" : "centred", anchor);
            const float x = anchor + (xs - anchor) * g_k;
            SET_GPR_S32(ctx, 7, static_cast<int32_t>(std::lround(x * 16.0f)) + offset);
        }

        // Return addresses of the cIDBase_trans calls in the HUD widgets' trans functions: cCockActButton (button
        // prompts, 0x13C168), cCockBossBar (0x13CF20), cCockItem (0x13E6E0), cCockLastBossBar (0x13EBA0),
        // cCockMiddleBoss (0x140638). cCockPlBar, cCockRader and cCockRoulette are regions of their own.
        bool isHudCaller(uint32_t ra)
        {
            switch (ra)
            {
            case 0x0013C18Cu:
            case 0x0013CFA0u:
            case 0x0013E728u:
            case 0x0013E73Cu:
            case 0x0013EC38u:
            case 0x001406CCu:
            case 0x001406FCu:
                return true;
            default:
                return false;
            }
        }

        // cIDBase_trans at the start of its entry loop: $s2 = the layout. One layout is one group: its entries (icons,
        // labels, frames) are designed around each other.
        void idBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_inIdTrans = true;
            g_curLayout = GPR_U32(ctx, 18) & 0x01FFFFFFu;
            g_idCaller = read32(rdram, GPR_U32(ctx, 29) + 0x40u);
            if (g_dumping)
            {
                // The F6 dump also lists every entry of each layout drawn (hidden ones too): index, kind (+0x88), flags
                // (+0x2C; 0x08000000 = not drawn), colour set (+0x8B), message id (+0x90), draw position (+0x50/+0x54),
                // for finding a spare text entry to carry a hint or an indicator.
                const int32_t count = static_cast<int32_t>(read32(rdram, g_curLayout + idb::kEntNum));
                const uint32_t entries = read32(rdram, g_curLayout + idb::kEnt);
                char line[160];
                std::snprintf(line, sizeof(line), "layout %08X (caller %08X): %d entries at %08X", g_curLayout, g_idCaller, count, entries);
                dump(line);
                for (int32_t i = 0; entries != 0u && i < count && i < 512; ++i)
                {
                    const uint32_t e = entries + ent::kSize * static_cast<uint32_t>(i);
                    float x, y;
                    const uint32_t xb = read32(rdram, e + ent::kDrawX), yb = read32(rdram, e + ent::kDrawY);
                    std::memcpy(&x, &xb, sizeof(x));
                    std::memcpy(&y, &yb, sizeof(y));
                    std::snprintf(line, sizeof(line), "  entry %3d %08X kind %u flags %08X colour %u msg %04X pos %.1f,%.1f", i, e,
                                  rdram[(e + ent::kKind) & 0x01FFFFFFu], read32(rdram, e + ent::kFlags), rdram[(e + ent::kColourSet) & 0x01FFFFFFu],
                                  read32(rdram, e + ent::kMsg) & 0xFFFFu, x, y);
                    dump(line);
                }
            }
            if (g_region == Region::Forced)
                return; // g_ctx is the region's
            // Only the in-game HUD widgets (by who called cIDBase_trans) keep to the screen edges; every other layout
            // (menus, the pause and status screens) is drawn as on the PS2, in the middle 4:3 of the screen.
            const uint32_t caller = read32(rdram, GPR_U32(ctx, 29) + 0x40u);
            if (!isHudCaller(caller))
            {
                g_ctx = Context{};
                return;
            }
            const uint32_t layout = GPR_U32(ctx, 18) & 0x01FFFFFFu;
            const float width = screenWidth(rdram);
            float fallback = 0.5f * width;
            if (g_last.find(layout) == g_last.end() && g_seen.find(layout) == g_seen.end() && g_known.find(layout) == g_known.end())
            {
                const int32_t count = static_cast<int32_t>(read32(rdram, layout + idb::kEntNum));
                const uint32_t entries = read32(rdram, layout + idb::kEnt);
                float x0 = 1e9f, x1 = -1e9f;
                for (int32_t i = 0; entries != 0u && i < count && i < 512; ++i)
                {
                    float x;
                    const uint32_t bits = read32(rdram, entries + ent::kSize * static_cast<uint32_t>(i) + ent::kDrawX);
                    std::memcpy(&x, &bits, sizeof(x));
                    if (!std::isfinite(x) || x < -width || x > 2.0f * width)
                        continue;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                }
                if (x0 <= x1)
                    fallback = anchorFromExtent(x0, x1, width);
            }
            g_ctx = Context{true, layout, anchorFor(layout, fallback, width)};
            g_lastEntry = g_ctx;
        }

        // cIDBase_trans, before an entry is drawn: $a1 = the entry (noted for the F6 dump; the roulette's hint bar is
        // laid out again here, roulette_bar.h).
        bool g_entryOverride = false; // g_ctx replaced for one entry (the style indicator); put back at the next
        Context g_savedCtx;

        void idEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_entry = GPR_U32(ctx, 5) & 0x01FFFFFFu;
            style_presets::noteEntry(g_entry); // the Techniques screen's style list draws arrows for its own entry
            roulette_bar::onEntry(rdram, g_curLayout, g_entry);
            if (g_entryOverride)
            {
                g_ctx = g_savedCtx;
                g_entryOverride = false;
            }
            // The style indicator rides in the money display's layout but belongs with the level meter at the left
            // edge: its own group, anchored there, so its draws neither follow the money nor widen its extent.
            if (style_presets::onHudEntry(g_entry) && g_wide && !menuNow())
            {
                const float width = screenWidth(rdram);
                g_savedCtx = g_ctx;
                // The left-edge anchor the level meter gets (a wide group in the left third), not this text's own
                // narrow extent (which would hold it where it is while the meter moves).
                g_ctx = Context{true, g_entry, anchorFromExtent(0.0f, 0.2f * width, width)};
                g_entryOverride = true;
            }
        }

        void idEnd(uint8_t *, R5900Context *, PS2Runtime *)
        {
            style_presets::noteEntry(0u);
            g_entryOverride = false;
            g_idCaller = 0u;
            g_inIdTrans = false;
            g_ctx = g_region == Region::Forced ? g_regionCtx : g_region == Region::Inherit ? g_lastEntry : Context{};
        }

        template <uint32_t Key, Region Mode>
        void regionBegin(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            g_region = Mode;
            g_lastEntry = Context{};
            if (Mode == Region::Forced)
            {
                const float width = screenWidth(rdram);
                g_regionCtx = Context{true, Key, anchorFor(Key, 0.5f * width, width)};
                g_ctx = g_regionCtx;
            }
            else
                g_ctx = Context{};
        }

        void regionEnd(uint8_t *, R5900Context *, PS2Runtime *)
        {
            g_region = Region::None;
            g_ctx = Context{};
            g_lastEntry = Context{};
        }

        const char *layoutName(Layout layout)
        {
            switch (layout)
            {
            case Layout::Anchored:
                return "16:9 (HUD at the edges)";
            default:
                return "4:3 (HUD where the PS2 had it)";
            }
        }

        Layout parseLayout(const char *text)
        {
            if (!text)
                return Layout::Anchored;
            std::string value(text);
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (value == "4:3" || value == "classic" || value == "centered" || value == "centred" || value == "center" ||
                value == "centre")
                return Layout::Centered;
            return Layout::Anchored; // "16:9", "wide", and the old "anchored" / "off"
        }

        void reportLoose()
        {
            if (g_loose.empty())
                return;
            std::vector<std::pair<uint32_t, Loose>> rows(g_loose.begin(), g_loose.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b)
                      { return a.second.prims > b.second.prims; });
            std::ostringstream out;
            out << "HUD (anchored): 2D drawn outside any layout (centred), by caller (x,y range):";
            const size_t shown = std::min<size_t>(rows.size(), 16);
            for (size_t i = 0; i < shown; ++i)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "\n    %08X %6u prims  x %.0f..%.0f  y %.0f..%.0f", rows[i].first,
                              rows[i].second.prims, rows[i].second.x0, rows[i].second.x1, rows[i].second.y0, rows[i].second.y1);
                out << line;
            }
            KM_INFO(Game, out.str());
            g_loose.clear();
        }
    }

    // All text and gauges go through the 2D narrowing: the pnach's own font and gauge narrowing is not used.
    bool narrowFont() { return false; }
    bool narrowGauge() { return false; }

    bool menuActive() { return menuNow(); }

    void onFrame(uint8_t *rdram)
    {
        roulette_bar::onFrame();
        // The pause menu's own screens (Techniques, the move list...) can be drawn over the 3D view with no
        // full-screen background: an open sub screen counts as a menu too (cSubScr D_003BD930, +0x48 the open screen's
        // kind, -1 when closed; decomp include/godhand/cSubScr.h). Narrowed, their layers came apart (the Techniques
        // screen's RETURN panel and its highlight, Oct 25).
        bool subScreen = false, tutorial = false;
        if (rdram)
        {
            const uint32_t subScr = read32(rdram, kSubScrPointer);
            if (subScr >= 0x100000u && subScr < PS2_RAM_SIZE - 0x100u)
                subScreen = static_cast<int32_t>(read32(rdram, subScr + game::SubScr::kKind)) >= 0;
            // A tutorial pop-up is drawn over the game with a full-screen backing: it is part of the game view, not a
            // menu (counted as one, the picture went 4:3 and back and the health block landed in its 4:3 place, Oct 25).
            tutorial = tutorialUp(rdram);
        }
        g_subScreenOpen = subScreen;
        if (tutorial && !subScreen)
            g_menuSeen = false;
        g_menuHold = (g_menuSeen || subScreen) ? 8 : std::max(0, g_menuHold - 1);
        g_menuSeen = false;
        if (g_wide)
            setMenuAspect(g_menuHold > 0 && g_menuMode == MenuMode::Pillarbox);
        if (!g_wide)
        {
            g_ctx = Context{};
            g_region = Region::None;
            g_inIdTrans = false;
            return;
        }
        if (!menuNow())
            for (const auto &[key, extent] : g_seen)
                g_known[key] = extent;
        g_last.swap(g_seen);
        g_seen.clear();
        g_ctx = Context{};
        g_region = Region::None;
        g_inIdTrans = false;
        if (g_dumping)
        {
            std::ostringstream out;
            out << "HUD F6 dump (" << layoutName(g_layout) << ", k " << g_k << "): " << g_dump.size() << " 2D primitives";
            for (const std::string &line : g_dump)
                out << "\n    " << line;
            KM_INFO(Game, out.str());
            g_dump.clear();
            g_dumping = false;
        }
        if (g_dumpArmed)
        {
            g_dumpArmed = false;
            g_dumping = true;
        }
        if (ps2x::hotkeys::take(ps2x::hotkeys::Action::Dump2D))
            g_dumpArmed = true;
        if (ps2x::hotkeys::take(ps2x::hotkeys::Action::HudLayout) || g_layoutRequest.exchange(0) != 0)
        {
            g_layout = g_layout == Layout::Anchored ? Layout::Centered : Layout::Anchored;
            g_loose.clear();
            KM_INFO(Game, "HUD layout switched: " << layoutName(g_layout));
        }
        g_layoutShown.store(g_layout == Layout::Anchored ? 0 : 1, std::memory_order_relaxed);
        if (const int mode = g_menuModeRequest.exchange(-1); mode >= 0)
            g_menuMode = mode == 1 ? MenuMode::Stretch : MenuMode::Pillarbox;
        if (++g_frame % 600u == 0u && g_layout == Layout::Anchored)
            reportLoose();
    }

    void install(bool wide, float k)
    {
        g_wide = wide;
        g_k = k;
        ps2xSetPatchFlag("hud2d", true);
        // k = (4/3) / aspect; the 16:9 area takes (16/9) / aspect of the width. Below 16:9 the HUD has no edges.
        const float aspect = (4.0f / 3.0f) / k;
        g_edgeShare = aspect >= 16.0f / 9.0f - 0.01f ? std::min(1.0f, (16.0f / 9.0f) / aspect) : 0.0f;
        g_layout = parseLayout(std::getenv("KAMITE_HUD"));
        {
            const char *menus = std::getenv("KAMITE_MENUS");
            std::string value = menus ? menus : "";
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            g_menuMode = value == "stretch" || value == "stretched" ? MenuMode::Stretch : MenuMode::Pillarbox;
        }
        ps2xRegisterHostHook("km_hud_line", &gpVertices<2, 0x70u>);
        ps2xRegisterHostHook("km_hud_poly3", &gpVertices<3, 0x80u>);
        ps2xRegisterHostHook("km_hud_poly4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("km_hud_polyft4", &gpVertices<4, 0xA0u>);
        ps2xRegisterHostHook("km_hud_polyg4", &gpVertices<4, 0x90u>);
        ps2xRegisterHostHook("km_hud_sprite_d168", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("km_hud_sprite_d6e8", &gpVertices<2, 0x40u>);
        ps2xRegisterHostHook("km_hud_sprite_db30", &gpVertices<2, 0x30u>);
        ps2xRegisterHostHook("km_hud_strip", &stripVertex);
        ps2xRegisterHostHook("km_hud_font_sprite", &fontSprite);
        ps2xRegisterHostHook("km_hud_id_begin", &idBegin);
        ps2xRegisterHostHook("km_hud_id_entry", &idEntry);
        ps2xRegisterHostHook("km_hud_id_end", &idEnd);
        ps2xRegisterHostHook("km_hud_plbar_begin", &regionBegin<kPlayerBar, Region::Forced>);
        ps2xRegisterHostHook("km_hud_radar_begin", &regionBegin<kRadar, Region::Forced>);
        ps2xRegisterHostHook("km_hud_roulette_begin", &regionBegin<kRoulette, Region::Forced>);
        ps2xRegisterHostHook("km_hud_region_end", &regionEnd);
        if (wide)
            KM_INFO(Game, "HUD layout: " << layoutName(g_layout) << " (the HUD layout hotkey switches 16:9 / 4:3; the 2D dump hotkey writes one frame's 2D to the log)");
    }

    int layoutIndex()
    {
        return g_layoutShown.load(std::memory_order_relaxed);
    }

    void requestLayout(int index)
    {
        if (layoutIndex() != (index ? 1 : 0))
            g_layoutRequest.store(1, std::memory_order_relaxed);
    }

    int menuModeIndex()
    {
        return g_menuMode == MenuMode::Stretch ? 1 : 0;
    }

    void requestMenuMode(int index)
    {
        g_menuModeRequest.store(index ? 1 : 0, std::memory_order_relaxed);
    }
}
