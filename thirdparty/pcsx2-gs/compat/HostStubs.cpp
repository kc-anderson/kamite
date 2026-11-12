// Host/VM functions PCSX2's GS calls, provided by the port instead of the rest of PCSX2 (docs/GS_PCSX2.md, phase 1).
// Most are empty: no OSD, no dumps, no video capture, no texture replacement yet. PCSX2 is GPL-3; so is this file.
#include "pcsx2gs_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>

#include "Config.h"
#include "Counters.h"
#include "GS.h"
#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "common/StringUtil.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "R5900.h"
#include "VMManager.h"
#include "common/CrashHandler.h"
#include "common/Image.h"
#include "imgui.h"
#include "imgui_internal.h" // ImGui::RegisterUserTexture (the screen's images)
#include "imgui_freetype.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
    WindowInfo g_window;
    double g_verticalFrequency = 59.94;

    // On-screen text (set_osd), drawn by ImGuiManager::RenderOSD on the GS thread.
    std::mutex g_osdMutex;
    std::string g_osdTop;
    std::string g_osdBottom;
    uint32_t g_osdTopRgba = 0;
    float g_osdTopAlpha = 1.0f;
    ImFont *g_osdFont = nullptr;

    std::string g_textureSerial; // SetTextureFolder: the pack folder's name

    // In-game menu (set_menu / menu_input): drawn by RenderOSD on the GS thread, input applied in NewFrame.
    std::mutex g_menuMutex;
    Pcsx2GsMenuFn g_menuDraw = nullptr;
    void *g_menuUser = nullptr;
    Pcsx2GsMenuInput g_menuInput{};
    float g_menuWheel = 0.0f;

    // Overlay lines (set_overlay, any thread -> RenderOSD on the GS thread). pcsx2gs (Kamite): the hitbox overlay.
    std::mutex g_overlayMutex;
    std::vector<float> g_overlayLines;      // x0 y0 x1 y1 per line, GS pixels
    std::vector<uint32_t> g_overlayColors;  // per line, R in the low byte
    std::vector<float> g_overlayTextXY;     // x y per label, GS pixels (guarded by g_overlayMutex too)
    std::vector<uint32_t> g_overlayTextColors;
    std::vector<std::string> g_overlayTexts;
    float g_sceneOffset[2] = {2048.0f - 320.0f, 2048.0f - 224.0f}; // GS thread: the scene draws' XYOFFSET, pixels
    float g_presentRect[4] = {};             // GS thread: the frame's place in the window at the last present
    float g_presentSize[2] = {640.0f, 448.0f}; // GS thread: the frame's size in GS pixels

    // The port's own screen (set_screen, any thread -> RenderOSD on the GS thread): the OPTIONS screen's text.
    std::mutex g_screenMutex;
    std::vector<Pcsx2GsScreenItem> g_screenItems;
    std::string g_screenStrings;  // the items' strings (ends with a 0)
    uint32_t g_screenStale = 0;   // presents since the list was last sent
    constexpr uint32_t kScreenStaleLimit = 8; // a screen gone without clearing its list
    struct ScreenFont
    {
        ImFont *font = nullptr;
        float capRatio = 0.7f; // cap height / font size ('H')
    };
    std::vector<std::pair<std::string, ScreenFont>> g_screenFonts; // GS thread, by path (font nullptr = failed)
    std::vector<std::pair<std::string, ImTextureData *>> g_screenImages; // GS thread, by path (nullptr = failed)

    int UiBeginWindow(const char *title, int *open, float width, float height)
    {
        // Text and window follow the window's size (sized for ~900 lines; never larger than the screen).
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float scale = std::clamp(display.y / 900.0f, 0.7f, 1.8f);
        ImGui::GetStyle().FontScaleMain = scale;
        width = std::min(width * scale, display.x - 24.0f);
        height = std::min(height * scale, display.y - 24.0f);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool keep = open ? *open != 0 : true;
        const bool visible = ImGui::Begin(title, open ? &keep : nullptr, ImGuiWindowFlags_NoCollapse);
        if (open)
            *open = keep ? 1 : 0;
        return visible ? 1 : 0;
    }
    void UiEndWindow() { ImGui::End(); }
    int UiBeginTabBar(const char *id) { return ImGui::BeginTabBar(id) ? 1 : 0; }
    void UiEndTabBar() { ImGui::EndTabBar(); }
    int UiBeginTab(const char *label) { return ImGui::BeginTabItem(label) ? 1 : 0; }
    void UiEndTab() { ImGui::EndTabItem(); }
    void UiText(const char *text) { ImGui::TextUnformatted(text ? text : ""); }
    void UiTextWrapped(const char *text) { ImGui::TextWrapped("%s", text ? text : ""); }
    void UiTextDisabled(const char *text) { ImGui::TextDisabled("%s", text ? text : ""); }
    void UiTextColored(uint32_t rgba, const char *text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rgba));
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopStyleColor();
    }
    void UiHeading(const char *text)
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopFont();
    }
    int UiCheckbox(const char *label, int *value)
    {
        bool checked = value && *value != 0;
        const bool changed = ImGui::Checkbox(label, &checked);
        if (value)
            *value = checked ? 1 : 0;
        return changed ? 1 : 0;
    }
    int UiButton(const char *label) { return ImGui::Button(label) ? 1 : 0; }
    void UiSameLine() { ImGui::SameLine(); }
    void UiSeparator() { ImGui::Separator(); }
    void UiSpacing() { ImGui::Spacing(); }
    int UiBeginChild(const char *id, float width, float height)
    {
        return ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders) ? 1 : 0;
    }
    void UiEndChild() { ImGui::EndChild(); }
    void UiPushId(int id) { ImGui::PushID(id); }
    void UiPopId() { ImGui::PopID(); }
    int UiCombo(const char *label, int *current, const char *const *items, int count)
    {
        int value = current ? *current : 0;
        const bool changed = ImGui::Combo(label, &value, items, count);
        if (current)
            *current = value;
        return changed ? 1 : 0;
    }
    int UiRadio(const char *label, int active) { return ImGui::RadioButton(label, active != 0) ? 1 : 0; }
    int UiSmallButton(const char *label) { return ImGui::SmallButton(label) ? 1 : 0; }
    void UiSeparatorText(const char *text) { ImGui::SeparatorText(text ? text : ""); }
    void UiIndent() { ImGui::Indent(); }
    void UiUnindent() { ImGui::Unindent(); }
    void UiSetNextWidth(float width)
    {
        if (width > 0.0f)
            ImGui::SetNextItemWidth(width * ImGui::GetStyle().FontScaleMain);
    }
    void UiBeginDisabled(int disabled) { ImGui::BeginDisabled(disabled != 0); }
    void UiEndDisabled() { ImGui::EndDisabled(); }
    // A slider also steps with the mouse wheel over it and with left/right (keys or d-pad) while it has the focus:
    // one step is the last shown digit's 5 (5 at "%.0f" is 1), Shift ten times that, Ctrl a tenth (K, Oct 26).
    int UiSliderFloat(const char *label, float *value, float min, float max, const char *format)
    {
        const char *fmt = format ? format : "%.2f";
        float v = value ? *value : 0.0f;
        bool changed = ImGui::SliderFloat(label, &v, min, max, fmt);
        if (!ImGui::IsItemActive() && !ImGui::IsItemDeactivated())
        {
            int decimals = 2;
            if (const char *dot = std::strchr(fmt, '.'))
                decimals = std::max(0, std::atoi(dot + 1));
            float step = decimals == 0 ? 1.0f : 5.0f * std::pow(10.0f, static_cast<float>(-decimals));
            const ImGuiIO &io = ImGui::GetIO();
            if (io.KeyShift)
                step *= 10.0f;
            if (io.KeyCtrl)
                step *= 0.1f;
            int ticks = 0;
            if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY) && io.MouseWheel != 0.0f)
                ticks += io.MouseWheel > 0.0f ? 1 : -1;
            if (ImGui::IsItemFocused())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight))
                    ++ticks;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft))
                    --ticks;
            }
            if (ticks != 0)
            {
                const float stepped = std::clamp(std::round((v + ticks * step) / step) * step, min, max);
                if (stepped != v)
                {
                    v = stepped;
                    changed = true;
                }
            }
        }
        if (value)
            *value = v;
        return changed ? 1 : 0;
    }
    int UiIsItemActive() { return ImGui::IsItemActive() ? 1 : 0; }
    int UiCollapsingHeader(const char *label, int defaultOpen)
    {
        return ImGui::CollapsingHeader(label ? label : "", defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0) ? 1 : 0;
    }
    int UiSliderInt(const char *label, int *value, int min, int max, const char *format)
    {
        int v = value ? *value : 0;
        const bool changed = ImGui::SliderInt(label, &v, min, max, format ? format : "%d");
        if (value)
            *value = v;
        return changed ? 1 : 0;
    }

    const Pcsx2GsUi g_ui = {4u, UiBeginWindow, UiEndWindow, UiBeginTabBar, UiEndTabBar, UiBeginTab, UiEndTab, UiText,
                            UiTextWrapped, UiTextDisabled, UiTextColored, UiHeading, UiCheckbox, UiButton, UiSameLine,
                            UiSeparator, UiSpacing, UiBeginChild, UiEndChild, UiPushId, UiPopId, UiCombo, UiRadio,
                            UiSmallButton, UiSeparatorText, UiIndent, UiUnindent, UiSetNextWidth, UiBeginDisabled,
                            UiEndDisabled, UiSliderFloat, UiIsItemActive, UiCollapsingHeader, UiSliderInt};

    // The menu's look: ImGui's dark style with the launcher's red.
    void ApplyMenuStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.WindowPadding = ImVec2(16.0f, 14.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        ImVec4 *colors = style.Colors;
        const ImVec4 red(0.79f, 0.12f, 0.09f, 1.0f), redHot(0.89f, 0.20f, 0.16f, 1.0f), redDark(0.45f, 0.07f, 0.05f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.94f);
        colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.06f, 0.05f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = redDark;
        colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.10f, 0.09f, 1.0f);
        colors[ImGuiCol_TabHovered] = redHot;
        colors[ImGuiCol_TabSelected] = red;
        colors[ImGuiCol_Button] = ImVec4(0.30f, 0.31f, 0.33f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = redHot;
        colors[ImGuiCol_ButtonActive] = red;
        colors[ImGuiCol_CheckMark] = redHot;
        colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.12f, 0.11f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.30f, 0.09f, 0.07f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = redHot;
        colors[ImGuiCol_NavCursor] = ImVec4(0.91f, 0.75f, 0.16f, 1.0f);
    }

    // The latest menu input as ImGui events (ImGui drops the ones that repeat the current state).
    void ApplyMenuInput()
    {
        Pcsx2GsMenuInput input;
        float wheel;
        bool active;
        {
            std::lock_guard<std::mutex> lock(g_menuMutex);
            input = g_menuInput;
            wheel = g_menuWheel;
            g_menuWheel = 0.0f;
            active = g_menuDraw != nullptr;
        }
        ImGuiIO &io = ImGui::GetIO();
        if (!active)
            input = Pcsx2GsMenuInput{-FLT_MAX, -FLT_MAX};
        io.AddMousePosEvent(input.mouse_x, input.mouse_y);
        for (int button = 0; button < 3; ++button)
            io.AddMouseButtonEvent(button, (input.mouse_buttons >> button) & 1u);
        if (wheel != 0.0f)
            io.AddMouseWheelEvent(0.0f, wheel);
        const auto key = [&](ImGuiKey imguiKey, uint32_t bits, uint32_t bit) { io.AddKeyEvent(imguiKey, (bits & bit) != 0u); };
        key(ImGuiKey_UpArrow, input.keys, PCSX2GS_KEY_UP);
        key(ImGuiKey_DownArrow, input.keys, PCSX2GS_KEY_DOWN);
        key(ImGuiKey_LeftArrow, input.keys, PCSX2GS_KEY_LEFT);
        key(ImGuiKey_RightArrow, input.keys, PCSX2GS_KEY_RIGHT);
        key(ImGuiKey_Enter, input.keys, PCSX2GS_KEY_ENTER);
        key(ImGuiKey_Space, input.keys, PCSX2GS_KEY_SPACE);
        key(ImGuiKey_Escape, input.keys, PCSX2GS_KEY_ESCAPE);
        key(ImGuiKey_Tab, input.keys, PCSX2GS_KEY_TAB);
        key(ImGuiMod_Shift, input.keys, PCSX2GS_KEY_SHIFT);
        key(ImGuiMod_Ctrl, input.keys, PCSX2GS_KEY_CTRL);
        key(ImGuiKey_GamepadDpadUp, input.pad, PCSX2GS_PAD_UP);
        key(ImGuiKey_GamepadDpadDown, input.pad, PCSX2GS_PAD_DOWN);
        key(ImGuiKey_GamepadDpadLeft, input.pad, PCSX2GS_PAD_LEFT);
        key(ImGuiKey_GamepadDpadRight, input.pad, PCSX2GS_PAD_RIGHT);
        key(ImGuiKey_GamepadFaceDown, input.pad, PCSX2GS_PAD_ACCEPT);
        key(ImGuiKey_GamepadFaceRight, input.pad, PCSX2GS_PAD_BACK);
        key(ImGuiKey_GamepadL1, input.pad, PCSX2GS_PAD_L1);
        key(ImGuiKey_GamepadR1, input.pad, PCSX2GS_PAD_R1);
    }
}

void pcsx2gs::SetMenu(Pcsx2GsMenuFn draw, void *user)
{
    std::lock_guard<std::mutex> lock(g_menuMutex);
    g_menuDraw = draw;
    g_menuUser = user;
}

void pcsx2gs::MenuInput(const Pcsx2GsMenuInput *input)
{
    if (!input)
        return;
    std::lock_guard<std::mutex> lock(g_menuMutex);
    g_menuWheel += input->wheel;
    g_menuInput = *input;
}

void pcsx2gs::SetOverlay(const float *lines, const uint32_t *colors, uint32_t count)
{
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    if (!lines || !colors || count == 0u)
    {
        g_overlayLines.clear();
        g_overlayColors.clear();
        return;
    }
    count = std::min(count, 65536u);
    g_overlayLines.assign(lines, lines + 4u * count);
    g_overlayColors.assign(colors, colors + count);
}

void pcsx2gs::SetOverlayText(const float *xy, const uint32_t *colors, const char *texts, uint32_t count)
{
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    g_overlayTextXY.clear();
    g_overlayTextColors.clear();
    g_overlayTexts.clear();
    if (!xy || !colors || !texts || count == 0u)
        return;
    count = std::min(count, 256u);
    g_overlayTextXY.assign(xy, xy + 2u * count);
    g_overlayTextColors.assign(colors, colors + count);
    for (uint32_t i = 0; i < count; ++i)
    {
        g_overlayTexts.emplace_back(texts);
        texts += g_overlayTexts.back().size() + 1u;
    }
}

void pcsx2gs::SetScreen(const Pcsx2GsScreenItem *items, uint32_t count, const char *strings, uint32_t bytes)
{
    std::lock_guard<std::mutex> lock(g_screenMutex);
    g_screenStale = 0;
    g_screenItems.clear();
    g_screenStrings.clear();
    if (!items || count == 0u)
        return;
    count = std::min(count, 1024u);
    g_screenItems.assign(items, items + count);
    if (strings && bytes)
        g_screenStrings.assign(strings, std::min(bytes, 1u << 20));
    g_screenStrings.push_back('\0'); // an offset past the end reads ""
}

void pcsx2gs::SetSceneOffset(float ox, float oy)
{
    g_sceneOffset[0] = ox;
    g_sceneOffset[1] = oy;
}

void pcsx2gs::SetPresentRect(float x0, float y0, float x1, float y1, float width, float height)
{
    g_presentRect[0] = x0;
    g_presentRect[1] = y0;
    g_presentRect[2] = x1;
    g_presentRect[3] = y1;
    if (width > 0.0f && height > 0.0f)
    {
        g_presentSize[0] = width;
        g_presentSize[1] = height;
    }
}

void pcsx2gs::SetOsd(const char *top, uint32_t topRgba, float topAlpha, const char *bottom)
{
    std::lock_guard<std::mutex> lock(g_osdMutex);
    g_osdTop = top ? top : "";
    g_osdBottom = bottom ? bottom : "";
    g_osdTopRgba = topRgba;
    g_osdTopAlpha = std::clamp(topAlpha, 0.0f, 1.0f);
}

void pcsx2gs::SetRenderWindow(const WindowInfo &window)
{
    g_window = window;
}

void pcsx2gs::SetFolders(const std::string &resources, const std::string &cache)
{
    EmuFolders::Resources = resources;
    EmuFolders::Cache = cache;
}

void pcsx2gs::SetTextureFolder(const std::string &folder)
{
    std::filesystem::path path(folder);
    while (!path.empty() && !path.has_filename())
        path = path.parent_path();
    if (path.filename() == "replacements")
        path = path.parent_path();
    EmuFolders::Textures = path.parent_path().string();
    g_textureSerial = path.filename().string();
}

namespace
{
    std::vector<std::string> g_extraTextureFolders; // AddExtraTextureFolder
}

void pcsx2gs::AddExtraTextureFolder(const std::string &folder)
{
    std::string path = folder;
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    if (!path.empty() && std::find(g_extraTextureFolders.begin(), g_extraTextureFolders.end(), path) == g_extraTextureFolders.end())
        g_extraTextureFolders.push_back(path);
}

const std::vector<std::string> &pcsx2gs::ExtraTextureFolders()
{
    return g_extraTextureFolders;
}

void pcsx2gs::SetVerticalFrequency(double hz)
{
    g_verticalFrequency = hz;
}

// --- configuration and emulator state the GS reads ---
Pcsx2Config EmuConfig;
bool FMVstarted = false;
alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];

namespace EmuFolders
{
    std::string Cache;
    std::string Logs;
    std::string Resources;
    std::string Snapshots;
    std::string Textures;
    std::string Videos;
}

Pcsx2Config::Pcsx2Config()
{
    bitset = 0;
}

Pcsx2Config::PadOptions::PadOptions()
{
    bitset = 0;
}

double GetVerticalFrequency()
{
    return g_verticalFrequency;
}

const char *ReportVideoMode()
{
    return "NTSC";
}

const char *ReportInterlaceMode()
{
    return "Progressive";
}

// The software renderer's JIT code lives in PCSX2's code reservation; here only that part (64 MB) exists.
u8 *SysMemory::GetCodePtr(size_t offset)
{
    static u8 *base = static_cast<u8 *>(
        VirtualAlloc(nullptr, HostMemoryMap::SWrecSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!base || offset < HostMemoryMap::SWrecOffset || offset > HostMemoryMap::SWrecOffset + HostMemoryMap::SWrecSize)
        return nullptr;
    return base + (offset - HostMemoryMap::SWrecOffset);
}

// --- Host ---
std::string_view Host::TranslateToStringView(const std::string_view context, const std::string_view msg)
{
    return msg;
}

std::string Host::TranslateToString(const std::string_view context, const std::string_view msg)
{
    return std::string(msg);
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration)
{
    std::fprintf(stderr, "[pcsx2-gs] %s\n", message.c_str());
}

void Host::AddIconOSDMessage(std::string key, const char *icon, const std::string_view message, float duration)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s\n", static_cast<int>(message.size()), message.data());
}

// As PCSX2's Host.cpp (the Vulkan device reports missing features through it).
void Host::ReportFormattedErrorAsync(const std::string_view title, const char* format, ...)
{
    std::va_list ap;
    va_start(ap, format);
    std::string message(StringUtil::StdStringFromFormatV(format, ap));
    va_end(ap);
    ReportErrorAsync(title, message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()),
                 message.data());
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
    function();
}

bool Host::GetBaseBoolSettingValue(const char *section, const char *key, bool default_value)
{
    return default_value;
}

std::string Host::GetStringSettingValue(const char *section, const char *key, const char *default_value)
{
    return default_value ? default_value : "";
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
    return g_window;
}

void Host::BeginPresentFrame()
{
}

void Host::ReleaseRenderWindow()
{
}

bool Host::IsFullscreen()
{
    return false;
}

void Host::SetFullscreen(bool enabled)
{
}

// --- VM / GS thread ---
bool VMManager::HasValidVM()
{
    return true;
}

// The texture replacement folder is <EmuFolders::Textures>/<serial>/replacements; with SetTextureFolder the serial is
// the name of the folder given (a pack folder named anything, e.g. kept beside PCSX2's own).
std::string VMManager::GetDiscSerial()
{
    return g_textureSerial.empty() ? std::string("SLUS-21503") : g_textureSerial;
}

std::string VMManager::GetTitle(bool prefer_en)
{
    return "Kamite";
}

u32 VMManager::GetDiscCRC()
{
    return 0u;
}

const std::vector<u32> &VMManager::Internal::GetSoftwareRendererProcessorList()
{
    static const std::vector<u32> none;
    return none;
}

void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
    func();
}

void MTGS::ApplySettings()
{
}

void MTGS::ToggleSoftwareRendering()
{
}

bool GSDumpReplayer::IsReplayingDump()
{
    return false;
}

bool GSDumpReplayer::IsRunner()
{
    return false;
}

void GSDumpReplayer::RenderUI()
{
}

void GSDumpReplayer::UpdateGSStats()
{
}

void CrashHandler::WriteDumpForCaller()
{
}

// --- performance metrics ---
void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit, bool is_skipping_present)
{
}

void PerformanceMetrics::OnGPUPresent(float gpu_time, u64 vs_invocations, u64 ps_invocations)
{
}

void PerformanceMetrics::SetGSSWThreadCount(u32 count)
{
}

void PerformanceMetrics::SetGSSWThread(u32 index, Threading::ThreadHandle thread)
{
}

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
    return InternalFPSMethod::None;
}

void PerformanceMetrics::StartSavingMetrics(u32 seconds)
{
}

bool PerformanceMetrics::IsSavingMetrics()
{
    return false;
}

void PerformanceMetrics::DumpSavedMetrics()
{
}

// --- ImGui: the devices draw ImGui's lists at every present, so a context and a frame must exist. No OSD yet: the
// lists stay empty (ImGuiManager.cpp of PCSX2 also loads its fonts, styles and the OSD, left out here).
bool ImGuiManager::Initialize()
{
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures |
                           ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ApplyMenuStyle();
        io.DisplayFramebufferScale = ImVec2(1, 1);
        // The OSD font: PCSX2's Roboto when resources/fonts has it, else ImGui's built-in one.
        const std::string roboto = EmuFolders::Resources + "/fonts/Roboto-Regular.ttf";
        g_osdFont = GetFileAttributesA(roboto.c_str()) != INVALID_FILE_ATTRIBUTES ? io.Fonts->AddFontFromFileTTF(roboto.c_str(), 20.0f) : nullptr;
        if (!g_osdFont)
            g_osdFont = io.Fonts->AddFontDefault();
    }
    WindowResized();
    NewFrame();
    return true;
}

void ImGuiManager::Shutdown(bool clear_state)
{
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
}

void ImGuiManager::WindowResized()
{
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(std::max(1u, g_window.surface_width)),
                                            static_cast<float>(std::max(1u, g_window.surface_height)));
}

void ImGuiManager::RequestScaleUpdate()
{
}

void ImGuiManager::ReloadFonts()
{
}

void ImGuiManager::NewFrame()
{
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ApplyMenuInput();
    ImGui::NewFrame();
}

void ImGuiManager::SkipFrame()
{
    ImGui::EndFrame();
    NewFrame();
}

// The screen's images: a PNG decoded by the replacement textures' loader (WIC). Outside the drawing code below, which
// tools/options/overlay_preview.py builds on its own with a loader of its own.
namespace
{
    bool LoadScreenImage(const char *path, std::vector<uint8_t> &rgba, int &width, int &height)
    {
        const int wide = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wide <= 0)
            return false;
        std::wstring widePath(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wide);
        FILE *fp = _wfopen(widePath.c_str(), L"rb");
        if (!fp)
            return false;
        GSTextureReplacements::CFile file(fp);
        GSTextureReplacements::ReplacementTexture image{};
        const GSTextureReplacements::ReplacementTextureLoader loader = GSTextureReplacements::GetLoader(path);
        const bool ok = loader && loader(file, path, &image, true) && image.format == GSTexture::Format::Color &&
                        image.width > 0u && image.height > 0u && image.pitch >= image.width * 4u &&
                        image.data.size() >= static_cast<size_t>(image.pitch) * image.height;
        std::fclose(fp);
        if (!ok)
            return false;
        width = static_cast<int>(image.width);
        height = static_cast<int>(image.height);
        rgba.resize(static_cast<size_t>(width) * height * 4u);
        for (int y = 0; y < height; ++y)
            std::memcpy(&rgba[static_cast<size_t>(y) * width * 4u], image.data.data() + static_cast<size_t>(y) * image.pitch,
                        static_cast<size_t>(width) * 4u);
        return true;
    }
}

// --- the port's own screen (pcsx2gs_set_screen): text in the port's fonts, triangles, boxes ---
namespace
{
    // One UTF-8 code point (a broken sequence reads as U+FFFD).
    unsigned int NextCodepoint(const char *&s, const char *end)
    {
        const unsigned char lead = static_cast<unsigned char>(*s++);
        if (lead < 0x80u)
            return lead;
        int extra;
        unsigned int c;
        if ((lead >> 5) == 0x6u)
            extra = 1, c = lead & 0x1Fu;
        else if ((lead >> 4) == 0xEu)
            extra = 2, c = lead & 0x0Fu;
        else if ((lead >> 3) == 0x1Eu)
            extra = 3, c = lead & 0x07u;
        else
            return 0xFFFDu;
        for (; extra > 0; --extra)
        {
            if (s >= end || (static_cast<unsigned char>(*s) >> 6) != 0x2u)
                return 0xFFFDu;
            c = (c << 6) | (static_cast<unsigned char>(*s++) & 0x3Fu);
        }
        return c;
    }

    // A font file, loaded on first use (GS thread; ImGui 1.92 adds fonts mid-frame).
    ScreenFont *ScreenFontFor(const char *path)
    {
        if (!path || !*path)
            return nullptr;
        for (auto &entry : g_screenFonts)
            if (entry.first == path)
                return entry.second.font ? &entry.second : nullptr;
        g_screenFonts.emplace_back(path, ScreenFont{});
        ScreenFont &f = g_screenFonts.back().second;
        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        config.OversampleH = 2; // the letters are squeezed sideways
        config.OversampleV = 1;
        f.font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 20.0f, &config);
        if (!f.font)
        {
            std::fprintf(stderr, "[gs] screen font did not load: %s\n", path);
            return nullptr;
        }
        // Sizes come as cap heights: the 'H' at a large size gives the font's ratio.
        if (ImFontBaked *baked = f.font->GetFontBaked(200.0f))
            if (const ImFontGlyph *h = baked->FindGlyph(static_cast<ImWchar>('H')); h && h->Y1 > h->Y0)
                f.capRatio = (h->Y1 - h->Y0) / 200.0f;
        return &f;
    }

    struct ScreenRun
    {
        ImFontBaked *baked = nullptr;
        float scale = 1.0f, scaleX = 1.0f; // baked glyph units -> pixels
        float tracking = 0.0f;             // pixels
    };

    float ScreenRunWidth(const ScreenRun &run, const char *text, const char *end)
    {
        float width = 0.0f;
        bool first = true;
        for (const char *s = text; s < end;)
        {
            const ImFontGlyph *g = run.baked->FindGlyph(static_cast<ImWchar>(NextCodepoint(s, end)));
            if (!g)
                continue;
            width += g->AdvanceX * run.scaleX + (first ? 0.0f : run.tracking);
            first = false;
        }
        return width;
    }

    // Glyph by glyph from the atlas (the draw list's texture), so the letters can be squeezed and spaced.
    void ScreenRunDraw(ImDrawList *list, const ScreenRun &run, const char *text, const char *end, float x, float top, ImU32 colour)
    {
        for (const char *s = text; s < end;)
        {
            const ImFontGlyph *g = run.baked->FindGlyph(static_cast<ImWchar>(NextCodepoint(s, end)));
            if (!g)
                continue;
            if (g->Visible)
            {
                list->PrimReserve(6, 4);
                list->PrimRectUV(ImVec2(x + g->X0 * run.scaleX, top + g->Y0 * run.scale),
                                 ImVec2(x + g->X1 * run.scaleX, top + g->Y1 * run.scale), ImVec2(g->U0, g->V0),
                                 ImVec2(g->U1, g->V1), colour);
            }
            x += g->AdvanceX * run.scaleX + run.tracking;
        }
    }

    // Offsets on rings out to the radius, a copy about every pixel round each ring (the stroke).
    template <typename Fn>
    void ForStrokeOffsets(float radius, Fn fn)
    {
        const int rings = std::clamp(static_cast<int>(std::ceil(radius / 1.5f)), 1, 4);
        for (int r = 1; r <= rings; ++r)
        {
            const float rr = radius * static_cast<float>(r) / static_cast<float>(rings);
            const int n = std::clamp(static_cast<int>(std::ceil(6.2831853f * rr)), 8, 32);
            for (int i = 0; i < n; ++i)
            {
                const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
                fn(rr * std::cos(a), rr * std::sin(a));
            }
        }
    }

    void DrawScreenText(ImDrawList *list, const Pcsx2GsScreenItem &item, const char *text, const char *fontPath, float x0,
                        float y0, float w, float h)
    {
        const float cap = item.size * h;
        ScreenFont *font = cap >= 1.0f && *text ? ScreenFontFor(fontPath) : nullptr;
        if (!font)
            return;
        const float fontSize = cap / std::max(0.05f, font->capRatio);
        // Softening: the letters baked smaller and drawn larger through the picture's linear filter, like the game's
        // own upscaled 2D (a blur of r pixels = baked r times smaller).
        const float soften = std::max(1.0f, item.blur * h);
        const float bakeSize = std::max(6.0f, std::round(fontSize / soften));
        ScreenRun run;
        run.baked = font->font->GetFontBaked(bakeSize);
        if (!run.baked)
            return;
        run.scale = fontSize / bakeSize;
        run.scaleX = run.scale * std::max(0.05f, item.scale_x);
        run.tracking = item.tracking * h;
        const char *end = text + std::strlen(text);
        float width = ScreenRunWidth(run, text, end); // bakes every glyph before any is drawn
        if (item.max_width > 0.0f && width > item.max_width * h)
        {
            const float squeeze = item.max_width * h / width;
            run.scaleX *= squeeze;
            run.tracking *= squeeze;
            width = item.max_width * h;
        }
        const ImFontGlyph *capH = run.baked->FindGlyph(static_cast<ImWchar>('H'));
        float x = x0 + item.x * w - (item.align == PCSX2GS_ALIGN_CENTRE ? 0.5f * width : item.align == PCSX2GS_ALIGN_RIGHT ? width : 0.0f);
        float top = y0 + item.y * h - (capH ? capH->Y1 : run.baked->Ascent) * run.scale;
        if (item.blur <= 0.0f)
        {
            x = std::floor(x + 0.5f);
            top = std::floor(top + 0.5f);
        }
        if (item.shadow >> 24)
            ScreenRunDraw(list, run, text, end, x + item.shadow_x * h, top + item.shadow_y * h, item.shadow);
        if ((item.stroke >> 24) && item.stroke_width > 0.0f)
            ForStrokeOffsets(item.stroke_width * h, [&](float dx, float dy) { ScreenRunDraw(list, run, text, end, x + dx, top + dy, item.stroke); });
        if (item.fill >> 24)
            ScreenRunDraw(list, run, text, end, x, top, item.fill);
    }

    void DrawScreenTriangle(ImDrawList *list, const Pcsx2GsScreenItem &item, float x0, float y0, float w, float h)
    {
        ImVec2 p[3] = {ImVec2(x0 + item.x * w, y0 + item.y * h), ImVec2(x0 + item.x1 * w, y0 + item.y1 * h),
                       ImVec2(x0 + item.x2 * w, y0 + item.y2 * h)};
        const auto cross = [](const ImVec2 &o, const ImVec2 &a, const ImVec2 &b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
        if (cross(p[0], p[1], p[2]) < 0.0f) // ImGui's anti-aliased fills want clockwise points
            std::swap(p[1], p[2]);
        // The stroke: the triangle grown by its width round the incentre (sharp corners).
        const auto grown = [&](float d, ImVec2 *out)
        {
            const auto length = [](const ImVec2 &a, const ImVec2 &b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); };
            const float a = length(p[1], p[2]), b = length(p[2], p[0]), c = length(p[0], p[1]), perimeter = a + b + c;
            const float r = perimeter > 0.0f ? std::fabs(cross(p[0], p[1], p[2])) / perimeter : 0.0f;
            const float k = r > 0.0f ? (r + d) / r : 1.0f;
            const ImVec2 centre((a * p[0].x + b * p[1].x + c * p[2].x) / std::max(perimeter, 1e-6f),
                                (a * p[0].y + b * p[1].y + c * p[2].y) / std::max(perimeter, 1e-6f));
            for (int i = 0; i < 3; ++i)
                out[i] = ImVec2(centre.x + (p[i].x - centre.x) * k, centre.y + (p[i].y - centre.y) * k);
        };
        const float stroke = (item.stroke >> 24) ? item.stroke_width * h : 0.0f;
        const float fringe = list->_FringeScale;
        list->_FringeScale = std::max(1.0f, item.blur * h); // softening = a wider anti-aliased edge
        ImVec2 outer[3];
        grown(stroke, outer);
        if (item.shadow >> 24)
        {
            const ImVec2 s(item.shadow_x * h, item.shadow_y * h);
            list->AddTriangleFilled(ImVec2(outer[0].x + s.x, outer[0].y + s.y), ImVec2(outer[1].x + s.x, outer[1].y + s.y),
                                    ImVec2(outer[2].x + s.x, outer[2].y + s.y), item.shadow);
        }
        if (stroke > 0.0f)
            list->AddTriangleFilled(outer[0], outer[1], outer[2], item.stroke);
        if (item.fill >> 24)
            list->AddTriangleFilled(p[0], p[1], p[2], item.fill);
        list->_FringeScale = fringe;
    }

    void DrawScreenRect(ImDrawList *list, const Pcsx2GsScreenItem &item, float x0, float y0, float w, float h)
    {
        const ImVec2 a(x0 + item.x * w, y0 + item.y * h), b(x0 + item.x1 * w, y0 + item.y1 * h);
        const float stroke = (item.stroke >> 24) ? item.stroke_width * h : 0.0f;
        if (item.shadow >> 24)
            list->AddRectFilled(ImVec2(a.x - stroke + item.shadow_x * h, a.y - stroke + item.shadow_y * h),
                                ImVec2(b.x + stroke + item.shadow_x * h, b.y + stroke + item.shadow_y * h), item.shadow);
        if (stroke > 0.0f)
            list->AddRectFilled(ImVec2(a.x - stroke, a.y - stroke), ImVec2(b.x + stroke, b.y + stroke), item.stroke);
        if (item.fill >> 24)
            list->AddRectFilled(a, b, item.fill);
    }

    // An image file, loaded on first use and shrunk by a whole factor (a box filter, alpha-weighted) to about the size
    // it is first drawn at, so the picture's linear filter never minifies it much. A later, larger window draws it
    // scaled up.
    ImTextureData *ScreenImageFor(const char *path, float drawnHeight)
    {
        if (!path || !*path)
            return nullptr;
        for (auto &entry : g_screenImages)
            if (entry.first == path)
                return entry.second;
        g_screenImages.emplace_back(path, nullptr);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!LoadScreenImage(path, rgba, w, h))
        {
            std::fprintf(stderr, "[gs] screen image did not load: %s\n", path);
            return nullptr;
        }
        const int f = std::clamp(static_cast<int>(static_cast<float>(h) / std::max(drawnHeight, 1.0f)), 1, 64);
        const int ow = std::max(1, w / f), oh = std::max(1, h / f);
        ImTextureData *tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, ow, oh);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
            {
                float sum[4] = {};
                for (int yy = y * f; yy < y * f + f; ++yy)
                    for (int xx = x * f; xx < x * f + f; ++xx)
                    {
                        const uint8_t *p = &rgba[(static_cast<size_t>(yy) * w + xx) * 4u];
                        const float a = p[3];
                        sum[0] += p[0] * a;
                        sum[1] += p[1] * a;
                        sum[2] += p[2] * a;
                        sum[3] += a;
                    }
                unsigned char *out = static_cast<unsigned char *>(tex->GetPixelsAt(x, y));
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<unsigned char>(sum[3] > 0.0f ? std::min(255.0f, sum[c] / sum[3] + 0.5f) : 0.0f);
                out[3] = static_cast<unsigned char>(sum[3] / static_cast<float>(f * f) + 0.5f);
            }
        ImGui::RegisterUserTexture(tex);
        g_screenImages.back().second = tex;
        return tex;
    }

    void DrawScreenImage(ImDrawList *list, const Pcsx2GsScreenItem &item, const char *path, float x0, float y0, float w, float h)
    {
        const ImVec2 a(x0 + item.x * w, y0 + item.y * h), b(x0 + item.x1 * w, y0 + item.y1 * h);
        if ((item.fill >> 24) == 0u || b.y - a.y < 1.0f)
            return;
        if (ImTextureData *tex = ScreenImageFor(path, b.y - a.y))
            list->AddImage(tex->GetTexRef(), a, b, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), item.fill);
    }

    // RenderOSD (GS thread): the list over the picture, under the menu and the OSD.
    void DrawScreen()
    {
        std::lock_guard<std::mutex> lock(g_screenMutex);
        if (g_screenItems.empty())
            return;
        if (++g_screenStale > kScreenStaleLimit)
        {
            g_screenItems.clear();
            g_screenStrings.clear();
            return;
        }
        const float x0 = g_presentRect[0], y0 = g_presentRect[1];
        const float w = g_presentRect[2] - x0, h = g_presentRect[3] - y0;
        if (w <= 1.0f || h <= 1.0f)
            return;
        const auto str = [](uint32_t offset) { return offset < g_screenStrings.size() ? g_screenStrings.c_str() + offset : ""; };
        ImDrawList *list = ImGui::GetBackgroundDrawList();
        list->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), true);
        for (const Pcsx2GsScreenItem &item : g_screenItems)
        {
            if (item.kind == PCSX2GS_SCREEN_TEXT)
                DrawScreenText(list, item, str(item.text), str(item.font), x0, y0, w, h);
            else if (item.kind == PCSX2GS_SCREEN_TRIANGLE)
                DrawScreenTriangle(list, item, x0, y0, w, h);
            else if (item.kind == PCSX2GS_SCREEN_RECT)
                DrawScreenRect(list, item, x0, y0, w, h);
            else if (item.kind == PCSX2GS_SCREEN_IMAGE)
                DrawScreenImage(list, item, str(item.text), x0, y0, w, h);
        }
        list->PopClipRect();
    }
}

// The port's overlays (the host loop draws them with raylib under its own GS): a message box at the top left
// (save-state messages, notes) and the F3 FPS line at the bottom left, sized like the raylib ones.
void ImGuiManager::RenderOSD()
{
    Pcsx2GsMenuFn menu;
    void *menuUser;
    {
        std::lock_guard<std::mutex> lock(g_menuMutex);
        menu = g_menuDraw;
        menuUser = g_menuUser;
    }
    if (menu)
        menu(&g_ui, menuUser);

    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        const float width = g_presentRect[2] - g_presentRect[0], height = g_presentRect[3] - g_presentRect[1];
        if (!g_overlayColors.empty())
        {
            ImDrawList *list = ImGui::GetBackgroundDrawList();
            if (width > 1.0f && height > 1.0f)
            {
                const float sx = width / g_presentSize[0], sy = height / g_presentSize[1];
                const float thickness = std::max(1.0f, std::floor(ImGui::GetIO().DisplaySize.y / 540.0f));
                list->PushClipRect(ImVec2(g_presentRect[0], g_presentRect[1]), ImVec2(g_presentRect[2], g_presentRect[3]), true);
                for (size_t i = 0; i < g_overlayColors.size(); ++i)
                {
                    const float *l = &g_overlayLines[4 * i];
                    const ImVec2 a(g_presentRect[0] + (l[0] - g_sceneOffset[0]) * sx, g_presentRect[1] + (l[1] - g_sceneOffset[1]) * sy);
                    const ImVec2 b(g_presentRect[0] + (l[2] - g_sceneOffset[0]) * sx, g_presentRect[1] + (l[3] - g_sceneOffset[1]) * sy);
                    list->AddLine(a, b, g_overlayColors[i], thickness);
                }
                list->PopClipRect();
            }
        }
        // the labels over the lines
        if (!g_overlayTexts.empty() && width > 1.0f && height > 1.0f)
        {
            ImDrawList *list = ImGui::GetBackgroundDrawList();
            const float sx = width / g_presentSize[0], sy = height / g_presentSize[1];
            list->PushClipRect(ImVec2(g_presentRect[0], g_presentRect[1]), ImVec2(g_presentRect[2], g_presentRect[3]), true);
            for (size_t i = 0; i < g_overlayTexts.size(); ++i)
            {
                const char *text = g_overlayTexts[i].c_str();
                const ImVec2 size = ImGui::CalcTextSize(text);
                const ImVec2 at(g_presentRect[0] + (g_overlayTextXY[2 * i] - g_sceneOffset[0]) * sx - size.x * 0.5f,
                                g_presentRect[1] + (g_overlayTextXY[2 * i + 1] - g_sceneOffset[1]) * sy - size.y);
                list->AddRectFilled(ImVec2(at.x - 3.0f, at.y - 1.0f), ImVec2(at.x + size.x + 3.0f, at.y + size.y + 1.0f), 0xA0000000u, 3.0f);
                list->AddText(at, g_overlayTextColors[i], text);
            }
            list->PopClipRect();
        }
    }

    DrawScreen();

    std::string top, bottom;
    uint32_t topRgba;
    float topAlpha;
    {
        std::lock_guard<std::mutex> lock(g_osdMutex);
        top = g_osdTop;
        bottom = g_osdBottom;
        topRgba = g_osdTopRgba;
        topAlpha = g_osdTopAlpha;
    }
    if ((top.empty() || topAlpha <= 0.0f) && bottom.empty())
        return;
    ImDrawList *list = ImGui::GetForegroundDrawList();
    ImFont *font = g_osdFont ? g_osdFont : ImGui::GetFont();
    const float height = ImGui::GetIO().DisplaySize.y;
    const float size = std::max(16.0f, std::floor(height / 36.0f));
    const auto box = [&](const std::string &text, float x, float y, ImU32 background, ImU32 foreground)
    {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        list->AddRectFilled(ImVec2(x - 8.0f, y - 6.0f), ImVec2(x + extent.x + 8.0f, y + extent.y + 6.0f), background);
        list->AddText(font, size, ImVec2(x, y), foreground, text.c_str());
    };
    if (!top.empty() && topAlpha > 0.0f)
    {
        const auto channel = [&](int shift) { return static_cast<int>((topRgba >> shift) & 0xFFu); };
        const int alpha = static_cast<int>(channel(24) * topAlpha);
        box(top, 24.0f, 20.0f, IM_COL32(channel(0), channel(8), channel(16), alpha),
            IM_COL32(255, 255, 255, static_cast<int>(255 * topAlpha)));
    }
    if (!bottom.empty())
        box(bottom, 16.0f, height - size - 20.0f, IM_COL32(0, 0, 0, 153), IM_COL32(255, 255, 255, 255));
}

void FullscreenUI::Render()
{
}

const ImFontLoader *ImGuiFreeType::GetFontLoader()
{
    return nullptr;
}

// --- GS dumps, video capture, PNG snapshots (left out) ---
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                               u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                               const GSPrivRegSet *regs)
{
    return nullptr;
}

std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                     u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                     const GSPrivRegSet *regs)
{
    return nullptr;
}

std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                      u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                      const GSPrivRegSet *regs)
{
    return nullptr;
}

void GSDumpBase::ReadFIFO(u32 size)
{
}

void GSDumpBase::Transfer(int index, const u8 *mem, size_t size)
{
}

bool GSDumpBase::VSync(int field, bool last, const GSPrivRegSet *regs)
{
    return false;
}

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
    return false;
}

bool GSCapture::DeliverVideoFrame(GSTexture *stex)
{
    return false;
}

void GSCapture::EndCapture()
{
}

bool GSCapture::IsCapturing()
{
    return false;
}

bool GSCapture::IsCapturingVideo()
{
    return false;
}

GSVector2i GSCapture::GetSize()
{
    return GSVector2i(0, 0);
}

std::string GSCapture::GetNextCaptureFileName()
{
    return {};
}

void GSCapture::Flush()
{
}

// A small PNG writer (stored deflate blocks, no libpng): the port's draw dump (Shift+F10) saves the frame's colour and
// depth through GSTexture::Save. RGBA and RGB_A write one RGBA image, RGB drops the alpha, R8I is greyscale.
bool GSPng::Save(GSPng::Format fmt, const std::string &file, const u8 *image, int w, int h, int pitch, int compression, bool rb_swapped)
{
    (void)compression;
    if (!image || w <= 0 || h <= 0)
        return false;
    int channels, in_bpp;
    u8 colour_type;
    switch (fmt)
    {
        case RGBA_PNG: case RGB_A_PNG: channels = 4; in_bpp = 4; colour_type = 6; break;
        case RGB_PNG: channels = 3; in_bpp = 4; colour_type = 2; break;
        case R8I_PNG: channels = 1; in_bpp = 1; colour_type = 0; break;
        default: return false;
    }
    std::vector<u8> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + w * channels));
    for (int y = 0; y < h; y++)
    {
        raw.push_back(0);
        const u8 *row = image + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < w; x++)
        {
            const u8 *px = row + x * in_bpp;
            if (channels == 1) { raw.push_back(px[0]); continue; }
            raw.push_back(px[rb_swapped ? 2 : 0]);
            raw.push_back(px[1]);
            raw.push_back(px[rb_swapped ? 0 : 2]);
            if (channels == 4) raw.push_back(px[3]);
        }
    }
    static u32 table[256];
    if (!table[1])
        for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[n] = c; }
    const auto put32 = [](std::vector<u8> &v, u32 x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); };
    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&](const char *type, const std::vector<u8> &data) {
        put32(out, static_cast<u32>(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        u32 c = 0xFFFFFFFFu;
        for (size_t i = start; i < out.size(); i++) c = table[(c ^ out[i]) & 0xFF] ^ (c >> 8);
        put32(out, c ^ 0xFFFFFFFFu);
    };
    std::vector<u8> ihdr;
    put32(ihdr, static_cast<u32>(w)); put32(ihdr, static_cast<u32>(h));
    ihdr.insert(ihdr.end(), {8, colour_type, 0, 0, 0});
    chunk("IHDR", ihdr);
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size() || pos == 0;)
    {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + len >= raw.size() ? 1 : 0);
        z.push_back(len & 0xFF); z.push_back(len >> 8); z.push_back(~len & 0xFF); z.push_back((~len >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
        for (size_t i = pos; i < pos + len; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
        pos += len;
        if (pos >= raw.size()) break;
    }
    put32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::FILE *f = std::fopen(file.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

RGBA8Image::RGBA8Image() = default;

RGBA8Image::RGBA8Image(RGBA8Image &&move)
    : Image(move)
{
}

bool RGBA8Image::SaveToFile(const char *filename, u8 quality) const
{
    return false;
}
