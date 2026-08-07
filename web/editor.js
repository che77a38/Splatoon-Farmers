// Script Editor — pure JS module with no DOM dependency, so the model can
// be exercised under Node's `node:test` without jsdom. The UI rendering
// helpers (`renderStepRow`, `stepIcon`) are also DOM-free; they build
// strings instead of DOM nodes so they remain testable.
//
// Step shape:
//   { type: 'hold',    buttons, dpad, sticks: [lx,ly,rx,ry], durationMs }
//   { type: 'release', buttons: 0, dpad: 15, sticks: [128,128,128,128], durationMs }
//   { type: 'delay',   durationMs }
//
// hold: maintain buttons/dpad/sticks for `durationMs`
// release: emit neutral report for `durationMs`
// delay: do nothing for `durationMs` (no HID frame is sent)

import { BUTTON_BITS, DPAD_CONTROLS } from "./manual-input.js";

// Step kinds we understand. Anything else is rejected at deserialization.
const STEP_TYPES = Object.freeze(["hold", "release", "delay"]);

// Reverse BUTTON_BITS map: bit index -> name. Computed once at module load.
const BUTTON_NAMES = Object.freeze(
  Object.entries(BUTTON_BITS).reduce((acc, [name, bit]) => {
    acc[bit] = name;
    return acc;
  }, {}),
);

// D-pad value -> short label. Mirrors dpadValue() in manual-input.js.
const DPAD_LABELS = Object.freeze({
  0: "↑",
  1: "↗",
  2: "→",
  3: "↘",
  4: "↓",
  5: "↙",
  6: "←",
  7: "↖",
  15: "·",
});

export function stepIcon(step) {
  if (!step) return "";
  if (step.type === "delay") return "⏱ 延时";
  if (step.type === "release") return "松开";
  if (step.type === "hold") {
    const names = [];
    for (let bit = 0; bit < 14; bit += 1) {
      if (step.buttons & (1 << bit)) {
        names.push(BUTTON_NAMES[bit] || `bit${bit}`);
      }
    }
    if (step.dpad !== undefined && step.dpad !== 15 && DPAD_LABELS[step.dpad]) {
      names.push(DPAD_LABELS[step.dpad]);
    }
    return names.length ? names.join(" + ") : "空";
  }
  return "?";
}

// Compile a step into the wire format the firmware consumes. Returns null
// for delay steps (the runner waits via requestAnimationFrame timing).
export function stepToRCommand(step) {
  if (step.type !== "hold" && step.type !== "release") return null;
  const sticks = step.sticks || [128, 128, 128, 128];
  const buttons = step.buttons | 0;
  const dpad = step.dpad | 0;
  return `R ${buttons} ${dpad} ${sticks[0]} ${sticks[1]} ${sticks[2]} ${sticks[3]}`;
}

function newHold(buttons = 0, dpad = 15, durationMs = 100) {
  return {
    type: "hold",
    buttons,
    dpad,
    sticks: [128, 128, 128, 128],
    durationMs,
  };
}

function newRelease(durationMs = 50) {
  return {
    type: "release",
    buttons: 0,
    dpad: 15,
    sticks: [128, 128, 128, 128],
    durationMs,
  };
}

function newDelay(durationMs = 100) {
  return { type: "delay", durationMs };
}

export class Script {
  constructor({ name = "未命名", steps = [], repeat = false } = {}) {
    this.name = name;
    this.steps = steps.slice();
    this.repeat = repeat;
  }

  totalMs() {
    let total = 0;
    for (const step of this.steps) {
      total += (step.durationMs | 0) || 0;
    }
    return total;
  }

  addStep(step) {
    this.steps.push(step);
  }

  insertStep(index, step) {
    const at = Math.max(0, Math.min(index, this.steps.length));
    this.steps.splice(at, 0, step);
  }

  removeStep(index) {
    if (index < 0 || index >= this.steps.length) return null;
    const [removed] = this.steps.splice(index, 1);
    return removed;
  }

  moveStep(fromIndex, toIndex) {
    if (fromIndex === toIndex) return;
    if (fromIndex < 0 || fromIndex >= this.steps.length) return;
    const target = Math.max(0, Math.min(toIndex, this.steps.length - 1));
    const [item] = this.steps.splice(fromIndex, 1);
    this.steps.splice(target, 0, item);
  }

  duplicateStep(index) {
    if (index < 0 || index >= this.steps.length) return null;
    const copy = JSON.parse(JSON.stringify(this.steps[index]));
    this.steps.splice(index + 1, 0, copy);
    return copy;
  }

  clear() {
    this.steps = [];
  }

  clone() {
    return new Script({
      name: this.name,
      steps: this.steps.map((s) => JSON.parse(JSON.stringify(s))),
      repeat: this.repeat,
    });
  }

  toJSON() {
    return serializeScript(this);
  }
}

export function serializeScript(script) {
  return JSON.stringify({
    name: script.name,
    repeat: script.repeat,
    steps: script.steps.map((step) => ({
      type: step.type,
      buttons: step.buttons | 0,
      dpad: step.dpad | 0,
      sticks: (step.sticks || [128, 128, 128, 128]).slice(),
      durationMs: step.durationMs | 0,
    })),
  });
}

export function deserializeScript(json) {
  let parsed;
  try {
    parsed = typeof json === "string" ? JSON.parse(json) : json;
  } catch (error) {
    throw new Error(`无法解析脚本 JSON: ${error.message}`);
  }
  if (!parsed || typeof parsed !== "object") {
    throw new Error("脚本 JSON 必须是对象");
  }
  const steps = Array.isArray(parsed.steps) ? parsed.steps : [];
  const cleanSteps = [];
  for (const raw of steps) {
    if (!raw || !STEP_TYPES.includes(raw.type)) continue;
    if (raw.type === "delay") {
      cleanSteps.push(newDelay(Math.max(10, raw.durationMs | 0 || 100)));
      continue;
    }
    const sticks = Array.isArray(raw.sticks) && raw.sticks.length === 4
      ? raw.sticks.map((v) => Math.max(0, Math.min(255, v | 0)))
      : [128, 128, 128, 128];
    cleanSteps.push({
      type: raw.type,
      buttons: raw.buttons | 0,
      dpad: raw.dpad | 0,
      sticks,
      durationMs: Math.max(10, raw.durationMs | 0 || 100),
    });
  }
  return new Script({
    name: typeof parsed.name === "string" ? parsed.name : "未命名",
    repeat: Boolean(parsed.repeat),
    steps: cleanSteps,
  });
}

export function emptyScript() {
  return new Script();
}

export { newHold, newRelease, newDelay };

// Recorder that turns raw press / release events into Script steps. The
// recorder owns no timers; the host (app.js) provides `time` via the
// `onRecordEvent` payload, so the class is fully testable under Node
// with fake clocks.
//
// Mapping rules (kept intentionally simple for v1):
//   press     -> push a 'hold' step with the *current* bitmap + dpad
//                (computed from the supplied activeControls set)
//   release   -> back-fill the previous hold's durationMs with the actual
//                elapsed time, then push a short 'release' step
//   gap > 50ms between two events -> push a 'delay' step
//   clear     -> close out the current hold (treat as immediate release)
//
// Analog sticks are always centered (128,128,128,128) since v1 has no
// stick input source.
export class ScriptRecorder {
  constructor(script) {
    this.script = script;
    this.active = false;
    this.lastEventTime = 0;
    // We track the bitmap ourselves rather than recomputing from active
    // controls because `onRecordEvent` only ships the *delta* control name,
    // not the full set. The host calls `applyActiveSet()` whenever the
    // active set changes; press/release events then mutate this snapshot.
    this.buttons = 0;
    this.dpad = 15;
  }

  start(time = Date.now()) {
    this.script.steps = [];
    this.lastEventTime = time;
    this.active = true;
    this.buttons = 0;
    this.dpad = 15;
  }

  stop() {
    this.active = false;
  }

  // Host-driven: call this whenever ManualInputState's activeControls set
  // changes so the recorder keeps an up-to-date bitmap. The recorder only
  // reads the set when a press event arrives.
  applyActiveSet(activeControls, { buttons, dpad }) {
    if (activeControls === undefined) return;
    if (buttons !== undefined) this.buttons = buttons | 0;
    if (dpad !== undefined) this.dpad = dpad | 0;
  }

  onRecordEvent(event) {
    if (!this.active) return;
    const { type, time = Date.now() } = event;
    const gap = time - this.lastEventTime;
    // A delay step only makes sense between two "open" actions — i.e.
    // between consecutive presses. Releases already close the previous hold
    // and are themselves short (50 ms), so inserting a delay before them
    // would just produce meaningless extra frames at run time.
    if (type === "press" && gap > 50 && this.script.steps.length > 0) {
      this.script.steps.push(newDelay(Math.round(gap)));
    }

    if (type === "press") {
      this.script.steps.push({
        type: "hold",
        buttons: this.buttons,
        dpad: this.dpad,
        sticks: [128, 128, 128, 128],
        durationMs: 100, // back-filled by the matching release
        _startedAt: time,
      });
    } else if (type === "release") {
      // Back-fill the most recent hold's actual duration.
      for (let i = this.script.steps.length - 1; i >= 0; i -= 1) {
        const step = this.script.steps[i];
        if (step.type === "hold" && step._startedAt !== undefined) {
          step.durationMs = Math.max(50, Math.round(time - step._startedAt));
          delete step._startedAt;
          break;
        }
      }
      this.script.steps.push(newRelease(50));
    } else if (type === "clear") {
      // Force-close any open hold.
      for (let i = this.script.steps.length - 1; i >= 0; i -= 1) {
        const step = this.script.steps[i];
        if (step.type === "hold" && step._startedAt !== undefined) {
          step.durationMs = Math.max(50, Math.round(time - step._startedAt));
          delete step._startedAt;
        }
      }
    }

    this.lastEventTime = time;
  }
}

// Stream runner: drives a Script's steps to the firmware via raw `R ...`
// frames sent through the Web Serial transport. Each hold/release step is
// pushed at the start of its durationMs window; the runner waits for the
// window to elapse (poll via requestAnimationFrame / injected clock) before
// advancing.
//
// `delay` steps do not produce a frame — they only consume time. After the
// last step the runner honors `script.repeat` and loops back to the start
// instead of stopping.
//
// The transport only needs `send(command)` and the runner keeps an
// `onProgress` hook so the host can update a progress bar. We intentionally
// do NOT subscribe to firmware responses — `R` frames only get `OK` back
// and chasing that path adds latency without correctness benefit.
export class ScriptRunner {
  constructor({ transport, getScript, requestFrame = null, onProgress = null, now = null } = {}) {
    if (!transport || typeof transport.send !== "function") {
      throw new Error("ScriptRunner 需要带 send() 方法的 transport");
    }
    if (typeof getScript !== "function") {
      throw new Error("ScriptRunner 需要 getScript 函数返回当前 Script");
    }
    this.transport = transport;
    this.getScript = getScript;
    // requestFrame(cb) -> handle. cb receives no arguments; the runner
    // tracks elapsed time internally via now().
    this.requestFrame = requestFrame || ((cb) => requestAnimationFrameSafe(cb));
    this.cancelFrame = null;
    this.onProgress = typeof onProgress === "function" ? onProgress : null;
    this.now = now || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    this.running = false;
    this.stepIndex = 0;
    this.stepStartMs = 0;
    this.frameHandle = null;
  }

  isRunning() {
    return this.running;
  }

  // Issue the firmware STREAM handshake and begin playback. Returns a
  // promise that resolves once the transport accepted the STREAM command.
  async play() {
    if (this.running) return;
    const script = this.getScript();
    if (!script || script.steps.length === 0) return;
    await this.transport.send("STREAM");
    this.running = true;
    this.stepIndex = 0;
    this.runStep();
  }

  // Cancel playback. Sends STREAM_END followed by a neutral frame so the
  // device settles back to a known state regardless of where the runner
  // was mid-step.
  async stop() {
    if (!this.running) return;
    this.running = false;
    if (this.frameHandle !== null && this.cancelFrame) {
      this.cancelFrame(this.frameHandle);
    }
    this.frameHandle = null;
    try {
      await this.transport.send("STREAM_END");
      await this.transport.send("R 0 15 128 128 128 128");
    } catch {
      // best-effort cleanup
    }
  }

  // Internal: emit the current step's frame (if any) and schedule the next
  // advancement. Called once per step boundary, not per frame.
  runStep() {
    if (!this.running) return;
    const script = this.getScript();
    const step = script.steps[this.stepIndex];
    if (!step) {
      this.finish(script);
      return;
    }
    const cmd = stepToRCommand(step);
    if (cmd) {
      this.transport.send(cmd).catch(() => {});
    }
    if (this.onProgress) {
      this.onProgress({
        stepIndex: this.stepIndex,
        totalSteps: script.steps.length,
      });
    }
    this.stepStartMs = this.now();
    this.scheduleAdvance(step.durationMs | 0);
  }

  // Hook the host-supplied requestFrame. The callback is invoked once per
  // animation frame; it decides whether the current step's window has
  // elapsed and either reschedules or advances.
  scheduleAdvance(durationMs) {
    this.frameHandle = this.requestFrame(() => this.tick(durationMs));
  }

  tick(expectedDuration) {
    this.frameHandle = null;
    if (!this.running) return;
    const script = this.getScript();
    if (!script) return;
    const elapsed = this.now() - this.stepStartMs;
    if (elapsed + 1 < expectedDuration) {
      this.scheduleAdvance(expectedDuration);
      return;
    }
    this.stepIndex += 1;
    if (this.stepIndex >= script.steps.length) {
      if (script.repeat) {
        this.stepIndex = 0;
        this.runStep();
      } else {
        this.finish(script);
      }
      return;
    }
    this.runStep();
  }

  finish(script) {
    this.running = false;
    if (this.frameHandle !== null && this.cancelFrame) {
      this.cancelFrame(this.frameHandle);
      this.frameHandle = null;
    }
    if (this.onProgress) {
      this.onProgress({
        stepIndex: this.stepIndex,
        totalSteps: script.steps.length,
        finished: true,
      });
    }
    this.transport.send("STREAM_END").catch(() => {});
    this.transport.send("R 0 15 128 128 128 128").catch(() => {});
  }
}

// requestAnimationFrame wrapper that also exposes cancel for testing.
function requestAnimationFrameSafe(cb) {
  if (typeof requestAnimationFrame === "function") {
    return requestAnimationFrame(() => cb());
  }
  return setTimeout(() => cb(), 16);
}

// Human-readable mm:ss.mmm formatter used by the editor summary. Kept here
// so editor.js stays self-contained and can be unit-tested without DOM.
export function formatMs(milliseconds) {
  const total = Math.max(0, Math.round(milliseconds | 0));
  const minutes = Math.floor(total / 60000);
  const seconds = Math.floor((total % 60000) / 1000);
  const millis = total % 1000;
  return `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(
    2,
    "0",
  )}.${String(millis).padStart(3, "0")}`;
}