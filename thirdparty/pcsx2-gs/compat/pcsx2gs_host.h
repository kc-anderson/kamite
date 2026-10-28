// What the port tells PCSX2's GS about its host (docs/GS_PCSX2.md). Implemented in HostStubs.cpp.
#pragma once

#include "common/WindowInfo.h"
#include "pcsx2gs_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pcsx2gs
{
    // The window the GS renders into (Host::AcquireRenderWindow returns it).
    void SetRenderWindow(const WindowInfo &window);
    // Folders: resources = the folder holding PCSX2's "shaders/" (dx11, dx12...); cache = shader cache.
    void SetFolders(const std::string &resources, const std::string &cache);
    // Replacement textures: the folder holding "replacements/" (any name; it stands for the serial folder).
    void SetTextureFolder(const std::string &folder);
    // More folders searched like replacements/ (any layout below them), e.g. a character mod's textures.
    void AddExtraTextureFolder(const std::string &folder);
    const std::vector<std::string> &ExtraTextureFolders();
    // Field rate the GS paces for (GetVerticalFrequency), e.g. 59.94.
    void SetVerticalFrequency(double hz);
    // Texts ImGuiManager::RenderOSD draws (pcsx2gs_api.h set_osd); thread-safe.
    void SetOsd(const char *top, uint32_t topRgba, float topAlpha, const char *bottom);
    // In-game menu (pcsx2gs_api.h set_menu / menu_input); thread-safe.
    void SetMenu(Pcsx2GsMenuFn draw, void *user);
    void MenuInput(const Pcsx2GsMenuInput *input);
    // The overlay lines (pcsx2gs_set_overlay; thread-safe): x0 y0 x1 y1 per line in GS pixel coordinates (the game's
    // world-to-screen matrix's output, XYOFFSET included), a colour per line (R in the low byte). count 0 clears.
    // ImGuiManager::RenderOSD draws them at each present, mapped through the scene's XYOFFSET and the present rect.
    void SetOverlay(const float *lines, const uint32_t *colors, uint32_t count);
    // GSRendererHW (GS thread): the XYOFFSET (pixels) of the scene's depth-tested triangle draws, for that mapping.
    void SetSceneOffset(float ox, float oy);
    // GSRenderer::VSync (GS thread): where the frame went in the window (client pixels) and its size in GS pixels.
    void SetPresentRect(float x0, float y0, float x1, float y1, float width, float height);
}
