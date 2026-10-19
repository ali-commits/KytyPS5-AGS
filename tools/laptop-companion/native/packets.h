#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace Companion {
// Exact RouteSpeaker payload from src/libs/dualSenseHaptics.cpp.
inline std::array<uint8_t, 38> SpeakerRoute(bool speaker) {
    std::array<uint8_t, 38> report {};
    report[0] = 0x80;
    if (speaker) {
        report[0] |= 0x20;
        report[1] = 0x80;
        report[5] = 0x64;
        report[7] = 0x30;
        report[37] = 0x02;
    }
    return report;
}
inline std::array<uint8_t, 47> Triggers(const uint8_t* left, const uint8_t* right, uint8_t flags) {
    std::array<uint8_t, 47> report {};
    report[0] = flags;
    std::copy_n(right, 11, report.begin() + 10);
    std::copy_n(left, 11, report.begin() + 21);
    return report;
}
}
