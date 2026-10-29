#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
    namespace PadButton
    {
        constexpr uint16_t Select = 1u << 0u;
        constexpr uint16_t L3 = 1u << 1u;
        constexpr uint16_t R3 = 1u << 2u;
        constexpr uint16_t Start = 1u << 3u;
        constexpr uint16_t Up = 1u << 4u;
        constexpr uint16_t Right = 1u << 5u;
        constexpr uint16_t Down = 1u << 6u;
        constexpr uint16_t Left = 1u << 7u;
        constexpr uint16_t L2 = 1u << 8u;
        constexpr uint16_t R2 = 1u << 9u;
        constexpr uint16_t L1 = 1u << 10u;
        constexpr uint16_t R1 = 1u << 11u;
        constexpr uint16_t Triangle = 1u << 12u;
        constexpr uint16_t Circle = 1u << 13u;
        constexpr uint16_t Cross = 1u << 14u;
        constexpr uint16_t Square = 1u << 15u;
    }

    enum class PadPressure : uint32_t
    {
        Right,
        Left,
        Up,
        Down,
        Triangle,
        Circle,
        Cross,
        Square,
        L1,
        R1,
        L2,
        R2,
        Count,
    };

    struct PadInputState
    {
        bool connected = false;
        uint16_t buttons = 0u;
        uint8_t rightX = 0x80u;
        uint8_t rightY = 0x80u;
        uint8_t leftX = 0x80u;
        uint8_t leftY = 0x80u;
        std::array<uint8_t, static_cast<size_t>(PadPressure::Count)> pressure{};
    };
}
