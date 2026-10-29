// The port's shortcut keys in kamite.ini and in the F2 menu. See hotkeys.h.
#include "kamite/hotkeys.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <cstdio>
#include <string>

namespace kamite::hotkeys
{
    namespace
    {
        namespace rt = ps2x::hotkeys;
        constexpr int kCount = static_cast<int>(rt::Action::Count);
        std::filesystem::path g_iniPath;
        int g_capturing = -1;    // the action a key is being captured for (GS thread)
        std::string g_status;    // the last save / capture result
        std::string g_warning;   // the last clash

        rt::Action action(int i) { return static_cast<rt::Action>(i); }
    }

    void install(const std::filesystem::path &iniPath)
    {
        g_iniPath = iniPath;
        IniFile ini;
        if (g_iniPath.empty() || !ini.load(g_iniPath))
            return;
        int read = 0;
        for (int i = 0; i < kCount; ++i)
        {
            const rt::Info &info = rt::info(action(i));
            const std::string value = ini.get("Hotkeys", info.id, "\x01");
            if (value == "\x01")
                continue;
            rt::Binding binding;
            if (rt::parse(value, binding))
            {
                rt::setBinding(action(i), binding);
                ++read;
            }
            else
                KM_WARN(Game, "hotkeys: [Hotkeys] " << info.id << "=" << value << " is not a key this build knows; "
                                                     << rt::text(info.def) << " kept");
        }
        if (read)
            KM_INFO(Game, "hotkeys: " << read << " binding(s) from kamite.ini [Hotkeys]");
    }

    bool save(std::string &status)
    {
        IniFile ini;
        if (g_iniPath.empty() || !ini.load(g_iniPath))
        {
            status = "Could not read kamite.ini.";
            return false;
        }
        for (int i = 0; i < kCount; ++i)
        {
            const rt::Binding binding = rt::binding(action(i));
            ini.set("Hotkeys", rt::info(action(i)).id, binding.bound() ? rt::text(binding) : "none");
        }
        const bool saved = ini.save();
        status = saved ? "Hotkeys saved to kamite.ini." : "Could not write kamite.ini.";
        return saved;
    }

#if defined(KAMITE_HAS_GS_MENU)
    void drawSection(const Pcsx2GsUi *ui)
    {
        if (ui->version < 4u)
        {
            ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get the hotkeys here.");
            return;
        }
        if (!ui->collapsing_header("Hotkeys", 0))
            return;
        ui->text_disabled("The port's own keys. Click one, then press the key (with Shift or Ctrl held if you want) or "
                          "pick a controller button; Esc keeps the old one. F1 (the Controls screen) stays as it is.");
        // A capture that finished: take the key. One the menu dropped (it was closed meanwhile): forget it.
        int key = 0;
        bool shift = false, ctrl = false;
        std::string gameUse;
        const bool done = g_capturing >= 0 && ps2x::menu::captured(key, shift, ctrl, gameUse);
        if (g_capturing >= 0 && !done && !ps2x::menu::capturing())
            g_capturing = -1;
        if (done)
        {
            const rt::Action target = action(g_capturing);
            g_capturing = -1;
            g_warning.clear();
            if (key != 0)
            {
                const rt::Binding binding{key, shift, ctrl};
                const rt::Action other = rt::conflict(target, binding);
                if (other != rt::Action::Count)
                    g_warning = rt::text(binding) + " is already " + rt::info(other).label + "; not changed.";
                else
                {
                    rt::setBinding(target, binding);
                    if (!gameUse.empty() && !shift && !ctrl)
                        g_warning = rt::text(binding) + " is also " + gameUse + " in the game's controls: it will do both.";
                    save(g_status);
                }
            }
        }
        for (int i = 0; i < kCount; ++i)
        {
            const rt::Info &info = rt::info(action(i));
            const rt::Binding binding = rt::binding(action(i));
            ui->push_id(i);
            char label[64];
            if (g_capturing == i)
                std::snprintf(label, sizeof(label), "press a key...##hk");
            else
                std::snprintf(label, sizeof(label), "%s##hk", binding.bound() ? rt::text(binding).c_str() : "(none)");
            ui->set_next_width(160.0f);
            ui->begin_disabled(g_capturing >= 0 && g_capturing != i);
            if (ui->button(label))
            {
                if (g_capturing == i)
                {
                    g_capturing = -1; // clicked again: never mind
                    ps2x::menu::cancelKeyCapture();
                }
                else
                {
                    g_capturing = i;
                    g_warning.clear();
                    ps2x::menu::beginKeyCapture();
                }
            }
            ui->end_disabled();
            ui->same_line();
            ui->text(info.label);
            if (binding != info.def)
            {
                ui->same_line();
                if (ui->small_button("default##hkdef"))
                {
                    rt::setBinding(action(i), info.def);
                    save(g_status);
                }
            }
            ui->pop_id();
        }
        if (g_capturing >= 0)
        {
            // Or a controller button, as the game sees it: the game layer watches the pad data for it.
            ui->text_disabled("...or a controller button:");
            int padCount = 0;
            const rt::PadName *pads = rt::padNames(padCount);
            for (int p = 0; p < padCount; ++p)
            {
                if (p % 8 != 0)
                    ui->same_line();
                ui->push_id(2000 + p);
                char padLabel[32];
                std::snprintf(padLabel, sizeof(padLabel), "%s##hkpad", pads[p].name);
                if (ui->small_button(padLabel))
                {
                    const rt::Binding binding{0, false, false, pads[p].bit};
                    const rt::Action target = action(g_capturing);
                    const rt::Action other = rt::conflict(target, binding);
                    if (other != rt::Action::Count)
                        g_warning = rt::text(binding) + " is already " + rt::info(other).label + "; not changed.";
                    else
                    {
                        rt::setBinding(target, binding);
                        g_warning = rt::text(binding) + ": the game sees the button too.";
                        save(g_status);
                    }
                    g_capturing = -1;
                    ps2x::menu::cancelKeyCapture();
                }
                ui->pop_id();
            }
        }
        if (g_capturing >= 0 && ui->small_button("No key for it (unbind)##hknone"))
        {
            rt::setBinding(action(g_capturing), rt::Binding{});
            g_capturing = -1;
            ps2x::menu::cancelKeyCapture();
            save(g_status);
        }
        if (!g_warning.empty())
            ui->text_colored(0xFF60C0FFu, g_warning.c_str());
        if (!g_status.empty())
            ui->text_disabled(g_status.c_str());
    }
#else
    void drawSection(const Pcsx2GsUi *) {}
#endif
}
