#pragma once

#include "ps2x/state_io.h"

#include <filesystem>
#include <functional>
#include <string>

namespace ps2x::savestate
{
    using SaveFn = std::function<void(ps2x::state::Writer &)>;
    using LoadFn = std::function<void(ps2x::state::Reader &)>;

    void registerExtension(std::string name, SaveFn save, LoadFn load);
    // Called after every load (with or without the extensions), before the game runs again.
    void onLoaded(std::function<void()> fn);

    void setQuickSlotPath(std::filesystem::path path);
    [[nodiscard]] std::filesystem::path quickSlotPath();
    void requestSave(std::filesystem::path path);
    void requestLoad(std::filesystem::path path);
    void requestQuickSave();
    void requestQuickLoad();
    // States loaded / saved so far (they change right after the load or save, before the game runs again).
    [[nodiscard]] uint32_t loadCount();
    [[nodiscard]] uint32_t saveCount();

    [[nodiscard]] bool overlayMessage(std::string &text, float &alpha);
}
