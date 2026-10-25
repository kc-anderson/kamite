#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Full-resolution versions of the textures the port changes at run time (the radar's glow; the prompt sheets).
//
// PCSX2 finds a replacement image by the texture's content: <TEX0 hash>-<CLUT hash>-<bits> (GSTextureReplacements),
// so a texture the port rewrites in RAM no longer matches the texture pack's image for it and is drawn at the PS2's
// resolution with the PS2's art (the radar's rope rings, which the pack's artist removed; the prompt sheets' letters).
// This module lets those features see what the pack has (packFile by name), name the textures they make (name8, the
// same hash PCSX2 computes) and write full-resolution images for them into a folder of the port's own that PCSX2
// searches too (generatedDir, added to PS2X_PCSX2_TEXTURE_EXTRA). PCSX2 lists the replacement folders when the GS
// opens, so the images have to be written before the game's first frame: install() and the features' prepare steps
// run right after the runtime is initialised.
namespace kamite::hd_textures
{
    // Reads [Runtime] PS2X_PCSX2_TEXTURES / PS2X_PCSX2_TEXTURE_DIR, lists the pack's file names and adds
    // <exe>/cache/hd-textures to PS2X_PCSX2_TEXTURE_EXTRA. Call after characters::install (which sets that variable).
    void install(const std::filesystem::path &exeDir);

    // Texture replacements on (and the generated folder usable).
    bool enabled();
    // The pack's file for a replacement name ("9c64...-82a5...-000061d3"), .png or .dds; empty when it has none.
    std::filesystem::path packFile(const std::string &name);
    // Where the port writes its own images (exists when enabled()).
    const std::filesystem::path &generatedDir();

    // The replacement name of a 128-texel-wide-or-more PSMT8 texture (TBW = width / 64) as the game uploads it: the
    // payload as the PSMCT32 image the game sends (ctWidth x ctHeight words, TBW ctWidth / 64, row-major), its CLUT as
    // the 16x16 PSMCT32 CSM1 block the game sends (1 KB). TCC on (the 0x4000 bit PCSX2's dumps of this game carry).
    // Checked against the pack's names for the radar (9c64c8261344d14c-82a523a12b163049-000061d3) and both prompt sheets.
    std::string name8(const uint8_t *payload, int ctWidth, int ctHeight, const uint8_t *clut, int width, int height);

    // RGBA8 images (the pack's PNGs keep the PS2's alpha scale: 0x80 = opaque). Load: false when the file is not a
    // readable PNG. Save writes a PNG.
    struct Image
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    bool loadPng(const std::filesystem::path &path, Image &out);
    bool savePng(const std::filesystem::path &path, const Image &image);
}
