// HTTP/WebSocket transport for the Splatoon-Farmers board. The board
// runs an AsyncWebServer that exposes /ws, and this module speaks the
// same line-oriented protocol the WebSerial path does: each WS text
// frame is one command line, and the board replies with one frame per
// response line. The interface mirrors SerialTransport in
// serial-transport.js so app.js can swap the two with no other
// changes — the only behavioral difference is the connection method
// (WebSocket instead of Web Serial) and the host we point at
// (splatoon.local on the LAN, fallback to a user-typed IP).
//
// The transport is intentionally tiny: it forwards whatever the app
// sends, splits multi-line responses, and reports disconnects so
// the rest of the page can show a "reconnecting" banner.

export class HttpTransport {
  // isSupported() is the same predicate app.js uses to enable the
  // connect button. WebSocket is available in every browser we
  // support (Chrome / Edge / Firefox / Safari modern). We probe
  // globalThis (works in both browsers and Node test environments)
  // rather than `window` directly so the test suite can run without
  // a DOM.
  static isSupported() {
    return typeof globalThis !== "undefined" && "WebSocket" in globalThis;
  }

  // Constructor takes an options bag with the same shape as
  // SerialTransport plus a host (default: splatoon.local) and a
  // secure flag (default: true so we use wss://, but the board's
  // v1 firmware only serves plain ws:// — set secure=false unless
  // the board has been re-flashed with TLS).
  constructor({ onLine, onDisconnect, host = "splatoon.local", secure = false } = {}) {
    if (typeof onLine !== "function") {
      throw new Error("HttpTransport requires onLine(line)");
    }
    if (typeof onDisconnect !== "function") {
      throw new Error("HttpTransport requires onDisconnect(error)");
    }
    this.onLine = onLine;
    this.onDisconnect = onDisconnect;
    this.host = host;
    this.secure = secure;
    this.ws = null;
    this.connected = false;
    // Track partial frames. The firmware may send a single command's
    // response across multiple text() calls, so we buffer until the
    // newline that terminates a logical line.
    this.buffer = "";
    // Track a single intentional close so onDisconnect fires only on
    // unexpected drops — matches SerialTransport's contract.
    this.intentionalClose = false;
  }

  // Open the WebSocket. Resolves on open; rejects on error. We wrap
  // the entire WebSocket construction in a Promise so the resolve /
  // reject handlers are bound *before* the constructor can fire any
  // callbacks. A naive `new WebSocket(url); ws.onopen = ...` pattern
  // races the fake / real WebSocket's synchronous onerror emission and
  // can drop errors on the floor.
  async connect() {
    if (this.ws) {
      throw new Error("HttpTransport already connected");
    }
    const protocol = this.secure ? "wss" : "ws";
    const url = `${protocol}://${this.host}/ws`;
    await new Promise((resolve, reject) => {
      let settled = false;
      const settleResolve = () => { if (!settled) { settled = true; resolve(); } };
      const settleReject = (err) => { if (!settled) { settled = true; reject(err); } };
      let ws;
      try {
        ws = new WebSocket(url);
      } catch (err) {
        return settleReject(err);
      }
      this.ws = ws;
      ws.onopen = settleResolve;
      ws.onerror = () => settleReject(new Error(`WebSocket connect to ${url} failed`));
      ws.onmessage = (ev) => {
        this.buffer += ev.data;
        const lines = this.buffer.split("\n");
        this.buffer = lines.pop() ?? "";
        for (const line of lines) {
          const trimmed = line.replace(/\r$/, "");
          if (trimmed.length > 0) this.onLine(trimmed);
        }
      };
      ws.onclose = (ev) => {
        this.connected = false;
        this.ws = null;
        if (!this.intentionalClose) {
          this.onDisconnect(new Error(
            `WebSocket closed: code=${ev.code} reason=${ev.reason || "(none)"}`));
        }
      };
    });
    this.connected = true;
  }

  // Append a trailing newline (the wire format requires it) and send.
  // The firmware treats the newline as the line terminator; both \n
  // and \r\n are accepted. We always send \n for consistency with
  // SerialTransport.
  async send(command) {
    if (!this.ws || !this.connected) {
      throw new Error("HttpTransport not connected");
    }
    if (typeof command !== "string") {
      throw new Error("HttpTransport.send expects a string");
    }
    return new Promise((resolve, reject) => {
      try {
        this.ws.send(command + "\n");
        resolve();
      } catch (err) {
        reject(err);
      }
    });
  }

  // Clean teardown. The intentionalClose guard prevents the
  // onclose handler from firing a disconnect event during a planned
  // shutdown — matches SerialTransport.
  async disconnect() {
    if (!this.ws) return;
    this.intentionalClose = true;
    try {
      this.ws.close();
    } catch {
      // best-effort
    }
    this.ws = null;
    this.connected = false;
  }
}
