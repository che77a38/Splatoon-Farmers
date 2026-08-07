import assert from "node:assert/strict";
import test from "node:test";

import {
  Script,
  ScriptRecorder,
  serializeScript,
  deserializeScript,
  stepIcon,
  stepToRCommand,
  newHold,
  newRelease,
  newDelay,
} from "../../web/editor.js";

test("Script totalMs sums every step duration", () => {
  const script = new Script({
    steps: [
      newHold(0b1010, 15, 100),
      newRelease(50),
      newDelay(1000),
    ],
  });
  assert.equal(script.totalMs(), 1150);
});

test("Script insertStep / removeStep / moveStep / duplicateStep", () => {
  const script = new Script({ steps: [newHold(1, 15, 100), newHold(2, 15, 200)] });
  script.insertStep(1, newHold(99, 15, 50));
  assert.equal(script.steps.length, 3);
  assert.equal(script.steps[1].buttons, 99);
  script.moveStep(0, 2);
  assert.equal(script.steps[2].buttons, 1);
  const dup = script.duplicateStep(1);
  assert.ok(dup);
  assert.equal(script.steps.length, 4);
  assert.equal(script.steps[1].buttons, script.steps[2].buttons);
  const removed = script.removeStep(0);
  assert.ok(removed);
  assert.equal(script.steps.length, 3);
});

test("Script clear / clone preserve independence", () => {
  const script = new Script({
    name: "demo",
    steps: [newHold(4, 0, 250)],
    repeat: true,
  });
  const clone = script.clone();
  clone.steps.push(newRelease(50));
  assert.equal(script.steps.length, 1);
  assert.equal(clone.steps.length, 2);
  script.clear();
  assert.equal(script.steps.length, 0);
  assert.equal(clone.steps.length, 2);
});

test("serializeScript round-trips through deserializeScript", () => {
  const original = new Script({
    name: "demo",
    repeat: true,
    steps: [
      newHold(0b1100, 2, 500),
      newDelay(250),
      newRelease(50),
    ],
  });
  const json = serializeScript(original);
  const restored = deserializeScript(json);
  assert.equal(restored.name, "demo");
  assert.equal(restored.repeat, true);
  assert.equal(restored.steps.length, 3);
  assert.deepEqual(restored.steps[0].sticks, [128, 128, 128, 128]);
  assert.equal(restored.steps[0].buttons, 0b1100);
  assert.equal(restored.steps[0].dpad, 2);
  assert.equal(restored.steps[0].durationMs, 500);
  assert.equal(restored.steps[1].type, "delay");
  assert.equal(restored.steps[2].type, "release");
});

test("deserializeScript drops invalid steps and clamps sticks", () => {
  const restored = deserializeScript({
    name: "x",
    steps: [
      { type: "hold", buttons: 1, dpad: 15, sticks: [999, -5, 128, 128], durationMs: 5 },
      { type: "unknown" },
      { type: "delay", durationMs: 999 },
    ],
  });
  assert.equal(restored.steps.length, 2);
  assert.equal(restored.steps[0].sticks[0], 255); // clamped
  assert.equal(restored.steps[0].sticks[1], 0); // clamped
  assert.equal(restored.steps[0].durationMs, 10); // floored
  assert.equal(restored.steps[1].durationMs, 999);
});

test("deserializeScript rejects malformed JSON", () => {
  assert.throws(() => deserializeScript("not json"), /无法解析脚本 JSON/);
  assert.throws(() => deserializeScript(JSON.stringify(null)), /脚本 JSON 必须是对象/);
});

test("stepIcon renders button names, dpad glyphs, delay, release", () => {
  assert.equal(stepIcon(newDelay(100)), "⏱ 延时");
  assert.equal(stepIcon(newRelease(50)), "松开");
  assert.equal(stepIcon(newHold(0, 15, 100)), "空");
  // buttons bit 1 = B, bit 2 = A → "B + A" (lowest bit first)
  assert.equal(stepIcon(newHold(0b110, 15, 100)), "B + A");
  assert.equal(stepIcon(newHold(0, 4, 100)), "↓");
  // X is bit 3, dpad right (→) is value 2 → "X + →"
  assert.equal(stepIcon(newHold(0b1000, 2, 100)), "X + →");
});

test("stepToRCommand formats hold/release and skips delay", () => {
  assert.equal(stepToRCommand(newHold(8, 0, 100)), "R 8 0 128 128 128 128");
  assert.equal(stepToRCommand(newRelease(50)), "R 0 15 128 128 128 128");
  assert.equal(stepToRCommand(newDelay(100)), null);
});

// --- ScriptRecorder --------------------------------------------------------

test("ScriptRecorder captures a press + release as hold/release pair", () => {
  const script = new Script();
  const recorder = new ScriptRecorder(script);
  recorder.start(0);
  recorder.applyActiveSet(new Set(["A"]), { buttons: 4, dpad: 15 });
  recorder.onRecordEvent({ type: "press", control: "A", source: "keyboard:KeyL", time: 0 });
  // Mid-hold the user presses another button; recorder should back-fill the
  // first hold's duration with the time-to-second-press.
  recorder.applyActiveSet(new Set(["A", "B"]), { buttons: 6, dpad: 15 });
  recorder.onRecordEvent({ type: "press", control: "B", source: "keyboard:KeyK", time: 120 });
  recorder.onRecordEvent({ type: "release", control: "B", source: "keyboard:KeyK", time: 170 });
  recorder.onRecordEvent({ type: "release", control: "A", source: "keyboard:KeyL", time: 220 });

  assert.equal(script.steps.length, 5);
  assert.equal(script.steps[0].type, "hold");
  assert.equal(script.steps[0].buttons, 4);
  assert.equal(script.steps[1].type, "delay");
  assert.equal(script.steps[1].durationMs, 120);
  assert.equal(script.steps[2].type, "hold");
  assert.equal(script.steps[2].buttons, 6);
  assert.equal(script.steps[2].durationMs, 50);
  assert.equal(script.steps[3].type, "release");
  assert.equal(script.steps[4].type, "release");
});

test("ScriptRecorder inserts delay steps for gaps > 50ms", () => {
  const script = new Script();
  const recorder = new ScriptRecorder(script);
  recorder.start(0);
  recorder.applyActiveSet(new Set(["A"]), { buttons: 4, dpad: 15 });
  recorder.onRecordEvent({ type: "press", control: "A", time: 0 });
  recorder.onRecordEvent({ type: "release", control: "A", time: 100 });
  // 500ms pause -> delay step
  recorder.applyActiveSet(new Set(["B"]), { buttons: 2, dpad: 15 });
  recorder.onRecordEvent({ type: "press", control: "B", time: 600 });

  const types = script.steps.map((s) => s.type);
  assert.deepEqual(types, ["hold", "release", "delay", "hold"]);
  assert.equal(script.steps[2].durationMs, 500);
});

test("ScriptRecorder is a no-op when inactive", () => {
  const script = new Script();
  const recorder = new ScriptRecorder(script);
  // No start() called
  recorder.onRecordEvent({ type: "press", control: "A", time: 0 });
  recorder.onRecordEvent({ type: "release", control: "A", time: 100 });
  assert.equal(script.steps.length, 0);
});

test("ScriptRecorder clear() closes any open hold", () => {
  const script = new Script();
  const recorder = new ScriptRecorder(script);
  recorder.start(0);
  recorder.applyActiveSet(new Set(["A"]), { buttons: 4, dpad: 15 });
  recorder.onRecordEvent({ type: "press", control: "A", time: 0 });
  recorder.onRecordEvent({ type: "clear", controls: ["A"], time: 300 });
  assert.equal(script.steps[0].durationMs, 300);
});