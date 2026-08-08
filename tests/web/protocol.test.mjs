import assert from "node:assert/strict";
import test from "node:test";

import { formatDuration, parseDeviceLine } from "../../web/protocol.js";
import { MockSerialTransport } from "../../web/serial-transport.js";

test("parses firmware status JSON", () => {
  const message = parseDeviceLine(
    '{"type":"status","ok":true,"state":"running","step":9,"steps":48}',
  );
  assert.equal(message.type, "status");
  assert.equal(message.state, "running");
  assert.equal(message.step, 9);
});

test("handles compatibility responses and malformed input", () => {
  assert.deepEqual(parseDeviceLine("PONG\r"), { type: "pong", ok: true });
  assert.deepEqual(parseDeviceLine("OK"), { type: "ack", ok: true });
  assert.equal(parseDeviceLine("ERR").ok, false);
  assert.equal(parseDeviceLine("{broken").type, "unknown");
  assert.equal(parseDeviceLine(""), null);
});

test("formats the complete embedded cycle", () => {
  assert.equal(formatDuration(64995), "01:04.995");
});

test("mock transport follows HELLO, START, STATUS and STOP", async () => {
  const lines = [];
  const transport = new MockSerialTransport({
    onLine: (line) => lines.push(parseDeviceLine(line)),
    onDisconnect: () => assert.fail("mock should not disconnect"),
  });

  await transport.connect();
  await transport.send("HELLO");
  await transport.send("START");
  await transport.send("STATUS");
  await transport.send("STOP");

  assert.equal(lines[0].type, "info");
  assert.equal(lines[0].state, "idle");
  assert.equal(lines[1].state, "running");
  assert.equal(lines[2].state, "running");
  assert.equal(lines[3].state, "idle");
  assert.equal(lines[3].routine, "material-farm");
});

test("manual raw report stops the mock macro and is acknowledged", async () => {
  const lines = [];
  const transport = new MockSerialTransport({
    onLine: (line) => lines.push(parseDeviceLine(line)),
    onDisconnect: () => assert.fail("mock should not disconnect"),
  });

  await transport.connect();
  await transport.send("START");
  await transport.send("R 20 0 128 128 128 128");
  await transport.send("STATUS");

  assert.equal(transport.lastReport, "R 20 0 128 128 128 128");
  assert.deepEqual(lines[1], { type: "ack", ok: true });
  assert.equal(lines[2].state, "idle");
});

test("START_APRICOT switches the mock to the apricot-den routine", async () => {
  const lines = [];
  const transport = new MockSerialTransport({
    onLine: (line) => lines.push(parseDeviceLine(line)),
    onDisconnect: () => assert.fail("mock should not disconnect"),
  });

  await transport.connect();
  await transport.send("HELLO");
  await transport.send("START_APRICOT");
  await transport.send("STATUS");

  assert.equal(lines[0].routine, "material-farm");
  assert.equal(lines[1].state, "running");
  assert.equal(lines[1].routine, "apricot-den");
  assert.equal(lines[1].steps, 35);
  assert.equal(lines[2].routine, "apricot-den");
  assert.equal(lines[2].state, "running");
});

test("START_INKBACK switches the mock to the apricot-den-inkback routine", async () => {
  const lines = [];
  const transport = new MockSerialTransport({
    onLine: (line) => lines.push(parseDeviceLine(line)),
    onDisconnect: () => assert.fail("mock should not disconnect"),
  });

  await transport.connect();
  await transport.send("HELLO");
  await transport.send("START_INKBACK");
  await transport.send("STATUS");

  assert.equal(lines[0].routine, "material-farm");
  assert.equal(lines[1].state, "running");
  assert.equal(lines[1].routine, "apricot-den-inkback");
  assert.equal(lines[1].steps, 100);
  assert.equal(lines[1].cycle_ms, 95750);
  assert.equal(lines[2].routine, "apricot-den-inkback");
  assert.equal(lines[2].state, "running");
});

test("STREAM and STREAM_END toggle the mock's streamMode flag", async () => {
  const lines = [];
  const transport = new MockSerialTransport({
    onLine: (line) => lines.push(parseDeviceLine(line)),
    onDisconnect: () => assert.fail("mock should not disconnect"),
  });

  await transport.connect();
  assert.equal(transport.streamMode, false);
  await transport.send("STREAM");
  assert.equal(transport.streamMode, true);
  assert.deepEqual(lines.at(-1), { type: "ack", ok: true });
  await transport.send("STREAM_END");
  assert.equal(transport.streamMode, false);
  assert.deepEqual(lines.at(-1), { type: "ack", ok: true });
});
