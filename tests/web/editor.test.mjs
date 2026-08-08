import assert from "node:assert/strict";
import test from "node:test";

import {
  Script,
  ScriptRecorder,
  ScriptRunner,
  serializeScript,
  deserializeScript,
  stepIcon,
  stepToRCommand,
  newHold,
  newRelease,
  newDelay,
  saveScriptToStorage,
  loadScriptFromStorage,
  scriptDownloadFilename,
  STEP_TEMPLATES,
  getStepTemplate,
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

// --- ScriptRunner ----------------------------------------------------------

function makeFakeTransport() {
  const sent = [];
  return {
    sent,
    send(cmd) {
      sent.push(cmd);
      return Promise.resolve();
    },
  };
}

test("ScriptRunner.play() sends STREAM then runs each step in order", async () => {
  const script = new Script({
    steps: [
      newHold(0b100, 15, 50),
      newRelease(50),
      newHold(0b010, 15, 50),
      newRelease(50),
    ],
  });
  const transport = makeFakeTransport();
  const ticks = [];
  // Fake clock + RAF: each cb tick advances the clock by the step duration
  // so the runner's `elapsed >= expectedDuration` check fires immediately.
  let nowMs = 0;
  const runner = new ScriptRunner({
    transport,
    getScript: () => script,
    requestFrame: (cb) => { ticks.push(() => { nowMs += 50; cb(); }); return ticks.length; },
    now: () => nowMs,
  });
  await runner.play();
  // 4 steps * 2 RAFs (one for runStep's own frame plus the advance) — actually
  // each step triggers one frame, so we need 4 ticks to traverse all 4 steps.
  for (let i = 0; i < 4; i += 1) ticks.shift()();
  // 1 STREAM + 4 R frames + finish() sends STREAM_END + neutral = 7 total.
  assert.equal(transport.sent.length, 7);
  assert.equal(transport.sent[0], "STREAM");
  assert.equal(transport.sent[1], "R 4 15 128 128 128 128");
  assert.equal(transport.sent[2], "R 0 15 128 128 128 128");
  assert.equal(transport.sent[3], "R 2 15 128 128 128 128");
  assert.equal(transport.sent[4], "R 0 15 128 128 128 128");
  runner.stop();
});

test("ScriptRunner.repeat=true loops indefinitely until stop()", async () => {
  const script = new Script({
    repeat: true,
    steps: [newHold(0b001, 15, 50), newRelease(50)],
  });
  const transport = makeFakeTransport();
  const ticks = [];
  let nowMs = 0;
  const runner = new ScriptRunner({
    transport,
    getScript: () => script,
    requestFrame: (cb) => { ticks.push(() => { nowMs += 50; cb(); }); return ticks.length; },
    now: () => nowMs,
  });
  await runner.play();
  // Drain 6 RAF ticks. Each tick advances one step, but the loop wraps
  // around so the actual number of emitted R frames depends on cycle
  // boundaries. The point is just that multiple cycles run and the runner
  // does not stop on its own.
  for (let i = 0; i < 6; i += 1) ticks.shift()();
  const rCountBeforeStop = transport.sent.filter((s) => s.startsWith("R ")).length;
  assert.ok(rCountBeforeStop >= 4, `repeat 应该发出多帧，实际 ${rCountBeforeStop}`);
  // STREAM should always be the first command.
  assert.equal(transport.sent[0], "STREAM");
  await runner.stop();
  assert.ok(transport.sent.includes("STREAM_END"));
  assert.ok(transport.sent.includes("R 0 15 128 128 128 128"));
  // No more R frames after stop().
  const rCountAfterStop = transport.sent.filter((s) => s.startsWith("R ")).length;
  for (let i = 0; i < 5; i += 1) {
    const cb = ticks.shift();
    if (cb) cb();
  }
  assert.equal(
    transport.sent.filter((s) => s.startsWith("R ")).length,
    rCountAfterStop,
    "stop() 之后不能再发 R 帧",
  );
});

test("ScriptRunner skips delay steps but advances the timeline", async () => {
  const script = new Script({
    steps: [newHold(1, 15, 50), newDelay(200), newRelease(50)],
  });
  const transport = makeFakeTransport();
  const ticks = [];
  // Single fake-clock cursor. Each tick represents one RAF callback.
  // Step 1: hold 50ms — needs 1 tick
  // Step 2: delay 200ms — needs 4 ticks (50ms each = 200ms total)
  // Step 3: release 50ms — needs 1 tick
  // Total: 6 ticks to finish.
  const advances = [50, 50, 50, 50, 50, 50];
  let idx = 0;
  let nowMs = 0;
  const runner = new ScriptRunner({
    transport,
    getScript: () => script,
    requestFrame: (cb) => {
      ticks.push(() => {
        nowMs += advances[idx++] || 50;
        cb();
      });
      return ticks.length;
    },
    now: () => nowMs,
  });
  await runner.play();
  for (let i = 0; i < 6; i += 1) ticks.shift()();
  // STREAM + 2 R frames (hold, release) + finish()'s STREAM_END + neutral.
  assert.equal(transport.sent.length, 5);
  assert.equal(transport.sent[0], "STREAM");
  assert.equal(transport.sent[1], "R 1 15 128 128 128 128");
  assert.equal(transport.sent[2], "R 0 15 128 128 128 128");
  assert.equal(transport.sent[3], "STREAM_END");
  assert.equal(transport.sent[4], "R 0 15 128 128 128 128");
});

test("ScriptRunner refuses to play an empty script", async () => {
  const script = new Script({ steps: [] });
  const transport = makeFakeTransport();
  const runner = new ScriptRunner({
    transport,
    getScript: () => script,
    requestFrame: () => 0,
  });
  await runner.play();
  assert.equal(transport.sent.length, 0);
  assert.equal(runner.isRunning(), false);
});

test("ScriptRunner.stop() is a no-op when not running", async () => {
  const transport = makeFakeTransport();
  const runner = new ScriptRunner({
    transport,
    getScript: () => new Script(),
    requestFrame: () => 0,
  });
  await runner.stop();
  assert.equal(transport.sent.length, 0);
});

// --- Persistence ----------------------------------------------------------
//
// Node has no localStorage, so each test installs a fresh in-memory stub on
// the global object. We never depend on the stub leaking between tests.

test("saveScriptToStorage / loadScriptFromStorage round-trips a Script", async () => {
  const original = globalThis.localStorage;
  const store = new Map();
  globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: (k) => store.delete(k),
    clear: () => store.clear(),
  };
  try {
    const script = new Script({
      name: "saved",
      repeat: true,
      steps: [newHold(8, 0, 500), newRelease(50), newDelay(1000)],
    });
    assert.equal(saveScriptToStorage(script), true);
    const restored = loadScriptFromStorage();
    assert.ok(restored);
    assert.equal(restored.name, "saved");
    assert.equal(restored.repeat, true);
    assert.equal(restored.steps.length, 3);
    assert.equal(restored.steps[0].buttons, 8);
    assert.equal(restored.steps[2].durationMs, 1000);
  } finally {
    if (original === undefined) delete globalThis.localStorage;
    else globalThis.localStorage = original;
  }
});

test("loadScriptFromStorage returns null when nothing is stored", () => {
  const original = globalThis.localStorage;
  globalThis.localStorage = {
    getItem: () => null,
    setItem: () => {},
    removeItem: () => {},
    clear: () => {},
  };
  try {
    assert.equal(loadScriptFromStorage(), null);
  } finally {
    if (original === undefined) delete globalThis.localStorage;
    else globalThis.localStorage = original;
  }
});

test("scriptDownloadFilename sanitizes unsafe characters", () => {
  const filename = scriptDownloadFilename(
    new Script({ name: "weird / name? with*chars" }),
  );
  assert.ok(!filename.includes("/"));
  assert.ok(!filename.includes("?"));
  assert.ok(filename.endsWith(".json"));
});

// --- STEP_TEMPLATES --------------------------------------------------------

test("STEP_TEMPLATES covers every face / shoulder / system key the manual UI exposes", () => {
  const ids = new Set(STEP_TEMPLATES.map((t) => t.id));
  // Face buttons
  ["press-A", "press-B", "press-X", "press-Y"].forEach((id) =>
    assert.ok(ids.has(id), `${id} missing from STEP_TEMPLATES`));
  // Shoulders
  ["press-L", "press-R", "press-ZL", "press-ZR"].forEach((id) =>
    assert.ok(ids.has(id), `${id} missing from STEP_TEMPLATES`));
  // System
  ["press-PLUS", "press-MINUS", "press-CAPTURE", "press-HOME",
    "press-L3", "press-R3"].forEach((id) =>
    assert.ok(ids.has(id), `${id} missing from STEP_TEMPLATES`));
});

test("getStepTemplate('press-PLUS', 250) yields a hold step with PLUS bit and 250ms", () => {
  const step = getStepTemplate("press-PLUS", 250);
  assert.ok(step);
  assert.equal(step.type, "hold");
  assert.equal(step.buttons, 1 << 9);
  assert.equal(step.dpad, 15);
  assert.equal(step.durationMs, 250);
  assert.deepEqual(step.sticks, [128, 128, 128, 128]);
});

test("getStepTemplate('release') defaults to a 100ms neutral release", () => {
  const step = getStepTemplate("release");
  assert.ok(step);
  assert.equal(step.type, "release");
  assert.equal(step.buttons, 0);
  assert.equal(step.dpad, 15);
  assert.equal(step.durationMs, 100);
});

test("getStepTemplate('delay') ignores duration and returns a 1000ms delay", () => {
  const step = getStepTemplate("delay", 5000);
  assert.ok(step);
  assert.equal(step.type, "delay");
  assert.equal(step.durationMs, 1000);
});

test("getStepTemplate returns null for unknown id", () => {
  assert.equal(getStepTemplate("does-not-exist"), null);
});