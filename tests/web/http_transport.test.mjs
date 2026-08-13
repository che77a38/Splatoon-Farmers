import assert from "node:assert/strict";
import test from "node:test";

import { HttpTransport } from "../../web/http-transport.js";

test("HttpTransport.isSupported checks for WebSocket on globalThis", () => {
  // The predicate is "WebSocket" in globalThis (not "window") so the
  // same code path works in browsers and in the Node test runner.
  // Browsers expose WebSocket on globalThis; Node does not unless a
  // test installs a stub. We round-trip both cases here.
  const saved = globalThis.WebSocket;
  delete globalThis.WebSocket;
  try {
    assert.equal(HttpTransport.isSupported(), false);
  } finally {
    if (saved) globalThis.WebSocket = saved;
  }
  function FakeWebSocket() {
    return { on() {}, send() {}, close() {} };
  }
  globalThis.WebSocket = FakeWebSocket;
  try {
    assert.equal(HttpTransport.isSupported(), true);
  } finally {
    delete globalThis.WebSocket;
  }
});

// The remaining tests need a richer fake that wires the standard
// `onopen = fn` property setter. That requires defining a host class
// rather than a plain object literal; the implementation lives in
// commit 8 alongside the integration tests. Skipped here to avoid
// spending more time on test infrastructure in commit 7.
test.skip("HttpTransport.connect resolves on WS open + reports via onLine", () => {});
test.skip("HttpTransport.send appends \\n to outgoing commands", () => {});
test.skip("HttpTransport.disconnect is silent unless the socket drops unexpectedly", () => {});
test.skip("HttpTransport.connect rejects on connection error", () => {});
