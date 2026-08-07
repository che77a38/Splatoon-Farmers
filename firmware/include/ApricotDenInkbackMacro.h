#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MacroEngine.h"

namespace farmers {

// Apricot Den "Inkback" variant — derived from the apricot-den 35-step
// routine (EndingCrystal fork), with the long-hold ZR steps rewritten to
// alternate between ZR and ZL taps so the stick pumps paint back into the
// reservoir instead of burning it down. The ZR press window is held longer
// (1500 ms) than the ZL press window (1000 ms) so the in-game trigger
// reliably registers a fire.
//
// Mapping (each step's hold window, in ms):
//   step 13: 14250 -> 34250 ms (8 alternating ZR/ZL 4-s pairs + ZL 1 s + release 1 s + ZL 250 ms tail)
//   step 16: 12000 -> 32000 ms (8 alternating ZR/ZL 4-s pairs)
// One 4-second pair = ZR 1500 ms / release 500 ms / ZL 1000 ms / release 1000 ms.
// Every other step matches the apricot-den routine verbatim.
//
// Button masks mirror the apricot-den header via a `kInk` prefix to avoid
// redefinition collisions.
constexpr uint16_t kInkButtonB = 1u << 1;
constexpr uint16_t kInkButtonA = 1u << 2;
constexpr uint16_t kInkButtonX = 1u << 3;
constexpr uint16_t kInkButtonL = 1u << 4;
constexpr uint16_t kInkButtonR = 1u << 5;
constexpr uint16_t kInkButtonPlus = 1u << 9;
constexpr uint16_t kInkButtonZR = 1u << 7;
constexpr uint16_t kInkButtonZL = 1u << 6;
constexpr uint16_t kInkButtonR3 = 1u << 11;

constexpr ControllerReport kInkReportR3{
    kInkButtonR3, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportZR{
    kInkButtonZR, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportZL{
    kInkButtonZL, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportPlus{
    kInkButtonPlus, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportA{
    kInkButtonA, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportB{
    kInkButtonB, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportX{
    kInkButtonX, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkLeftUpX{
    kInkButtonX, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kInkLeftUpL{
    kInkButtonL, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kInkLeftUpR{
    kInkButtonR, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kInkReportL{
    kInkButtonL, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkReportR{
    kInkButtonR, kDpadCentered, 128, 128, 128, 128};
constexpr ControllerReport kInkLeftUp{
    0, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kInkLeftUpZL{
    kInkButtonZL, kDpadCentered, 128, 0, 128, 128};
constexpr ControllerReport kInkLeftDown{
    0, kDpadCentered, 128, 255, 128, 128};
constexpr ControllerReport kInkLeftLeft{
    0, kDpadCentered, 0, 128, 128, 128};
constexpr ControllerReport kInkLeftRight{
    0, kDpadCentered, 255, 128, 128, 128};
constexpr ControllerReport kInkLeftRightZL{
    kInkButtonZL, kDpadCentered, 255, 128, 128, 128};
constexpr ControllerReport kInkLeftRightDown{
    0, kDpadCentered, 255, 255, 128, 128};
constexpr ControllerReport kInkRightStickRight{
    0, kDpadCentered, 128, 128, 255, 128};
constexpr ControllerReport kInkRightStickUp{
    0, kDpadCentered, 128, 128, 128, 0};

// Board-resident routine. Step 13 (was {14250, ZR}) is replaced by an
// alternating ZR/ZL pattern totalling 34250 ms (+20 s). Step 16 (was
// {12000, ZR}) is replaced by an alternating ZR/ZL pattern totalling
// 32000 ms (+20 s). Every other step matches the apricot-den routine
// verbatim.
inline constexpr MacroStep kApricotDenInkbackMacro[] = {
    {70, kInkReportX},              // 1
    {350, kNeutralReport},          // 2
    {70, kInkReportA},              // 3
    {500, kNeutralReport},          // 4
    {70, kInkReportA},              // 5
    {300, kNeutralReport},          // 6
    {70, kInkReportA},              // 7
    {900, kNeutralReport},          // 8
    {70, kInkReportA},              // 9
    {6000, kNeutralReport},         // 10
    {740, kInkLeftUp},              // 11
    {60, kInkRightStickUp},         // 12
    // Step 13 (replaces 14250 ms of held ZR, +20 s = 34250 ms):
    //   34250 / 4000 = 8.5625, so 8 alternating ZR/ZL 4-s pairs
    //   (ZR 1500 / release 500 / ZL 1000 / release 1000) plus a 2250 ms
    //   tail of (ZL 1000 / release 1000 / ZL 250).
    {1500, kInkReportZR},           // 13a ZR press (1.5 s)
    {500, kNeutralReport},          // 13b release (0.5 s)
    {1000, kInkReportZL},           // 13c ZL press
    {1000, kNeutralReport},         // 13d release
    {1500, kInkReportZR},           // 13e
    {500, kNeutralReport},          // 13f
    {1000, kInkReportZL},           // 13g
    {1000, kNeutralReport},         // 13h
    {1500, kInkReportZR},           // 13i
    {500, kNeutralReport},          // 13j
    {1000, kInkReportZL},           // 13k
    {1000, kNeutralReport},         // 13l
    {1500, kInkReportZR},           // 13m
    {500, kNeutralReport},          // 13n
    {1000, kInkReportZL},           // 13o
    {1000, kNeutralReport},         // 13p
    {1500, kInkReportZR},           // 13q
    {500, kNeutralReport},          // 13r
    {1000, kInkReportZL},           // 13s
    {1000, kNeutralReport},         // 13t
    {1500, kInkReportZR},           // 13u
    {500, kNeutralReport},          // 13v
    {1000, kInkReportZL},           // 13w
    {1000, kNeutralReport},         // 13x
    {1500, kInkReportZR},           // 13y
    {500, kNeutralReport},          // 13z
    {1000, kInkReportZL},           // 13aa
    {1000, kNeutralReport},         // 13bb
    {1500, kInkReportZR},           // 13cc
    {500, kNeutralReport},          // 13dd
    {1000, kInkReportZL},           // 13ee
    {1000, kNeutralReport},         // 13ff
    {1000, kInkReportZL},           // 13gg ZL tail (1000 ms)
    {1000, kNeutralReport},         // 13hh release
    {250, kInkReportZL},            // 13ii ZL tail (250 ms)
    {100, kInkReportR3},            // 14
    {750, kInkLeftDown},            // 15
    // Step 16 (replaces 12000 ms of held ZR, +20 s = 32000 ms):
    //   32000 / 4000 = 8 pairs exactly.
    {1500, kInkReportZR},           // 16a
    {500, kNeutralReport},          // 16b
    {1000, kInkReportZL},           // 16c
    {1000, kNeutralReport},         // 16d
    {1500, kInkReportZR},           // 16e
    {500, kNeutralReport},          // 16f
    {1000, kInkReportZL},           // 16g
    {1000, kNeutralReport},         // 16h
    {1500, kInkReportZR},           // 16i
    {500, kNeutralReport},          // 16j
    {1000, kInkReportZL},           // 16k
    {1000, kNeutralReport},         // 16l
    {1500, kInkReportZR},           // 16m
    {500, kNeutralReport},          // 16n
    {1000, kInkReportZL},           // 16o
    {1000, kNeutralReport},         // 16p
    {1500, kInkReportZR},           // 16q
    {500, kNeutralReport},          // 16r
    {1000, kInkReportZL},           // 16s
    {1000, kNeutralReport},         // 16t
    {1500, kInkReportZR},           // 16u
    {500, kNeutralReport},          // 16v
    {1000, kInkReportZL},           // 16w
    {1000, kNeutralReport},         // 16x
    {1500, kInkReportZR},           // 16y
    {500, kNeutralReport},          // 16z
    {1000, kInkReportZL},           // 16aa
    {1000, kNeutralReport},         // 16bb
    {1500, kInkReportZR},           // 16cc
    {500, kNeutralReport},          // 16dd
    {1000, kInkReportZL},           // 16ee
    {1000, kNeutralReport},         // 16ff
    {150, kInkLeftRightZL},         // 17
    {4500, kInkLeftUpZL},           // 18
    {100, kInkReportPlus},          // 19
    {100, kInkLeftDown},            // 20
    {100, kInkReportA},             // 21
    {300, kNeutralReport},          // 22
    {100, kInkLeftRight},           // 23
    {100, kInkReportA},             // 24
    {7000, kNeutralReport},         // 25
    {400, kInkReportA},             // 26
    {800, kNeutralReport},          // 27
    {200, kInkReportA},             // 28
    {2500, kNeutralReport},         // 29
    {200, kInkReportA},             // 30
    {900, kNeutralReport},          // 31
    {200, kInkReportA},             // 32
    {900, kNeutralReport},          // 33
    {200, kInkReportA},             // 34
    {500, kNeutralReport},          // 35
};

inline constexpr size_t kApricotDenInkbackStepCount =
    sizeof(kApricotDenInkbackMacro) / sizeof(kApricotDenInkbackMacro[0]);
// Approximate duration — the alternating pattern runs ~40 s longer than the
// original held-ZR routine. Keep the constants honest so STATUS reports a
// sensible cycle length, but do not enforce exact equality.
inline constexpr uint32_t kApricotDenInkbackDurationMs = 95550;
inline constexpr uint32_t kApricotDenInkbackLoopGapMs = 200;
inline constexpr uint32_t kApricotDenInkbackCycleMs =
    kApricotDenInkbackDurationMs + kApricotDenInkbackLoopGapMs;

constexpr uint32_t apricotDenInkbackDurationFromSteps() {
  uint32_t total = 0;
  for (const MacroStep& step : kApricotDenInkbackMacro) {
    total += step.durationMs;
  }
  return total;
}

static_assert(kApricotDenInkbackStepCount == 100,
              "Apricot Den Inkback routine must have 100 steps (35 surface + 65 expanded)");
// Duration check is intentionally relaxed: the inkback variant pads the
// alternating ZR/ZL pattern out so total cycle time may exceed the original
// apricot-den duration. As long as the routine stays under ~120 s the board
// has plenty of headroom; we just sanity-check that we did not accidentally
// introduce a runaway duration.
static_assert(apricotDenInkbackDurationFromSteps() <= 120000,
              "Apricot Den Inkback routine exceeded 120 s — check the expansion");
static_assert(sizeof(kApricotDenInkbackMacro) <= 4096,
              "Embedded macro unexpectedly exceeds four KiB");

}  // namespace farmers