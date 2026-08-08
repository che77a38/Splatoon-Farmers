#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MacroEngine.h"

namespace farmers {

// switch_ESP32 button mask:
// B=bit 1, A=bit 2, X=bit 3, L=bit 4, R=bit 5.
constexpr uint16_t kButtonB = 1u << 1;
constexpr uint16_t kButtonA = 1u << 2;
constexpr uint16_t kButtonX = 1u << 3;
constexpr uint16_t kButtonL = 1u << 4;
constexpr uint16_t kButtonR = 1u << 5;

constexpr ControllerReport kReportA{
    kButtonA, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kReportB{
    kButtonB, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kReportX{
    kButtonX, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kLeftUp{
    0, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kLeftUpX{
    kButtonX, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kLeftUpL{
    kButtonL, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kLeftUpR{
    kButtonR, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kLeftRight{
    0, kDpadCentered, 255, 128, 128, 128};
constexpr ControllerReport kLeftDown{
    0, kDpadCentered, 128, 255, 128, 128};
constexpr ControllerReport kLeftLeft{
    0, kDpadCentered, 0, 128, 128, 128};

// Board-resident material farming routine. Keeping the full sequence in this
// constexpr array makes execution independent from the browser connection.
inline constexpr MacroStep kMaterialFarmMacro[] = {
    {170, kReportX},
    {580, kNeutralReport},
    {70, kReportA},
    {1390, kNeutralReport},
    {120, kReportA},
    {1260, kNeutralReport},
    {90, kReportA},
    {11440, kNeutralReport},
    {11250, kLeftUp},
    {230, kLeftUpX},
    {200, kLeftUp},
    {1280, kNeutralReport},
    {120, kReportB},
    {320, kNeutralReport},
    {110, kReportB},
    {360, kNeutralReport},
    {520, kLeftUp},
    {220, kLeftUpL},
    {1080, kLeftUp},
    {270, kLeftUpL},
    {820, kLeftUp},
    {130, kLeftUpR},
    {1510, kLeftUp},
    {2325, kNeutralReport},
    {20, kLeftRight},
    {1080, kLeftRight},
    {460, kLeftRight},
    {560, kLeftRight},
    {1100, kLeftRight},
    {1980, kNeutralReport},
    {1740, kLeftDown},
    {1090, kLeftRight},
    {2070, kLeftLeft},
    {2130, kNeutralReport},
    // 8. 收尾 + 多段 A 拍 — 每个延时在原版基础上 +100ms, 让 Splatoon 动画
// 喘息 (用户要求)。
    {340, kReportA},
    {220, kNeutralReport},
    {3320, kNeutralReport},
    {330, kReportA},
    {1840, kNeutralReport},
    {390, kReportA},
    {1970, kNeutralReport},
    {315, kReportA},
    {1970, kNeutralReport},
    {320, kReportA},
    {1720, kNeutralReport},
    {270, kReportA},
    {970, kNeutralReport},
    {340, kReportA},
};

inline constexpr size_t kMaterialFarmStepCount =
    sizeof(kMaterialFarmMacro) / sizeof(kMaterialFarmMacro[0]);
inline constexpr uint32_t kMaterialFarmDurationMs = 62410;
inline constexpr uint32_t kMaterialFarmLoopGapMs = 2585;
inline constexpr uint32_t kMaterialFarmCycleMs =
    kMaterialFarmDurationMs + kMaterialFarmLoopGapMs;

constexpr uint32_t materialFarmDurationFromSteps() {
  uint32_t total = 0;
  for (const MacroStep& step : kMaterialFarmMacro) {
    total += step.durationMs;
  }
  return total;
}

static_assert(kMaterialFarmStepCount == 48,
              "Material farming routine must have 48 steps");
static_assert(materialFarmDurationFromSteps() == kMaterialFarmDurationMs,
              "Material farming routine duration changed");
static_assert(sizeof(kMaterialFarmMacro) <= 1024,
              "Embedded macro unexpectedly exceeds one KiB");

}  // namespace farmers
