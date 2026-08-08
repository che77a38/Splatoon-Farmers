#include <assert.h>
#include <stdint.h>

#include "MaterialFarmMacro.h"
#include "MacroEngine.h"

using farmers::MacroEngine;
using farmers::MacroPhase;

namespace {

void testEmbeddedMacroMetadata() {
  assert(farmers::kMaterialFarmStepCount == 48);
  assert(farmers::materialFarmDurationFromSteps() == 62410);
  assert(farmers::kMaterialFarmLoopGapMs == 2585);
  assert(farmers::kMaterialFarmCycleMs == 64995);
  assert(sizeof(farmers::kMaterialFarmMacro) == 576);
}

void testEveryStepAndLoopGap() {
  MacroEngine engine(farmers::kMaterialFarmMacro,
                     farmers::kMaterialFarmStepCount,
                     farmers::kMaterialFarmLoopGapMs, true);

  uint32_t now = 1000;
  engine.start(now);
  assert(engine.running());
  assert(engine.phase() == MacroPhase::kSteps);
  assert(engine.stepIndex() == 0);
  assert(engine.report() == farmers::kReportX);
  assert(engine.consumeReportChanged());
  assert(!engine.consumeReportChanged());

  for (size_t index = 0; index < farmers::kMaterialFarmStepCount; ++index) {
    const auto& step = farmers::kMaterialFarmMacro[index];
    engine.tick(now + step.durationMs - 1);
    assert(engine.phase() == MacroPhase::kSteps);
    assert(engine.stepIndex() == index);

    now += step.durationMs;
    engine.tick(now);
    if (index + 1 < farmers::kMaterialFarmStepCount) {
      assert(engine.phase() == MacroPhase::kSteps);
      assert(engine.stepIndex() == index + 1);
      assert(engine.report() == farmers::kMaterialFarmMacro[index + 1].report);
    }
  }

  assert(engine.running());
  assert(engine.phase() == MacroPhase::kLoopGap);
  assert(engine.report() == farmers::kNeutralReport);
  assert(engine.cycleCount() == 1);

  engine.tick(now + farmers::kMaterialFarmLoopGapMs - 1);
  assert(engine.phase() == MacroPhase::kLoopGap);

  now += farmers::kMaterialFarmLoopGapMs;
  engine.tick(now);
  assert(engine.phase() == MacroPhase::kSteps);
  assert(engine.stepIndex() == 0);
  assert(engine.report() == farmers::kReportX);
  assert(engine.cycleCount() == 1);
}

void testStopAlwaysNeutralizes() {
  MacroEngine engine(farmers::kMaterialFarmMacro,
                     farmers::kMaterialFarmStepCount,
                     farmers::kMaterialFarmLoopGapMs, true);
  engine.start(42);
  engine.consumeReportChanged();
  engine.stop();

  assert(!engine.running());
  assert(engine.phase() == MacroPhase::kIdle);
  assert(engine.report() == farmers::kNeutralReport);
  assert(engine.consumeReportChanged());
}

void testMillisWraparound() {
  const farmers::MacroStep steps[] = {
      {20, farmers::kReportA},
      {10, farmers::kNeutralReport},
  };
  MacroEngine engine(steps, 2, 5, true);
  const uint32_t start = UINT32_MAX - 9;
  engine.start(start);
  engine.tick(10);
  assert(engine.stepIndex() == 1);
  engine.tick(20);
  assert(engine.phase() == MacroPhase::kLoopGap);
  engine.tick(25);
  assert(engine.phase() == MacroPhase::kSteps);
  assert(engine.stepIndex() == 0);
}

}  // namespace

int main() {
  testEmbeddedMacroMetadata();
  testEveryStepAndLoopGap();
  testStopAlwaysNeutralizes();
  testMillisWraparound();
  return 0;
}
