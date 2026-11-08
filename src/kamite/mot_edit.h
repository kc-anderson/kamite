#pragma once

// MOT motions edited in place (PlatinumGames' "mtb3": tools/models/mot.py): parse a motion's records, sample a curve,
// encode values as a curve, serialize, add a per-frame delta to a channel. Shared by npc_poses (NPC poses) and
// punish_fit (the punishes' heights and bend), from npc_poses' own code (Nov 8).

#include <array>
#include <cstdint>
#include <vector>

namespace kamite::mot
{
    float halfToFloat(uint16_t v);
    uint16_t floatToHalf(float x);
    // The motion's length from its record table (the last curve block's end), 0 if it is not a motion.
    uint32_t motionLength(const uint8_t *m, uint32_t available);

    struct Rec
    {
        uint8_t bone = 0, channel = 0;
        uint32_t flags = 0;
        uint16_t count = 0;
        float constant = 0.0f;
        std::vector<uint8_t> block; // header (6 halves) and keys, for a curve
    };

    bool parse(const std::vector<uint8_t> &m, std::vector<Rec> &recs);
    std::vector<uint8_t> serialize(uint16_t frames, uint8_t loop, const std::vector<Rec> &recs);
    float sample(const Rec &rec, float t);
    // A curve through `v` (one value a frame), keys every 3 frames (more apart past 120) and on the last; `every` > 0
    // sets the spacing. `loop`: the slopes at the ends wrap round (a looping motion) or not.
    Rec encode(uint8_t bone, uint8_t component, const std::vector<float> &v, int every = 0, bool loop = true);
    // delta[f] (repeated if shorter) added to the bone's component (0..2 translation, 3..5 rotation) every frame; the
    // record becomes a curve.
    // `every` as encode's (0: its default spacing; 1: a key every frame, the curve kept to the 8-bit steps).
    void addCurve(std::vector<uint8_t> &m, int bone, int component, const std::vector<float> &delta, int every = 0);

    // Rotations (row-major 3x3) in the game's order (mot.py, pose.rot 'XYZ'): R = Rz * Ry * Rx.
    using Mat = std::array<float, 9>;
    Mat mul(const Mat &a, const Mat &b);
    Mat rotX(float a);
    Mat rotY(float a);
    Mat rotZ(float a);
    Mat euler(float x, float y, float z);
    void toEuler(const Mat &r, float &x, float &y, float &z);
    float nearAngle(float a, float ref); // a moved by whole turns to within half a turn of ref
    // The bone's rotation becomes pre[f] * its own (pre in its parent's space) at every frame f (the last repeats when
    // pre is shorter); its x, y, z curves are resampled (`every` as encode's). With `post`: pre[f] * its own * post[f]
    // (post in the bone's own space, e.g. a twist about its length); pre may then be empty.
    void preRotate(std::vector<uint8_t> &m, int bone, const std::vector<Mat> &pre, int every = 0,
                   const std::vector<Mat> &post = {});
}
