// Applies punish_fit.cpp rewrite()'s pose edits (same order, same matrices) to a motion blob, per-frame values from a file.
#include "kamite/mot_edit.h"
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
using namespace kamite;
int main(int argc, char **argv)
{
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> m((std::istreambuf_iterator<char>(f)), {});
    uint16_t frames; std::memcpy(&frames, m.data() + 4, 2);
    std::ifstream vf(argv[3]);
    const float kDegrees = 3.14159265358979f / 180.0f;
    std::vector<mot::Mat> pose[7], twist;
    for (int fr = 0; fr < frames; ++fr)
    {
        float v[13];
        for (float &x : v) vf >> x;
        float a[8];
        for (int i = 0; i < 8; ++i) a[i] = v[4 + i] * kDegrees;
        pose[0].push_back(mot::mul(mot::rotZ(a[1]), mot::rotX(a[0])));
        pose[1].push_back(mot::mul(mot::rotZ(a[2]), mot::rotY(a[3])));
        pose[2].push_back(mot::mul(mot::rotZ(a[2] * 0.5f), mot::rotY(a[3] * 0.5f)));
        pose[3].push_back(mot::mul(mot::rotZ(-a[5]), mot::rotY(-a[6])));
        pose[4].push_back(mot::mul(mot::rotZ(-a[5] * 0.5f), mot::rotY(-a[6] * 0.5f)));
        pose[5].push_back(mot::rotY(a[4]));
        pose[6].push_back(mot::rotY(-a[7]));
        twist.push_back(mot::rotX(v[12] * kDegrees * 0.5f));
    }
    const int bones[7] = {1, 13, 12, 7, 6, 14, 8};
    for (int i = 0; i < 7; ++i) mot::preRotate(m, bones[i], pose[i], 1);
    for (int b : {15, 16}) mot::preRotate(m, b, {}, 1, twist);
    std::ofstream o(argv[2], std::ios::binary); o.write(reinterpret_cast<const char *>(m.data()), m.size());
}
