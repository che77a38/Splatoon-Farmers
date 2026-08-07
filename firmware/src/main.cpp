#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "ControllerReport.h"
#include "MaterialFarmMacro.h"
#include "ApricotDenMacro.h"
#include "ApricotDenInkbackMacro.h"
#include "MacroEngine.h"
#include "switch_ESP32.h"

/*
 * Hardware topology:
 *   ESP32-S3 native USB (GPIO19 D-, GPIO20 D+) -> Nintendo Switch dock
 *   ESP32-S3 UART0 through the board's USB-UART bridge -> browser/PC
 *
 * This deliberately uses a UART-backed serial port. The native USB peripheral
 * is reserved for switch_ESP32's Nintendo Switch HID device.
 */
#ifndef ATT_CONTROL_SERIAL
#define ATT_CONTROL_SERIAL Serial
#endif

namespace {

constexpr uint32_t kControlBaudRate = 115200;
constexpr char kFirmwareVersion[] = "SplatoonFarmers/1.0.0";

NSGamepad Gamepad;
farmers::MacroEngine MaterialMacro(
    farmers::kMaterialFarmMacro, farmers::kMaterialFarmStepCount,
    farmers::kMaterialFarmLoopGapMs, true);
farmers::MacroEngine ApricotMacro(
    farmers::kApricotDenMacro, farmers::kApricotDenStepCount,
    farmers::kApricotDenLoopGapMs, true);
farmers::MacroEngine ApricotInkbackMacro(
    farmers::kApricotDenInkbackMacro, farmers::kApricotDenInkbackStepCount,
    farmers::kApricotDenInkbackLoopGapMs, true);

// Currently selected macro for START* commands. STOP is script-agnostic.
enum class ActiveScript : uint8_t { kMaterial, kApricot, kApricotInkback };
ActiveScript Active = ActiveScript::kMaterial;

farmers::MacroEngine& activeMacro() {
  switch (Active) {
    case ActiveScript::kApricotInkback: return ApricotInkbackMacro;
    case ActiveScript::kApricot:         return ApricotMacro;
    case ActiveScript::kMaterial:        return MaterialMacro;
  }
  return MaterialMacro;
}

const char* activeScriptName() {
  switch (Active) {
    case ActiveScript::kApricotInkback: return "apricot-den-inkback";
    case ActiveScript::kApricot:        return "apricot-den";
    case ActiveScript::kMaterial:       return "material-farm";
  }
  return "material-farm";
}

char LineBuffer[128];
size_t LineLength = 0;
bool LineOverflow = false;

// When true, the host is driving the bus via raw `R ...` frames (custom
// scripts). Macro engines are stopped and tick() is skipped each loop so
// they cannot re-emit HID reports. Set by `STREAM`, cleared by `STREAM_END`.
bool streamMode = false;

uint8_t clampAxis(unsigned long value) {
  return value > 255 ? 255 : static_cast<uint8_t>(value);
}

uint8_t normalizeDpad(unsigned long value) {
  if (value <= NSGAMEPAD_DPAD_UP_LEFT ||
      value == NSGAMEPAD_DPAD_CENTERED) {
    return static_cast<uint8_t>(value);
  }
  return NSGAMEPAD_DPAD_CENTERED;
}

void applyReport(const farmers::ControllerReport& report) {
  Gamepad.buttons(report.buttons & 0x3fff);
  Gamepad.dPad(normalizeDpad(report.dpad));
  Gamepad.leftXAxis(report.leftX);
  Gamepad.leftYAxis(report.leftY);
  Gamepad.rightXAxis(report.rightX);
  Gamepad.rightYAxis(report.rightY);
  Gamepad.write();
}

void applyRawReport(unsigned long buttons, unsigned long dpad,
                    unsigned long leftX, unsigned long leftY,
                    unsigned long rightX, unsigned long rightY) {
  const farmers::ControllerReport report{
      static_cast<uint16_t>(buttons & 0x3fff),
      normalizeDpad(dpad),
      clampAxis(leftX),
      clampAxis(leftY),
      clampAxis(rightX),
      clampAxis(rightY),
  };
  applyReport(report);
}

const char* phaseName(farmers::MacroPhase phase) {
  switch (phase) {
    case farmers::MacroPhase::kSteps:
      return "steps";
    case farmers::MacroPhase::kLoopGap:
      return "gap";
    default:
      return "idle";
  }
}

// Per-script metadata for emitState(). Returns the static descriptor matching
// the currently selected script so the JSON reflects whichever routine the
// caller is asking about.
struct ScriptMeta {
  const char* name;
  size_t stepCount;
  uint32_t durationMs;
  uint32_t loopGapMs;
  uint32_t cycleMs;
};

ScriptMeta activeMeta() {
  if (Active == ActiveScript::kApricotInkback) {
    return {"apricot-den-inkback", farmers::kApricotDenInkbackStepCount,
            farmers::kApricotDenInkbackDurationMs,
            farmers::kApricotDenInkbackLoopGapMs,
            farmers::kApricotDenInkbackCycleMs};
  }
  if (Active == ActiveScript::kApricot) {
    return {"apricot-den",       farmers::kApricotDenStepCount,
            farmers::kApricotDenDurationMs,
            farmers::kApricotDenLoopGapMs,
            farmers::kApricotDenCycleMs};
  }
  return {"material-farm",      farmers::kMaterialFarmStepCount,
          farmers::kMaterialFarmDurationMs,
          farmers::kMaterialFarmLoopGapMs,
          farmers::kMaterialFarmCycleMs};
}

void emitState(const char* type) {
  farmers::MacroEngine& macro = activeMacro();
  const ScriptMeta meta = activeMeta();
  const size_t visibleStep =
      macro.phase() == farmers::MacroPhase::kSteps ? macro.stepIndex() + 1 : 0;
  ATT_CONTROL_SERIAL.printf(
      "{\"type\":\"%s\",\"ok\":true,\"firmware\":\"%s\","
      "\"routine\":\"%s\",\"embedded\":true,\"state\":\"%s\","
      "\"phase\":\"%s\",\"step\":%u,\"steps\":%u,\"cycle\":%lu,"
      "\"duration_ms\":%lu,\"loop_gap_ms\":%lu,\"cycle_ms\":%lu}\n",
      type, kFirmwareVersion, meta.name,
      macro.running() ? "running" : "idle",
      phaseName(macro.phase()), static_cast<unsigned int>(visibleStep),
      static_cast<unsigned int>(meta.stepCount),
      static_cast<unsigned long>(macro.cycleCount()),
      static_cast<unsigned long>(meta.durationMs),
      static_cast<unsigned long>(meta.loopGapMs),
      static_cast<unsigned long>(meta.cycleMs));
}

void flushMacroReport() {
  farmers::MacroEngine& macro = activeMacro();
  if (macro.consumeReportChanged()) {
    applyReport(macro.report());
  }
}

// STOP is script-agnostic: stop whichever engine is currently running so a
// START_APRICOT followed by STOP still halts the apricot script even if the
// caller switches back to material-farm mid-stop.
void stopAllMacros() {
  MaterialMacro.stop();
  ApricotMacro.stop();
  ApricotInkbackMacro.stop();
  flushMacroReport();
}

void handleLine(char* line) {
  if (strcmp(line, "PING") == 0) {
    ATT_CONTROL_SERIAL.println("PONG");
    return;
  }
  if (strcmp(line, "HELLO") == 0 || strcmp(line, "INFO") == 0) {
    emitState("info");
    return;
  }
  if (strcmp(line, "STATUS") == 0) {
    emitState("status");
    return;
  }
  if (strcmp(line, "STOP") == 0) {
    stopAllMacros();
    emitState("status");
    return;
  }
  // Stream mode: stop every macro engine and let the host drive raw `R ...`
  // frames until STREAM_END. While streamMode is true the macro tick is
  // suppressed in loop() so a stopped engine cannot re-emit stale reports.
  if (strcmp(line, "STREAM") == 0) {
    stopAllMacros();
    streamMode = true;
    ATT_CONTROL_SERIAL.println("OK");
    return;
  }
  if (strcmp(line, "STREAM_END") == 0) {
    streamMode = false;
    applyReport(farmers::kNeutralReport);
    ATT_CONTROL_SERIAL.println("OK");
    return;
  }
  // Script selection. START / START_MATERIAL = material-farm (default);
  // START_APRICOT = apricot-den. Selecting a script always stops whichever
    // script is currently running so a stale engine never keeps emitting HID
    // reports after a switch.
  if (strcmp(line, "START") == 0 ||
      strcmp(line, "START_MATERIAL") == 0 ||
      strcmp(line, "START_DEFAULT") == 0) {
    Active = ActiveScript::kMaterial;
    stopAllMacros();
    farmers::MacroEngine& macro = activeMacro();
    macro.start(millis());
    flushMacroReport();
    emitState("status");
    return;
  }
  if (strcmp(line, "START_APRICOT") == 0 ||
      strcmp(line, "START2") == 0) {
    Active = ActiveScript::kApricot;
    stopAllMacros();
    farmers::MacroEngine& macro = activeMacro();
    macro.start(millis());
    flushMacroReport();
    emitState("status");
    return;
  }
  if (strcmp(line, "START_INKBACK") == 0 ||
      strcmp(line, "START3") == 0) {
    Active = ActiveScript::kApricotInkback;
    stopAllMacros();
    farmers::MacroEngine& macro = activeMacro();
    macro.start(millis());
    flushMacroReport();
    emitState("status");
    return;
  }
  if (strcmp(line, "SCRIPT") == 0) {
    ATT_CONTROL_SERIAL.printf("{\"script\":\"%s\"}\n", activeScriptName());
    return;
  }

  char command[8] = {0};
  unsigned long buttons = 0;
  unsigned long dpad = NSGAMEPAD_DPAD_CENTERED;
  unsigned long leftX = farmers::kAxisCentered;
  unsigned long leftY = farmers::kAxisCentered;
  unsigned long rightX = farmers::kAxisCentered;
  unsigned long rightY = farmers::kAxisCentered;
  const int parsed =
      sscanf(line, "%7s %lu %lu %lu %lu %lu %lu", command, &buttons, &dpad,
             &leftX, &leftY, &rightX, &rightY);

  if (parsed == 7 &&
      (strcmp(command, "R") == 0 || strcmp(command, "REPORT") == 0)) {
    // Raw reports power manual input and leave a fallback path for future
    // computer-loaded routines. Entering this mode stops both embedded
    // routines so neither continues to emit HID reports.
    //
    // In stream mode the macro engines are already stopped by STREAM, so the
    // unconditional stopAllMacros() call would be redundant. Skip it to keep
    // the per-frame path cheap: the browser sends tens of frames per second.
    if (!streamMode) {
      stopAllMacros();
      activeMacro().consumeReportChanged();
    }
    applyRawReport(buttons, dpad, leftX, leftY, rightX, rightY);
    ATT_CONTROL_SERIAL.println("OK");
    return;
  }

  ATT_CONTROL_SERIAL.println("ERR");
}

void readControlSerial() {
  while (ATT_CONTROL_SERIAL.available() > 0) {
    const char character = static_cast<char>(ATT_CONTROL_SERIAL.read());
    if (character == '\n' || character == '\r') {
      if (LineOverflow) {
        ATT_CONTROL_SERIAL.println("ERR");
      } else if (LineLength > 0) {
        LineBuffer[LineLength] = '\0';
        handleLine(LineBuffer);
      }
      LineLength = 0;
      LineOverflow = false;
      continue;
    }

    if (LineOverflow) {
      continue;
    }
    if (LineLength < sizeof(LineBuffer) - 1) {
      LineBuffer[LineLength++] = character;
    } else {
      LineOverflow = true;
    }
  }
}

}  // namespace

void setup() {
  ATT_CONTROL_SERIAL.begin(kControlBaudRate);
  Gamepad.begin();
  USB.begin();
  applyReport(farmers::kNeutralReport);
}

void loop() {
  readControlSerial();
  // Tick whichever script is active. The non-active engine stays stopped.
  // Skip the tick entirely while the host is driving raw frames via
  // STREAM — the engines are stopped at that point and ticking them would
  // be wasted work plus a stale flushMacroReport() could overwrite the
  // most recently streamed HID report.
  if (!streamMode) {
    activeMacro().tick(millis());
  }
  flushMacroReport();
  Gamepad.loop();
}
