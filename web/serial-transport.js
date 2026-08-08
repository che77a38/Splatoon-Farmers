import { DEVICE_BAUD_RATE } from "./protocol.js";

export class SerialTransport {
  constructor({ onLine, onDisconnect }) {
    this.onLine = onLine;
    this.onDisconnect = onDisconnect;
    this.port = null;
    this.reader = null;
    this.readTask = null;
    this.writeChain = Promise.resolve();
    this.connected = false;
    this.intentionalClose = false;
  }

  static isSupported() {
    return "serial" in navigator;
  }

  async connect() {
    if (!SerialTransport.isSupported()) {
      throw new Error("当前浏览器不支持 Web Serial，请使用桌面版 Chrome 或 Edge。");
    }

    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: DEVICE_BAUD_RATE, bufferSize: 255 });
    try {
      await this.port.setSignals({
        dataTerminalReady: false,
        requestToSend: false,
      });
    } catch {
      // Some USB-UART drivers do not expose modem control lines. The data
      // channel still works, and avoiding a hard failure is safer here.
    }
    this.intentionalClose = false;
    this.connected = true;
    this.readTask = this.readLoop();
  }

  send(command) {
    const write = async () => {
      if (!this.connected || !this.port?.writable) {
        throw new Error("串口尚未连接");
      }
      const writer = this.port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(`${command}\n`));
      } finally {
        writer.releaseLock();
      }
    };
    const result = this.writeChain.then(write, write);
    this.writeChain = result.catch(() => {});
    return result;
  }

  async disconnect() {
    if (!this.port) {
      return;
    }
    this.intentionalClose = true;
    this.connected = false;

    await this.writeChain.catch(() => {});
    if (this.reader) {
      try {
        await this.reader.cancel();
      } catch {
        // The physical port may already be gone.
      }
    }
    if (this.readTask) {
      try {
        await this.readTask;
      } catch {
        // readLoop reports unexpected failures through onDisconnect.
      }
    }
    try {
      await this.port.close();
    } finally {
      this.port = null;
      this.readTask = null;
      this.intentionalClose = false;
    }
  }

  async readLoop() {
    let buffered = "";
    try {
      while (this.connected && this.port?.readable) {
        this.reader = this.port.readable.getReader();
        try {
          while (this.connected) {
            const { value, done } = await this.reader.read();
            if (done) {
              break;
            }
            buffered += new TextDecoder().decode(value, { stream: true });
            const lines = buffered.split(/\r?\n/);
            buffered = lines.pop() ?? "";
            for (const line of lines) {
              if (line.trim()) {
                this.onLine(line);
              }
            }
          }
        } finally {
          this.reader.releaseLock();
          this.reader = null;
        }
      }
    } catch (error) {
      if (!this.intentionalClose && this.connected) {
        this.connected = false;
        this.onDisconnect(error);
      }
      return;
    }

    if (!this.intentionalClose && this.connected) {
      this.connected = false;
      this.onDisconnect(new Error("串口数据流已经断开"));
    }
  }
}

export class MockSerialTransport {
  constructor({ onLine, onDisconnect }) {
    this.onLine = onLine;
    this.onDisconnect = onDisconnect;
    this.connected = false;
    this.state = "idle";
    this.phase = "idle";
    this.step = 0;
    this.cycle = 0;
    this.lastReport = null;
    this.streamMode = false;
    // Tracks the currently selected routine, mirroring firmware `Active`.
    // Mirrors the SCRIPTS table in app.js so the mock stays in sync with the
    // web UI's expectations. Kept inline (not imported) to avoid pulling the
    // browser-only app.js into Node's module loader.
    this.routine = "material-farm";
    this.steps = 48;
    this.durationMs = 61010;
    this.loopGapMs = 2585;
    this.cycleMs = 63595;
  }

  static isSupported() {
    return true;
  }

  selectRoutine(name) {
    if (name === "apricot-den-inkback") {
      this.routine = "apricot-den-inkback";
      this.steps = 100;
      this.durationMs = 95550;
      this.loopGapMs = 200;
      this.cycleMs = 95750;
    } else if (name === "apricot-den") {
      this.routine = "apricot-den";
      this.steps = 35;
      this.durationMs = 55550;
      this.loopGapMs = 200;
      this.cycleMs = 55750;
    } else {
      this.routine = "material-farm";
      this.steps = 48;
      this.durationMs = 62410;
      this.loopGapMs = 2585;
      this.cycleMs = 64995;
    }
  }

  async connect() {
    this.connected = true;
  }

  async send(command) {
    if (!this.connected) {
      throw new Error("模拟串口尚未连接");
    }
    if (
      command === "START" ||
      command === "START_MATERIAL" ||
      command === "START_DEFAULT"
    ) {
      this.selectRoutine("material-farm");
      this.state = "running";
      this.phase = "steps";
      this.step = 1;
      this.emit("status");
    } else if (command === "START_APRICOT" || command === "START2") {
      this.selectRoutine("apricot-den");
      this.state = "running";
      this.phase = "steps";
      this.step = 1;
      this.emit("status");
    } else if (command === "START_INKBACK" || command === "START3") {
      this.selectRoutine("apricot-den-inkback");
      this.state = "running";
      this.phase = "steps";
      this.step = 1;
      this.emit("status");
    } else if (command === "STOP") {
      this.state = "idle";
      this.phase = "idle";
      this.step = 0;
      this.emit("status");
    } else if (command === "HELLO" || command === "INFO") {
      this.emit("info");
    } else if (command === "STATUS") {
      this.emit("status");
    } else if (command === "SCRIPT") {
      this.onLine(JSON.stringify({ script: this.routine }));
    } else if (command === "PING") {
      this.onLine("PONG");
    } else if (/^R \d+ \d+ \d+ \d+ \d+ \d+$/.test(command)) {
      this.state = "idle";
      this.phase = "idle";
      this.step = 0;
      this.lastReport = command;
      this.onLine("OK");
    } else if (command === "STREAM") {
      // Stream mode: firmware stops the macro engines and accepts raw
      // `R ...` frames as the host-driven HID bus. The mock mirrors this
      // by toggling its stream flag without altering the routine name.
      this.streamMode = true;
      this.onLine("OK");
    } else if (command === "STREAM_END") {
      this.streamMode = false;
      this.onLine("OK");
    } else {
      this.onLine("ERR");
    }
  }

  async disconnect() {
    this.connected = false;
  }

  emit(type) {
    this.onLine(
      JSON.stringify({
        type,
        ok: true,
        firmware: "SplatoonFarmers/mock",
        routine: this.routine,
        embedded: true,
        state: this.state,
        phase: this.phase,
        step: this.step,
        steps: this.steps,
        cycle: this.cycle,
        duration_ms: this.durationMs,
        loop_gap_ms: this.loopGapMs,
        cycle_ms: this.cycleMs,
      }),
    );
  }
}
