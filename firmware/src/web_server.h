#pragma once

#include <Arduino.h>

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

#include "config_store.h"
#include "wifi_manager.h"

namespace farmers {

// One line of WS reply text. The ctx is whatever the dispatcher
// registered with setWsCommandHandler() — typically the originating
// AsyncWebSocketClient pointer so replies go back to the same peer.
typedef void (*WsReplyFn)(const char* line, void* ctx);

// Dispatch a single text frame received on the /ws endpoint. The
// dispatcher (lives in main.cpp alongside the macro engines and the
// streamMode flag) parses the command, mutates firmware state, and
// replies via `reply(line)` for each response line — at minimum one
// of OK / ERR / a JSON payload. The replyCtx is whatever the caller
// wants to thread through — typically the originating
// AsyncWebSocketClient pointer so replies go back to the same peer —
// but the dispatcher treats it as opaque and never inspects it.
// Returns nothing; the dispatcher must send at least one reply before
// returning so the browser never hangs waiting on /ws.
typedef void (*WsCommandHandler)(const char* line,
                                 WsReplyFn reply, void* replyCtx);

// Captive portal + provisioning endpoints served by AsyncWebServer. Owns
// no transport state — the WiFi manager already runs the radio; this
// class just adds the user-facing HTTP surface on top.
//
// Lifecycle:
//   1. web_server.begin(&Config)   — call once from setup() when the
//      radio is up (in either STA or AP mode). The portal works in
//      both: STA serves the real WebUI, AP also serves it via
//      splatoon.local (mDNS).
//   2. web_server.tick()            — call from loop() so the DNS
//      server can drain its UDP queue.
class WebServer {
 public:
  WebServer();

  // Idempotent. Wires up the routes. Captive portal DNS server
  // starts only in AP mode (STA's mDNSResponder already answers).
  void begin(ConfigStore* config, WifiManager* wifi);

  // Drive the DNS server (only in AP mode). Cheap when not active.
  void tick();

  bool isActive() const { return server_ != nullptr; }

  // Wire the WS command dispatcher. The handler owns all command parsing
  // and reply logic so the protocol surface is shared between the
  // serial path and the WiFi transport without duplicating the
  // command table. Call once from setup() before web traffic starts.
  void setWsCommandHandler(WsCommandHandler handler) {
    wsCommandHandler_ = handler;
  }

 private:
  // GET handlers
  void onRoot(AsyncWebServerRequest* req);
  void onScan(AsyncWebServerRequest* req);
  // POST /api/wifi — {ssid, pass} -> save + restart in 2s
  void onSetWifi(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                 size_t index, size_t total);
  // POST /api/reset — wipe WiFi creds + jump to AP
  void onResetWifi(AsyncWebServerRequest* req);

  // Captive portal: redirect any unknown host to /provision
  void onCaptiveRedirect(AsyncWebServerRequest* req);

  // DNS: in AP mode, every query returns the softAP IP so the OS
  // auto-pop the portal page when the user joins the AP.
  void startDns();
  void stopDns();

  // WebSocket endpoint at /ws. Text frames are forwarded to the
  // dispatcher registered with setWsCommandHandler(); the dispatcher
  // is the single source of truth for the protocol surface and is
  // shared with the serial path. If no dispatcher is registered
  // (e.g. before setup() finishes wiring it), frames fall through to
  // a raw 6-tuple HID report parser so a 6-tuple frame still pushes
  // Gamepad.write().
  void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len);
  void wsRawReportFallback(AsyncWebSocketClient* client, const String& line);
  static void replyTo(AsyncWebSocketClient* client, const char* line);

  ConfigStore* config_ = nullptr;
  WifiManager* wifi_ = nullptr;
  AsyncWebServer* server_ = nullptr;
  AsyncWebSocket ws_{"/ws"};
  DNSServer* dns_ = nullptr;
  // Whether the user has submitted new WiFi credentials and a restart
  // is pending. Reset at boot by reading the counter.
  bool restart_pending_ = false;
  uint32_t restart_at_ms_ = 0;
  // Dispatcher invoked for every WS text frame. Owned by main.cpp so
  // the handler can mutate the macro engines / streamMode flag without
  // exposing them through this header.
  WsCommandHandler wsCommandHandler_ = nullptr;
};

}  // namespace farmers
