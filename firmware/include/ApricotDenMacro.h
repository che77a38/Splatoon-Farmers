#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MacroEngine.h"

namespace farmers {

// Apricot Den routine — ported from EndingCrystal/Splatoon-Farmers fork
// (https://github.com/EndingCrystal/Splatoon-Farmers). The web UI displays
// this as "天妇罗巢穴"; the upstream file only exposes this as
// `kMaterialFarmMacro`, so we rename to `kApricotDenMacro` to keep both
// scripts coexisting in the same translation unit.
//
// Button masks mirror the upstream header. We keep all names under the
// `farmers` namespace; constants that overlap with MaterialFarmMacro.h
// are prefixed with `kApricot` to avoid redefinition errors.
constexpr uint16_t kApricotButtonB = 1u << 1;
constexpr uint16_t kApricotButtonA = 1u << 2;
constexpr uint16_t kApricotButtonX = 1u << 3;
constexpr uint16_t kApricotButtonL = 1u << 4;
constexpr uint16_t kApricotButtonR = 1u << 5;
constexpr uint16_t kApricotButtonPlus = 1u << 9;  // Plus / BUTTON_START
constexpr uint16_t kApricotButtonZR = 1u << 7;
constexpr uint16_t kApricotButtonZL = 1u << 6;
constexpr uint16_t kApricotButtonR3 = 1u << 11;

constexpr ControllerReport kApricotReportR3{
    kApricotButtonR3, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportZR{
    kApricotButtonZR, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportPlus{
    kApricotButtonPlus, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportA{
    kApricotButtonA, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportB{
    kApricotButtonB, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportX{
    kApricotButtonX, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotLeftUpX{
    kApricotButtonX, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kApricotLeftUpL{
    kApricotButtonL, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kApricotLeftUpR{
    kApricotButtonR, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kApricotReportL{
    kApricotButtonL, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotReportR{
    kApricotButtonR, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kApricotLeftUp{
    0, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kApricotLeftUpZL{
    kApricotButtonZL, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kApricotLeftDown{
    0, kDpadCentered, 128, 255, 128, 128};
constexpr ControllerReport kApricotLeftLeft{
    0, kDpadCentered, 0, 128, 128, 128};
constexpr ControllerReport kApricotLeftRight{
    0, kDpadCentered, 255, 128, 128, 128};
constexpr ControllerReport kApricotLeftRightZL{
    kApricotButtonZL, kDpadCentered, 255, 128, 128, 128};
constexpr ControllerReport kApricotLeftRightDown{
    0, kDpadCentered, 255, 255, 128, 128};
constexpr ControllerReport kApricotRightStickRight{
    0, kDpadCentered, 128, 128, 255, 128};
constexpr ControllerReport kApricotRightStickUp{
    0, kDpadCentered, 128, 128, 128, 0};

// Board-resident routine lifted verbatim from EndingCrystal's
// MaterialFarmMacro.h. 35 steps, 55550 ms duration, 200 ms loop gap.
inline constexpr MacroStep kApricotDenMacro[] = {
    {70, kApricotReportX},              // 1
    {350, kNeutralReport},              // 2
    {70, kApricotReportA},              // 3
    {500, kNeutralReport},              // 4
    {70, kApricotReportA},              // 5
    {300, kNeutralReport},              // 6
    {70, kApricotReportA},              // 7
    {900, kNeutralReport},              // 8
    {70, kApricotReportA},              // 9
    {6000, kNeutralReport},             // 10
    {740, kApricotLeftUp},              // 11
    {60, kApricotRightStickUp},         // 11
    {14250, kApricotReportZR},          // 12 Shoot
    {100, kApricotReportR3},            // 13
    {750, kApricotLeftDown},            // 14
    {12000, kApricotReportZR},          // 15 Shoot
    {150, kApricotLeftRightZL},         // 16
    {4500, kApricotLeftUpZL},           // 17
    {100, kApricotReportPlus},
    {100, kApricotLeftDown},
    {100, kApricotReportA},
    {300, kNeutralReport},
    {100, kApricotLeftRight},
    {100, kApricotReportA},
    {7000, kNeutralReport},
    {400, kApricotReportA},
    {800, kNeutralReport},
    {200, kApricotReportA},
    {2500, kNeutralReport},
    {200, kApricotReportA},
    {900, kNeutralReport},
    {200, kApricotReportA},
    {900, kNeutralReport},
    {200, kApricotReportA},
    {500, kNeutralReport},
};

inline constexpr size_t kApricotDenStepCount =
    sizeof(kApricotDenMacro) / sizeof(kApricotDenMacro[0]);
inline constexpr uint32_t kApricotDenDurationMs = 55550;
inline constexpr uint32_t kApricotDenLoopGapMs = 200;
inline constexpr uint32_t kApricotDenCycleMs =
    kApricotDenDurationMs + kApricotDenLoopGapMs;

constexpr uint32_t apricotDenDurationFromSteps() {
  uint32_t total = 0;
  for (const MacroStep& step : kApricotDenMacro) {
    total += step.durationMs;
  }
  return total;
}

static_assert(kApricotDenStepCount == 35,
              "Apricot Den routine must have 35 steps");
static_assert(apricotDenDurationFromSteps() == kApricotDenDurationMs,
              "Apricot Den routine duration changed");
static_assert(sizeof(kApricotDenMacro) <= 1024,
              "Embedded macro unexpectedly exceeds one KiB");

}  // namespace farmers