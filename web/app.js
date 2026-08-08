import { formatDuration, parseDeviceLine } from "./protocol.js";
import {
  buildManualReport,
  KEYBOARD_BINDINGS,
  ManualInputState,
} from "./manual-input.js";
import { MockSerialTransport, SerialTransport } from "./serial-transport.js";
import {
  Script,
  ScriptRecorder,
  ScriptRunner,
  emptyScript,
  stepIcon,
  formatMs,
  saveScriptToStorage,
  loadScriptFromStorage,
  scriptToJsonUrl,
  scriptDownloadFilename,
  scriptFromFileInput,
  STEP_TEMPLATES,
  getStepTemplate,
} from "./editor.js";

const elements = {
  connectionButton: document.querySelector('[data-testid="connect-button"]'),
  startButton: document.querySelector('[data-testid="start-button"]'),
  stopButton: document.querySelector('[data-testid="stop-button"]'),
  statusBadge: document.querySelector('[data-testid="status-badge"]'),
  statusText: document.querySelector('[data-testid="status-text"]'),
  detailText: document.querySelector('[data-testid="detail-text"]'),
  progress: document.querySelector('[data-testid="macro-progress"]'),
  stepText: document.querySelector('[data-testid="step-text"]'),
  browserNote: document.querySelector('[data-testid="browser-note"]'),
  errorText: document.querySelector('[data-testid="error-text"]'),
  durationText: document.querySelector('[data-testid="duration-text"]'),
  manualStatus: document.querySelector('[data-testid="manual-status"]'),
  routineText: document.querySelector('[data-testid="routine-text"]'),
  stepCountText: document.querySelector('[data-testid="step-count-text"]'),
  heroStepCount: document.querySelector('[data-testid="hero-step-count"]'),
  scriptChips: [
    ...document.querySelectorAll(
      '[data-testid="script-material"], [data-testid="script-apricot"], [data-testid="script-inkback"], [data-testid="script-custom"]',
    ),
  ],
  customSummary: document.querySelector('[data-testid="custom-summary"]'),
  editorCard: document.querySelector('[data-testid="editor-card"]'),
  editorStatus: document.querySelector('[data-testid="editor-status"]'),
  editorStatusText: document.querySelector('[data-testid="editor-status-text"]'),
  editorStepCount: document.querySelector('[data-testid="editor-step-count"]'),
  editorTotalMs: document.querySelector('[data-testid="editor-total-ms"]'),
  editorRepeatText: document.querySelector('[data-testid="editor-repeat-text"]'),
  editorClearButton: document.querySelector('[data-testid="editor-clear"]'),
  editorRepeatCheckbox: document.querySelector('[data-testid="editor-repeat"]'),
  editorRecButton: document.querySelector('[data-testid="editor-rec"]'),
  editorRecordingHint: document.querySelector('[data-testid="editor-recording-hint"]'),
  editorExportButton: document.querySelector('[data-testid="editor-export"]'),
  editorImportButton: document.querySelector('[data-testid="editor-import"]'),
  editorImportInput: document.querySelector('[data-testid="editor-import-input"]'),
  editorAddMenu: document.querySelector('[data-testid="editor-add-menu"]'),
  editorAddLists: document.querySelectorAll('.editor-add-list[data-template-group]'),
  editorSteps: document.querySelector('[data-testid="editor-steps"]'),
  editorEmpty: document.querySelector('[data-testid="editor-empty"]'),
};
const manualButtons = [
  ...document.querySelectorAll("button[data-control]"),
];

// Script display metadata, keyed by the `routine` string the device emits in
// `info` / `status` JSON. The command we send depends on the key:
//   material-farm         -> START          (default, 48-step 杏棱巢穴)
//   apricot-den           -> START_APRICOT  (EndingCrystal 35-step 天妇罗巢穴)
//   apricot-den-inkback   -> START_INKBACK  (天妇罗回墨版: same 35-step surface
//                                              but the two held-ZR windows are
//                                              rewritten as alternating ZR/ZL
//                                              taps to pump paint back in)
const SCRIPTS = {
  "material-farm": {
    label: "杏棱巢穴",
    command: "START",
    stepCount: 48,
    cycleMs: 64995,
  },
  "apricot-den": {
    label: "天妇罗巢穴",
    command: "START_APRICOT",
    stepCount: 35,
    cycleMs: 55750,
  },
  "apricot-den-inkback": {
    label: "天妇罗回墨版",
    command: "START_INKBACK",
    stepCount: 100,
    cycleMs: 95750,
  },
  // "custom" is a host-side script driven by the WebUI editor. The picker
  // chip sets `selectedScript = "custom"` but the actual steps live in
  // `customScript` (see below); running this routine is handled by the
  // stream runner in app.js's startButton handler — never by `sendCommand`.
  custom: {
    label: "自定义",
    command: null,
    stepCount: 0,
    cycleMs: 0,
  },
};
const DEFAULT_SCRIPT_KEY = "material-farm";
const KNOWN_SCRIPT_KEYS = new Set(Object.keys(SCRIPTS));

// Script the user has *selected* in the picker. Distinct from `deviceRoutine`,
// which mirrors the firmware's currently-running script (updated from
// STATUS responses). The picker only takes effect when the user clicks Start.
let selectedScript = DEFAULT_SCRIPT_KEY;
// Last `routine` value reported by the device. Defaults to the picker choice
// so the UI does not flash "杏棱巢穴" before the first STATUS comes back.
let deviceRoutine = selectedScript;

// In-browser script for the "custom" picker chip. Owned by app.js, mutated
// by the editor (later commits add recorder + runner wiring).
const customScript = (() => {
  const restored = typeof loadScriptFromStorage === "function"
    ? loadScriptFromStorage()
    : null;
  return restored || emptyScript();
})();
const scriptRecorder = new ScriptRecorder(customScript);
// Stream runner is created lazily on first "运行" click once transport is
// connected; we keep a single instance per session.
let streamRunner = null;
// Debounced auto-save: every edit rerenders the editor card; we save to
// localStorage at most once per 500 ms so rapid mutations don't thrash.
let saveTimer = null;
function scheduleAutoSave() {
  if (saveTimer !== null) return;
  saveTimer = window.setTimeout(() => {
    saveTimer = null;
    saveScriptToStorage(customScript);
  }, 500);
}

const mockMode = new URLSearchParams(window.location.search).get("mock") === "1";
const TransportClass = mockMode ? MockSerialTransport : SerialTransport;
const transportSupported = TransportClass.isSupported();

let transport = null;
let connected = false;
let busy = false;
let deviceState = "unknown";
let devicePhase = "idle";
let currentStep = 0;
// Display step count tracks the *device's* current routine when known, else
// falls back to the picker-selected script's declared count.
let stepCount = SCRIPTS[selectedScript].stepCount;
let pollTimer = null;
let activeManualControls = new Set();
// One-shot flag: set true right before the post-connect HELLO so that the
// single response that comes back is allowed to re-snap the picker to the
// firmware's actual active script. Cleared as soon as any line is consumed,
// so STATUS polls never trigger a chip yank.
let pendingHelloSync = false;
// recorderEvent is a host-side bridge: each time ManualInputState notifies
// us with the new active controls set, we feed the bitmap + dpad back into
// the recorder so a subsequent press event has an up-to-date snapshot.
function recorderEvent(event) {
  scriptRecorder.onRecordEvent(event);
  // Only refresh the editor card on press/release/clear so we don't thrash
  // the DOM during multi-source repeated set updates.
  renderEditorCard();
  if (scriptRecorder.active) scheduleAutoSave();
}

const manualInputState = new ManualInputState(onManualInputChange, recorderEvent);

elements.durationText.textContent = formatDuration(SCRIPTS[selectedScript].cycleMs);
syncScriptChipUi();
renderEditorCard();

function setError(message = "") {
  elements.errorText.textContent = message;
  elements.errorText.hidden = !message;
}

// Render the script editor card. The card is shown only when the picker is
// on the "custom" chip so it does not steal space while the user is using a
// firmware-resident routine. Step rows are pure DOM built from the
// `customScript.steps` array; mutation events go through the same helpers
// the recorder will use in later commits.
function renderEditorCard() {
  if (!elements.editorCard) return;
  const isCustom = selectedScript === "custom";
  elements.editorCard.hidden = !isCustom;
  if (!isCustom) return;

  const steps = customScript.steps;
  const totalMs = customScript.totalMs();

  if (elements.customSummary) {
    elements.customSummary.textContent =
      `${steps.length} 步 · ${formatMs(totalMs)}`;
  }
  if (elements.editorStepCount) {
    elements.editorStepCount.textContent = String(steps.length);
  }
  if (elements.editorTotalMs) {
    elements.editorTotalMs.textContent = formatMs(totalMs);
  }
  if (elements.editorRepeatText) {
    elements.editorRepeatText.textContent = customScript.repeat ? "循环" : "单次";
  }
  if (elements.editorEmpty) {
    elements.editorEmpty.hidden = steps.length > 0;
  }
  if (elements.editorClearButton) {
    elements.editorClearButton.disabled = steps.length === 0 || scriptRecorder.active;
  }
  if (elements.editorExportButton) {
    elements.editorExportButton.disabled = steps.length === 0 || scriptRecorder.active;
  }
  if (elements.editorImportButton) {
    elements.editorImportButton.disabled = scriptRecorder.active;
  }
  if (elements.editorImportInput) {
    elements.editorImportInput.disabled = scriptRecorder.active;
  }
  if (elements.editorAddMenu) {
    const summary = elements.editorAddMenu.querySelector(".editor-add-summary");
    if (summary) summary.style.opacity = scriptRecorder.active ? "0.45" : "1";
  }
  for (const item of document.querySelectorAll(".editor-add-item")) {
    item.disabled = scriptRecorder.active;
  }
  if (elements.editorRepeatCheckbox) {
    elements.editorRepeatCheckbox.checked = customScript.repeat;
    elements.editorRepeatCheckbox.disabled = scriptRecorder.active;
  }
  if (elements.editorRecButton) {
    elements.editorRecButton.disabled = false;
    elements.editorRecButton.classList.toggle("is-recording", scriptRecorder.active);
    elements.editorRecButton.setAttribute(
      "aria-pressed",
      String(scriptRecorder.active),
    );
    elements.editorRecButton.textContent = scriptRecorder.active ? "■ STOP REC" : "● REC";
  }
  if (elements.editorRecordingHint) {
    elements.editorRecordingHint.hidden = !scriptRecorder.active;
  }
  if (elements.editorStatusText) {
    elements.editorStatusText.textContent = scriptRecorder.active
      ? `录制中 · ${steps.length} 步`
      : (steps.length === 0 ? "空脚本" : `${steps.length} 步`);
  }
  if (elements.editorStatus) {
    elements.editorStatus.dataset.state = scriptRecorder.active
      ? "recording"
      : (steps.length === 0 ? "empty" : "ready");
  }

  if (elements.editorSteps) {
    elements.editorSteps.replaceChildren(...steps.map((step, idx) =>
      renderStepRow(step, idx)));
  }
}

function renderStepRow(step, idx) {
  const li = document.createElement("li");
  li.className = "editor-step";
  li.dataset.type = step.type;
  li.dataset.stepIdx = String(idx);

  const num = document.createElement("span");
  num.className = "editor-step-num";
  num.textContent = `#${idx + 1}`;
  li.appendChild(num);

  const icon = document.createElement("span");
  icon.className = "editor-step-icon";
  icon.textContent = stepIcon(step);
  li.appendChild(icon);

  const duration = document.createElement("input");
  duration.className = "editor-step-duration";
  duration.type = "number";
  duration.min = "10";
  duration.step = "10";
  duration.value = String(step.durationMs | 0);
  duration.setAttribute("aria-label", `第 ${idx + 1} 步时长`);
  duration.addEventListener("change", () => {
    const value = Math.max(10, parseInt(duration.value, 10) || 10);
    customScript.steps[idx].durationMs = value;
    duration.value = String(value);
    renderEditorCard();
    scheduleAutoSave();
  });
  li.appendChild(duration);

  const actions = document.createElement("div");
  actions.className = "editor-step-actions";
  actions.appendChild(stepActionButton("↑", "up", idx, idx === 0));
  actions.appendChild(stepActionButton("↓", "down", idx, idx === customScript.steps.length - 1));
  actions.appendChild(stepActionButton("⎘", "dup", idx, false));
  actions.appendChild(stepActionButton("✕", "del", idx, false, "editor-step-action--danger"));
  li.appendChild(actions);

  return li;
}

function stepActionButton(label, action, idx, disabled, modifier = "") {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = `editor-step-action ${modifier}`.trim();
  btn.textContent = label;
  btn.disabled = disabled;
  btn.addEventListener("click", () => {
    if (action === "up") customScript.moveStep(idx, idx - 1);
    else if (action === "down") customScript.moveStep(idx, idx + 1);
    else if (action === "dup") customScript.duplicateStep(idx);
    else if (action === "del") customScript.removeStep(idx);
    renderEditorCard();
    scheduleAutoSave();
  });
  return btn;
}

// Reflects the picker's selection into the chip buttons (active state,
// aria-pressed, and the visible label/count). Called once at boot, on every
// chip click, and whenever the device reports a different running routine.
function syncScriptChipUi() {
  if (!elements.scriptChips) return;
  for (const chip of elements.scriptChips) {
    if (!chip) continue;
    const isActive = chip.dataset.script === selectedScript;
    chip.classList.toggle("is-active", isActive);
    chip.setAttribute("aria-pressed", String(isActive));
  }
}

function render() {
  const manualActive = connected && activeManualControls.size > 0;
  const running = connected && deviceState === "running" && !manualActive;
  elements.connectionButton.textContent = connected ? "断开串口" : "连接手柄";
  elements.connectionButton.disabled = busy || !transportSupported;
  elements.startButton.disabled = busy || !connected || running || manualActive;
  elements.stopButton.disabled = busy || !connected || !running;
  // Picker is enabled as soon as we're connected. We deliberately let the user
  // switch scripts while a routine is running — pressing Start afterwards will
  // restart with the new script (the firmware stops the old engine first).
  if (elements.scriptChips) {
    for (const chip of elements.scriptChips) {
      if (chip) chip.disabled = busy || !connected;
    }
  }
  for (const button of manualButtons) {
    const pressed = activeManualControls.has(button.dataset.control);
    button.disabled = busy || !connected;
    button.classList.toggle("is-pressed", pressed);
    button.setAttribute("aria-pressed", String(pressed));
  }

  elements.statusBadge.dataset.state = connected
    ? manualActive
      ? "manual"
      : running
      ? "running"
      : "connected"
    : "disconnected";

  if (!connected) {
    elements.statusText.textContent = "未连接";
    elements.detailText.textContent =
      deviceState === "running"
        ? "控制线已断开；板载远征任务可能仍在独立运行"
        : "先用 USB-UART 连接电脑";
  } else if (manualActive) {
    elements.statusText.textContent = "手动输入";
    elements.detailText.textContent = `已按下 ${activeManualControls.size} 个控制 · 板载脚本已停止`;
  } else if (running && devicePhase === "gap") {
    elements.statusText.textContent = "补给间隔";
    elements.detailText.textContent = `已完成 ${Math.max(
      1,
      Number(elements.statusBadge.dataset.cycle || 1),
    )} 轮 · 准备下一次${SCRIPTS[deviceRoutine].label}`;
  } else if (running) {
    elements.statusText.textContent = "远征执行中";
    elements.detailText.textContent = `脚本在 ESP32-S3 本地执行 · 第 ${currentStep}/${stepCount} 步`;
  } else {
    elements.statusText.textContent = "已连接 · 待命";
    elements.detailText.textContent = "素材脚本已固化在 Flash，点击即可从第 1 步出发";
  }

  if (!connected) {
    elements.manualStatus.textContent = "连接后启用";
    elements.manualStatus.dataset.state = "disconnected";
  } else if (manualActive) {
    elements.manualStatus.textContent = `${activeManualControls.size} 个输入按下`;
    elements.manualStatus.dataset.state = "active";
  } else {
    elements.manualStatus.textContent = "键盘输入已启用";
    elements.manualStatus.dataset.state = "ready";
  }

  elements.progress.max = stepCount;
  elements.progress.value = running ? currentStep : 0;
  elements.stepText.textContent = running
    ? `${currentStep} / ${stepCount}`
    : `0 / ${stepCount}`;
}

function applyDeviceMessage(message, { syncPicker = false } = {}) {
  if (!message || message.ok === false) {
    if (message?.message) {
      setError(message.message);
    }
    return;
  }
  if (message.type !== "info" && message.type !== "status") {
    return;
  }

  deviceState = message.state === "running" ? "running" : "idle";
  devicePhase = message.phase || "idle";
  currentStep = Number(message.step) || 0;
  stepCount = Number(message.steps) || stepCount;
  elements.statusBadge.dataset.cycle = String(Number(message.cycle) || 0);
  // The device reports its running routine in `routine`. We track it as
  // `deviceRoutine` for the status copy and the gap-phase "prepare next ..."
  // text. The picker only follows the device on the *initial* HELLO after
  // reconnect (so the chip matches whatever was already running). STATUS
  // polls do NOT yank the chip back — the user's picker choice stays
  // authoritative until they press Start; pressing Start will overwrite the
  // firmware's selection with whichever chip is highlighted.
  if (typeof message.routine === "string" &&
      KNOWN_SCRIPT_KEYS.has(message.routine)) {
    deviceRoutine = message.routine;
    const meta = SCRIPTS[deviceRoutine];
    elements.routineText.textContent = meta.label;
    elements.stepCountText.textContent = String(meta.stepCount);
    if (elements.heroStepCount) {
      elements.heroStepCount.textContent = `${meta.stepCount} STEPS`;
    }
    if (syncPicker) {
      selectedScript = deviceRoutine;
      syncScriptChipUi();
    }
  }
  if (Number.isFinite(message.cycle_ms)) {
    elements.durationText.textContent = formatDuration(message.cycle_ms);
  }
  setError();
  render();
}

function onLine(line) {
  const syncPicker = pendingHelloSync;
  pendingHelloSync = false;
  applyDeviceMessage(parseDeviceLine(line), { syncPicker });
}

function onManualInputChange(activeControls) {
  activeManualControls = activeControls;
  if (connected) {
    deviceState = "idle";
    devicePhase = "idle";
    currentStep = 0;
    setError();
  }
  // Keep the recorder's bitmap/dpad snapshot in sync with the live manual
  // state. We compute the report here (instead of asking ManualInputState)
  // because the report already encodes buttons + dpad in the wire format.
  const report = buildManualReport(activeControls);
  scriptRecorder.applyActiveSet(activeControls, {
    buttons: report.buttons,
    dpad: report.dpad,
  });
  render();

  if (!connected || !transport) {
    return;
  }
  transport.send(report.command).catch((error) => {
    setError(error?.message || "手动输入发送失败");
    render();
  });
}

function onUnexpectedDisconnect(error) {
  connected = false;
  busy = false;
  clearInterval(pollTimer);
  pollTimer = null;
  manualInputState.clear();
  setError(error?.message || "串口连接意外断开");
  render();
}

async function connect() {
  busy = true;
  setError();
  render();
  transport = new TransportClass({
    onLine,
    onDisconnect: onUnexpectedDisconnect,
  });
  try {
    await transport.connect();
    connected = true;
    // The HELLO right after connecting is the only response that should
    // re-snap the chip to whatever the firmware was already running. We flag
    // it with a transport-side hook so `onLine` can pass `syncPicker: true`
    // for this single response, then we clear the flag.
    pendingHelloSync = true;
    await transport.send("HELLO");
    pollTimer = window.setInterval(() => {
      transport?.send("STATUS").catch(onUnexpectedDisconnect);
    }, 1000);
  } catch (error) {
    connected = false;
    transport = null;
    setError(error?.message || "无法连接串口");
  } finally {
    busy = false;
    render();
  }
}

async function disconnect() {
  busy = true;
  clearInterval(pollTimer);
  pollTimer = null;
  manualInputState.clear();
  render();
  try {
    await transport?.disconnect();
  } catch (error) {
    setError(error?.message || "断开串口时发生错误");
  } finally {
    connected = false;
    transport = null;
    busy = false;
    render();
  }
}

async function sendCommand(command) {
  busy = true;
  setError();
  render();
  try {
    await transport.send(command);
  } catch (error) {
    setError(error?.message || "指令发送失败");
  } finally {
    busy = false;
    render();
  }
}

elements.connectionButton.addEventListener("click", () => {
  if (connected) {
    disconnect();
  } else {
    connect();
  }
});
elements.startButton.addEventListener("click", () => {
  if (selectedScript === "custom") {
    playCustomScript();
  } else {
    sendCommand(SCRIPTS[selectedScript].command);
  }
});
elements.stopButton.addEventListener("click", () => {
  if (streamRunner && streamRunner.isRunning()) {
    streamRunner.stop();
    setError();
  } else {
    sendCommand("STOP");
  }
});

async function playCustomScript() {
  if (!connected || !transport) {
    setError("请先连接手柄");
    return;
  }
  if (customScript.steps.length === 0) {
    setError("脚本为空 — 请先添加步骤或录制");
    return;
  }
  if (scriptRecorder.active) {
    setError("录制中，请先停止录制");
    return;
  }
  if (!streamRunner) {
    streamRunner = new ScriptRunner({
      transport,
      getScript: () => customScript,
      onProgress: ({ stepIndex, totalSteps, finished }) => {
        if (elements.editorStatusText) {
          elements.editorStatusText.textContent = finished
            ? `已结束 · ${totalSteps} 步`
            : `运行中 · ${stepIndex + 1}/${totalSteps}`;
        }
        if (elements.editorStatus) {
          elements.editorStatus.dataset.state = finished ? "finished" : "running";
        }
      },
    });
  }
  setError();
  await streamRunner.play();
}

for (const chip of elements.scriptChips) {
  chip.addEventListener("click", () => {
    const target = chip.dataset.script;
    if (!KNOWN_SCRIPT_KEYS.has(target) || target === selectedScript) {
      return;
    }
    selectedScript = target;
    // Update the picker state and the static readouts. The device will pick
    // up the change the next time the user presses Start (or immediately if a
    // STATUS poll happens to fire between now and then — `applyDeviceMessage`
    // is the source of truth and will re-sync us either way).
    syncScriptChipUi();
    const meta = SCRIPTS[selectedScript];
    if (target === "custom") {
      // The custom chip's step count / duration come from customScript, not
      // from the static SCRIPTS table; defer those updates to renderEditorCard.
      elements.routineText.textContent = meta.label;
      stepCount = customScript.steps.length || 1;
      currentStep = 0;
      elements.durationText.textContent = formatDuration(0);
      elements.stepCountText.textContent = String(customScript.steps.length);
      if (elements.heroStepCount) {
        elements.heroStepCount.textContent = `${customScript.steps.length} STEPS`;
      }
      renderEditorCard();
    } else {
      elements.routineText.textContent = meta.label;
      elements.stepCountText.textContent = String(meta.stepCount);
      if (elements.heroStepCount) {
        elements.heroStepCount.textContent = `${meta.stepCount} STEPS`;
      }
      elements.durationText.textContent = formatDuration(meta.cycleMs);
      stepCount = meta.stepCount;
      currentStep = 0;
      renderEditorCard();
    }
    render();
  });
}

if (elements.editorClearButton) {
  elements.editorClearButton.addEventListener("click", () => {
    if (customScript.steps.length === 0) return;
    if (!window.confirm("清空脚本会删除所有步骤，确定吗？")) return;
    customScript.clear();
    renderEditorCard();
    scheduleAutoSave();
    if (elements.customSummary) {
      elements.customSummary.textContent = "0 步 · 00:00.0";
    }
    if (elements.stepCountText) {
      elements.stepCountText.textContent = "0";
    }
    if (elements.heroStepCount) {
      elements.heroStepCount.textContent = "0 STEPS";
    }
  });
}

if (elements.editorRepeatCheckbox) {
  elements.editorRepeatCheckbox.addEventListener("change", () => {
    customScript.repeat = elements.editorRepeatCheckbox.checked;
    renderEditorCard();
    scheduleAutoSave();
  });
}

if (elements.editorExportButton) {
  elements.editorExportButton.addEventListener("click", () => {
    if (customScript.steps.length === 0) return;
    const url = scriptToJsonUrl(customScript);
    const a = document.createElement("a");
    a.href = url;
    a.download = scriptDownloadFilename(customScript);
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    // Revoke after a tick so the browser has time to start the download.
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  });
}

if (elements.editorImportButton && elements.editorImportInput) {
  elements.editorImportButton.addEventListener("click", () => {
    elements.editorImportInput.click();
  });
  elements.editorImportInput.addEventListener("change", async (event) => {
    const file = event.target.files && event.target.files[0];
    if (!file) return;
    try {
      const script = await scriptFromFileInput(file);
      // Replace the current script in place. Steps are immutable copies so
      // mutations to the loaded script do not affect storage.
      customScript.name = script.name;
      customScript.repeat = script.repeat;
      customScript.steps = script.steps;
      renderEditorCard();
      scheduleAutoSave();
    } catch (error) {
      setError(error.message || "导入脚本失败");
    } finally {
      // Clear the input so importing the same file twice fires change again.
      event.target.value = "";
    }
  });
}

if (elements.editorRecButton) {
  elements.editorRecButton.addEventListener("click", () => {
    if (scriptRecorder.active) {
      scriptRecorder.stop();
    } else {
      // Confirm overwrite if a script is already loaded. Skipping when the
      // script is empty avoids the prompt on first use.
      if (customScript.steps.length > 0 &&
          !window.confirm("开始录制会清空当前脚本，确定吗？")) {
        return;
      }
      scriptRecorder.start();
    }
    renderEditorCard();
  });
}

function pointerSource(pointerId) {
  return `pointer:${pointerId}`;
}

for (const button of manualButtons) {
  const control = button.dataset.control;
  button.addEventListener("pointerdown", (event) => {
    if (
      !connected ||
      busy ||
      (event.pointerType === "mouse" && event.button !== 0)
    ) {
      return;
    }
    event.preventDefault();
    try {
      button.setPointerCapture(event.pointerId);
    } catch {
      // Pointer capture is optional; window blur still releases all controls.
    }
    manualInputState.press(pointerSource(event.pointerId), control);
  });

  const releasePointer = (event) => {
    manualInputState.release(pointerSource(event.pointerId));
  };
  button.addEventListener("pointerup", releasePointer);
  button.addEventListener("pointercancel", releasePointer);
  button.addEventListener("lostpointercapture", releasePointer);
  button.addEventListener("contextmenu", (event) => event.preventDefault());

  button.addEventListener("keydown", (event) => {
    if (
      !connected ||
      busy ||
      (event.code !== "Space" && event.code !== "Enter")
    ) {
      return;
    }
    event.preventDefault();
    manualInputState.press(
      `button:${control}:${event.code}`,
      control,
    );
  });
  button.addEventListener("keyup", (event) => {
    if (event.code !== "Space" && event.code !== "Enter") {
      return;
    }
    event.preventDefault();
    manualInputState.release(`button:${control}:${event.code}`);
  });
  button.addEventListener("blur", () => {
    manualInputState.release(`button:${control}:Space`);
    manualInputState.release(`button:${control}:Enter`);
  });
}

window.addEventListener("keydown", (event) => {
  const control = KEYBOARD_BINDINGS[event.code];
  if (
    !control ||
    !connected ||
    busy ||
    event.metaKey ||
    event.ctrlKey ||
    event.altKey
  ) {
    return;
  }
  event.preventDefault();
  manualInputState.press(`keyboard:${event.code}`, control);
});

window.addEventListener("keyup", (event) => {
  const source = `keyboard:${event.code}`;
  if (!manualInputState.hasSource(source)) {
    return;
  }
  event.preventDefault();
  manualInputState.release(source);
});

window.addEventListener("blur", () => manualInputState.clear());
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    manualInputState.clear();
  }
});

if (!transportSupported) {
  elements.connectionButton.disabled = true;
  elements.browserNote.textContent =
    "当前浏览器不支持 Web Serial。请用桌面版 Chrome 或 Edge，并通过 localhost 打开本页。";
  elements.browserNote.dataset.warning = "true";
} else if (mockMode) {
  elements.browserNote.textContent =
    "DEMO MODE · 正在使用模拟串口，不会连接真实设备";
}

// Populate the "+ 添加步骤" menu by splitting STEP_TEMPLATES into their
// `group` buckets. Each <li> renders a single button; clicking inserts a
// new step at the end of customScript and closes the details panel.
if (elements.editorAddMenu && elements.editorAddLists.length) {
  for (const list of elements.editorAddLists) {
    const group = list.dataset.templateGroup;
    const templates = STEP_TEMPLATES.filter((t) => t.group === group);
    for (const tpl of templates) {
      const li = document.createElement("li");
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "editor-add-item";
      btn.dataset.templateId = tpl.id;
      btn.textContent = tpl.label;
      btn.addEventListener("click", () => {
        const step = getStepTemplate(tpl.id);
        if (!step) return;
        customScript.steps.push(step);
        renderEditorCard();
        scheduleAutoSave();
        elements.editorAddMenu.removeAttribute("open");
      });
      li.appendChild(btn);
      list.appendChild(li);
    }
  }
  // Clicking outside the menu closes it. We attach the listener once at the
  // document level so it survives re-renders.
  document.addEventListener("click", (event) => {
    if (!elements.editorAddMenu.open) return;
    if (!elements.editorAddMenu.contains(event.target)) {
      elements.editorAddMenu.removeAttribute("open");
    }
  });
  // Esc closes the menu too.
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && elements.editorAddMenu.open) {
      elements.editorAddMenu.removeAttribute("open");
    }
  });
}

render();
