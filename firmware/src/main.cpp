#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <esp_log.h>

#include "ControllerReport.h"
#include "MaterialFarmMacro.h"
#include "ApricotDenMacro.h"
#include "ApricotDenInkbackMacro.h"
#include "MacroEngine.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "web_server.h"
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

// Persistent config (NVS) + WiFi AP/STA state machine + web server.
// These are owned at file scope so the existing serial-protocol handlers
// (handleLine, etc.) can route through the same code paths. Lifetime
// covers the whole app.
farmers::ConfigStore Config;
farmers::WifiManager Wifi;
farmers::WebServer Http;

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
  Serial.printf("[USER] cmd: %s\n", line);
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

// GPIO0 is the BOOT button on ESP32-S3-DevKitC-1 (active-low, internal
// pull-up needed). After the application boots we use it as a script
// selector: pressing it N times within the first kBootSelectWindowMs
// milliseconds after boot auto-starts the matching embedded script:
//   1 tap -> 杏棱巢穴 (material-farm, 48 steps)
//   2 taps -> 天妇罗巢穴 (apricot-den, 35 steps)
//   3 taps -> 天妇罗回墨 (apricot-den-inkback, 100 steps)
//   0 taps -> idle, wait for the web UI / serial command
// If BOOT is *held* for kBootResetWindowMs (5 s) — a much longer press
// than the tap counter debounce — we wipe the saved WiFi credentials
// and bounce the device back to AP provisioning mode. LED blinks while
// the hold timer is in flight so the user can see what's happening.
//
// kBootPin is configurable via -DBOOT_BUTTON_GPIO=<N> at build time
// so a board that wires the button to a different pin (e.g. some
// N16R8 board variants route BOOT to GPIO35 or GPIO3) can override
// the default without editing source.
#ifndef BOOT_BUTTON_GPIO
constexpr uint8_t kBootPin = 0;  // GPIO0 on standard DevKitC-1
#else
constexpr uint8_t kBootPin = BOOT_BUTTON_GPIO;
#endif
constexpr uint8_t kLedPin = 48;
constexpr uint32_t kBootSelectWindowMs = 3000;
// Combined select + reset window is 10 s. The first 3 s are the
// short-tap selector (1/2/3 taps -> script 1/2/3); the next 7 s
// accept a long-press for the WiFi reset. We deliberately make the
// long-press window wider than the strict "5 s hold" requirement so
// users who overshoot still hit the threshold — most buttons
// bounce on a 5-s hold, and 3 s of *cumulative* low time is
// already an unambiguous "I mean to reset this" gesture.
constexpr uint32_t kBootResetWindowMs = 7000;
// Threshold: cumulative LOW time. kBootResetWindowMs is the max
// total wait; kBootResetThresholdMs is what counts as a "long
// press" once detected. 3 s is forgiving without making accidental
// resets likely.
constexpr uint32_t kBootResetThresholdMs = 5000;
constexpr uint32_t kBootDebounceMs = 50;

// Count BOOT-button presses within the selection window. Returns 0..3.
// The window starts when setup() returns; we use millis() as a monotonic
// reference (rollover at ~50 days is irrelevant for a 3-second window).
//
// Debouncing strategy: wait for the line to go LOW, record the press,
// then wait for the line to go HIGH before counting another press. Any
// Shared BOOT-button helpers. These exist so readBootPressCount and
// waitForBootLongPress can share the debounce + edge-detection logic
// instead of each re-implementing the same poll loop (DRY).
namespace {

// Wait for BOOT to transition from HIGH to LOW (a fresh falling
// edge). Returns the millis() at the moment the debounced falling
// edge was observed. The function returns 0 if the deadline
// elapses before any falling edge. We require a sustained LOW
// read for kBootDebounceMs so a contact bounce does not register
// as a press.
uint32_t waitForBootPress(uint32_t deadlineMs) {
  const uint32_t start = millis();
  bool prev = HIGH;
  while (millis() < deadlineMs) {
    if (digitalRead(kBootPin) == LOW && prev == HIGH) {
      delay(kBootDebounceMs);
      if (digitalRead(kBootPin) == LOW) {
        return millis();
      }
    }
    prev = digitalRead(kBootPin);
    delay(5);
  }
  return 0;
}

// Wait for BOOT to return to HIGH (release) after a press started at
// pressStartMs. Returns the millis() at the moment the line settled
// HIGH for at least kBootDebounceMs. If the line never goes HIGH
// (i.e. the user is still holding the button), returns 0.
uint32_t waitForBootRelease(uint32_t pressStartMs) {
  uint32_t lastHighMs = 0;
  while (true) {
    if (digitalRead(kBootPin) == HIGH) {
      if (lastHighMs == 0) lastHighMs = millis();
      if (millis() - lastHighMs >= kBootDebounceMs) {
        return lastHighMs;
      }
    } else {
      lastHighMs = 0;
    }
    delay(5);
  }
}

// Result of the BOOT-button selection-window scan. The press
// counter increments on each tap; if the user held BOOT down past
// the end of the 3 s window we surface that as longHold==true so
// the caller can fall through to the long-press branch without
// having to wait for a second press.
struct BootSelection {
  uint32_t presses;        // number of taps detected
  bool longHold;          // true if BOOT was still down at end of window
  uint32_t pressStartMs;   // millis() at the moment BOOT first went LOW
                           // (0 if no press ever occurred)
};

// Wait for a continuous BOOT press whose cumulative low time
// reaches thresholdMs. Returns true on success, false if the
// window expired or the user released too soon. The threshold is
// anchored to the moment BOOT first went LOW (NOT to setup()),
// so a press of any length hits the threshold after
// thresholdMs of cumulative low time.
//
// startMs is the optional anchor for the press — if the user
// already pressed BOOT during the selection window and we know
// the press start, we use that as lowStartMs directly and skip
// waiting for a new falling edge. This avoids the "press counter
// drained BOOT and now we sit here waiting for a second press
// that never comes" race.
bool waitForBootHold(uint32_t thresholdMs, uint32_t startMs = 0) {
  uint32_t lowStartMs = 0;
  if (startMs != 0) {
    lowStartMs = startMs;
    Serial.printf("[BOOT] long-press: reusing press-start at %u ms\n",
                  (unsigned)startMs);
  } else {
    // Wait for a fresh falling edge. No outer deadline — we want
    // to give the user the full press duration to reach the
    // threshold.
    lowStartMs = waitForBootPress(UINT32_MAX);
    if (lowStartMs == 0) return false;
    Serial.printf("[BOOT] long-press: fresh press at %u ms\n",
                  (unsigned)lowStartMs);
  }
  // Sample BOOT every 5 ms; transient HIGHs shorter than
  // kBootDebounceMs are contact noise and do not reset the
  // accumulator. Sustained HIGH ends the press. The threshold is
  // cumulative low time.
  uint32_t lastHighMs = 0;
  while (true) {
    const uint32_t now = millis();
    if (digitalRead(kBootPin) == LOW) {
      lastHighMs = 0;
      if (now - lowStartMs >= thresholdMs) {
        Serial.printf("[BOOT] long-press reached at low-time=%u ms (threshold %u)\n",
                      (unsigned)(now - lowStartMs), thresholdMs);
        for (int i = 0; i < 3; ++i) {
          digitalWrite(kLedPin, LOW);
          delay(60);
          digitalWrite(kLedPin, HIGH);
          delay(60);
        }
        return true;
      }
    } else {
      if (lastHighMs == 0) lastHighMs = now;
      if (now - lastHighMs >= kBootDebounceMs) {
        Serial.printf("[BOOT] released at low-time=%u ms (needed %u)\n",
                      (unsigned)(lastHighMs - lowStartMs), thresholdMs);
        return false;
      }
    }
    delay(5);
  }
}

BootSelection readBootPressCount(uint32_t startMs) {
  const uint32_t deadline = startMs + kBootSelectWindowMs;
  BootSelection result{0, false, 0};
  while (millis() < deadline) {
    const uint32_t pressStart = waitForBootPress(deadline);
    if (pressStart == 0) {
      // Window expired with no press seen.
      return result;
    }
    if (result.pressStartMs == 0) {
      // The first press is what autoStartFromBoot needs to anchor
      // the long-press detection against; record it now.
      result.pressStartMs = pressStart;
    }
    // Visible feedback: short LED-off flash for a fresh tap.
    digitalWrite(kLedPin, LOW);
    delay(60);
    digitalWrite(kLedPin, HIGH);
    // Did the user hold the button past the 3 s window, or release
    // it for a tap? We wait for a release with a deadline at the
    // end of the selection window.
    const uint32_t releaseMs = waitForBootRelease(pressStart);
    if (releaseMs == 0 || releaseMs > deadline) {
      // The user is still holding BOOT past the window. Treat as
      // a long-hold gesture, not a script tap. Return immediately
      // so autoStartFromBoot can hand pressStartMs to the
      // long-press detector.
      result.longHold = true;
      return result;
    }
    result.presses += 1;
    delay(kBootDebounceMs);
  }
  return result;
}

// Wait for a continuous BOOT press whose cumulative low time
// reaches thresholdMs. Returns true on success, false if the
// window expired or the user released too soon. The threshold is
// anchored to the moment BOOT first went LOW (NOT to setup()),
// so a press of any length hits the threshold after
// thresholdMs of cumulative low time.
}  // namespace

// Auto-start the embedded script matching the BOOT-button press count. The
// press counter is consulted exactly once at boot, before the web UI is
// likely to connect — if the user wants web control they just don't
// press BOOT, or they can STOP the script at any time via the existing
// STOP serial command.
void autoStartFromBoot() {
  const uint32_t start = millis();
  // LED-on signals the 3-second selection window. The LED stays lit
  // the whole time so the user has a clear "you can press now"
  // indicator. readBootPressCount turns the LED off when it
  // detects the first falling edge (so the user sees their press
  // acknowledged). If the LED is still on when readBootPressCount
  // returns, no taps were registered and we move into the long-press
  // window.
  digitalWrite(kLedPin, LOW);
  // Step 1: 3-second script-selector window. Counts short taps.
  const BootSelection sel = readBootPressCount(start);
  digitalWrite(kLedPin, sel.presses > 0 ? LOW : HIGH);
  Serial.printf("[BOOT] selector window closed, presses=%u, longHold=%d\n",
                (unsigned)sel.presses, (int)sel.longHold);
  if (sel.presses > 0 && !sel.longHold) {
    // Real taps detected — pick the script.
    if (sel.presses == 1) Active = ActiveScript::kMaterial;
    else if (sel.presses == 2) Active = ActiveScript::kApricot;
    else /* sel.presses >= 3 */ Active = ActiveScript::kApricotInkback;
    stopAllMacros();
    farmers::MacroEngine& macro = activeMacro();
    macro.start(millis());
    flushMacroReport();
    digitalWrite(kLedPin, LOW);
    return;
  }
  // No taps (or the user held the button down). Step 2: continue
  // the long-press detection from where the press counter left off.
  // We pass sel.pressStartMs to waitForBootHold so the threshold
  // counter is anchored to the moment BOOT first went LOW, not to
  // the moment this function runs (which is milliseconds later).
  Serial.printf("[BOOT] entering long-press threshold=%u ms (anchor=%u)\n",
                (unsigned)kBootResetThresholdMs, (unsigned)sel.pressStartMs);
  if (waitForBootHold(kBootResetThresholdMs, sel.pressStartMs)) {
    Serial.println("[BOOT] long-press detected -> clearing WiFi credentials");
    Wifi.resetCredentials();
    digitalWrite(kLedPin, LOW);
  }
}

// Detect a continuous low on the BOOT line for at least threshold ms,
// starting from startMs. Returns true if the press was long enough. The
// LED is pulsed at 4 Hz during the hold so the user can see the timer
// tick. We abort early as soon as the user releases so accidental
// presses do not trigger a reset.
// (The old waitForBootLongPress implementation was removed when
// waitForBootHold landed; the new primitive lives in the anonymous
// namespace above and is what autoStartFromBoot calls.)

void setup() {
  // Open the control serial first so the WiFi banner is visible.
  ATT_CONTROL_SERIAL.begin(kControlBaudRate);
  // Silence the TinyUSB USBHID log spam BEFORE USB.begin() runs.
  // The library prints `SendReport(): not ready` at ERROR level every
  // ~5 ms while no Switch is plugged in, which would bury our own
  // Serial output. esp_log_level_set is exported by ESP-IDF 4.4's log
  // component; arduino-esp32 2.0.17 does not pull in the master
  // log-level setter but per-tag works once the SDK is initialised.
  esp_log_level_set("USBHID", ESP_LOG_NONE);
  Serial.println("[boot] setup() enter");
  // Note: the TinyUSB USBHID log spam is silenced inside
  // WifiManager::begin() (the first thing that runs in the WiFi
  // path), so the WiFi banner below is actually readable.
  pinMode(kBootPin, INPUT_PULLUP);
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, HIGH); // off (active-low LED)
  Gamepad.begin();
  USB.begin();
  applyReport(farmers::kNeutralReport);
  Serial.println("[boot] before Config.begin");
  Config.begin();
  Serial.println("[boot] after Config.begin");
  Wifi.begin(&Config);
  Http.begin(&Config, &Wifi);
  Serial.printf("[WiFi] mode=%s, status=%s, ip=%s, ap_ssid=%s, mdns=%s.local\n",
                Wifi.mode() == farmers::WifiMode::kSta       ? "sta" :
                Wifi.mode() == farmers::WifiMode::kStaConnecting ? "sta-connecting" : "ap",
                Wifi.statusMessage(), Wifi.localIp().c_str(),
                Wifi.apSsid().c_str(), Wifi.mdnsName().c_str());
  Serial.println("[boot] before autoStartFromBoot");
  // TEMPORARY: disabled for diagnostic. The user's new board has BOOT
  // on a different pin and the default GPIO0 waitForBootHold blocks
  // forever. Re-enable once -DBOOT_BUTTON_GPIO=<pin> is set.
  // autoStartFromBoot();
  Serial.println("[boot] autoStartFromBoot SKIPPED (BOOT pin unknown)");
  Serial.println("[boot] setup() done");
}

void loop() {
  readControlSerial();
  // Drive the WiFi reconnect / AP-fallback state machine. Cheap when the
  // connection is steady (kSta path is a single WiFi.status() check).
  Wifi.tick();
  // Drive the captive-portal DNS server + any pending deferred restart
  // from a /api/wifi or /api/reset POST.
  Http.tick();
  // Tick whichever script is active. The non-active engine stays stopped.
  // Skip the tick entirely while the host is driving raw frames via
  // STREAM — the engines are stopped at that point and ticking them would
  // be wasted work plus a stale flushMacroReport() could overwrite the
  // most recently streamed HID report.
  if (!streamMode) {
    activeMacro().tick(millis());
  }
  // The switch_ESP32 library's Gamepad.loop() is what floods the serial
  // with `SendReport(): not ready` every 5 ms while no Switch is
  // plugged in (HID host not ready). The vendor source is pre-compiled
  // inside .pio so we cannot patch it. We side-step the call entirely
  // and re-implement the report re-send ourselves: every 10 ms, if
  // the macro engine has a fresh report queued, push it via
  // Gamepad.write(). When no Switch is plugged in, write() returns
  // false and we stop trying for a beat before retrying.
  static uint32_t lastReportMs = 0;
  const uint32_t now = millis();
  if (now - lastReportMs >= 10) {
    lastReportMs = now;
    farmers::MacroEngine& macro = activeMacro();
    if (macro.consumeReportChanged()) {
      applyReport(macro.report());
    }
  }
  // flushMacroReport() and the old Gamepad.loop() are intentionally
  // removed: the loop's 10 ms re-send above is the only place that
  // touches Gamepad.write(). Skipping the vendor loop() eliminates the
  // 5 ms `SendReport(): not ready` log spam that buried the serial
  // output while no Switch is plugged in.
}
