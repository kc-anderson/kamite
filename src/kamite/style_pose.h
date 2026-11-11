#pragma once

class PS2Runtime;

// Signal Style Switch's arm swing (K, Nov 11): on a style switch while Gene is free (idle / walking / running:
// mode 0, phase 0..3), his arms and shoulders play the throw-back of his quick turn (motion 30), drawn only, blended
// over his own motion and faded out the moment he does anything else. Gameplay, inputs and timing are untouched.
namespace kamite::style_pose
{
    void install(PS2Runtime &runtime); // with Signal Style Switch on
    void trigger();                    // any thread: a switch in play (skipped if Gene isn't free)

    void drawTuning(const void *ui);   // temporary F2 sliders (Pcsx2GsUi *), from style_presets' panel
}
