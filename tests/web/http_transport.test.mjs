import assert from "node:assert/strict";
import test from "node:test";

import { HttpTransport } from "../../web/http-transport.js";

// The browser WebSocket standard supports both `addEventListener('open',
// fn)` and the legacy `socket.onopen = fn` property setter. The legacy
// form is what HttpTransport.connect() uses, so the fake must wire the
// property setter into the same handler map. We do that by defining
// the fake as a class with explicit property setters, which is more
// robust than defineProperty-in-a-loop on a plain object literal.
class FakeWebSocket {
  constructor(url) {
    this.url = url;
    this.sent = [];
    this._handlers = {};
  }
  addEventListener(event, fn) {
    this._handlers[event] = fn;
  }
  // Property-style setters. onopen, onmessage, onclose, onerror
  // each forward into the corresponding handler slot.
  set onopen(fn) { this._handlers.open = fn; }
  set onmessage(fn) { this._handlers.message = fn; }
  set onclose(fn) { this._handlers.close = fn; }
  set onerror(fn) { this._handlers.error = fn; }
  send(data) { this.sent.push(data); }
  close() { this._handlers.close?.({ code: 1000, reason: "" }); }
  // Manually fire an event — the HttpTransport code awaits this in
  // place of the real browser event loop.
  trigger(event, payload) { this._handlers[event]?.(payload); }
}

function installFake() {
  const created = [];
  globalThis.WebSocket = function (url) {
    const f = new FakeWebSocket(url);
    created.push(f);
    return f;
  };
  return created;
}

test("HttpTransport.isSupported checks for WebSocket on globalThis", () => {
  const saved = globalThis.WebSocket;
  delete globalThis.WebSocket;
  try {
    assert.equal(HttpTransport.isSupported(), false);
  } finally {
    if (saved) globalThis.WebSocket = saved;
  }
  function FakeWebSocket() {}
  globalThis.WebSocket = FakeWebSocket;
  try {
    assert.equal(HttpTransport.isSupported(), true);
  } finally {
    delete globalThis.WebSocket;
  }
});

test("HttpTransport.connect resolves on WS open + reports via onLine", async () => {
  const created = installFake();
  try {
    const lines = [];
    const transport = new HttpTransport({
      onLine: (l) => lines.push(l),
      onDisconnect: () => {},
    });
    const p = transport.connect();
    // Yield once so connect()'s Promise body has run and the onopen
    // setter is in place.
    await Promise.resolve();
    assert.equal(created.length, 1);
    assert.equal(created[0].url, "ws://splatoon.local/ws");
    created[0].trigger("open", {});
    await p;
    assert.equal(transport.connected, true);
    created[0].trigger("message", { data: "OK\n" });
    assert.deepEqual(lines, ["OK"]);
  } finally {
    delete globalThis.WebSocket;
  }
});

test("HttpTransport.send appends \\n to outgoing commands", async () => {
  const created = installFake();
  try {
    const transport = new HttpTransport({
      onLine: () => {},
      onDisconnect: () => {},
    });
    const p = transport.connect();
    await Promise.resolve();
    created[0].trigger("open", {});
    await p;
    await transport.send("STATUS");
    assert.equal(created[0].sent[0], "STATUS\n");
  } finally {
    delete globalThis.WebSocket;
  }
});

test.skip("HttpTransport.disconnect is silent unless the socket drops unexpectedly", () => {});

test("HttpTransport.connect rejects on connection error", async () => {
  const created = installFake();
  try {
    const transport = new HttpTransport({
      onLine: () => {},
      onDisconnect: () => {},
    });
    const p = transport.connect();
    await Promise.resolve();
    created[0].trigger("error", {});
    await assert.rejects(p, /WebSocket connect to .+ failed/);
  } finally {
    delete globalThis.WebSocket;
  }
});
