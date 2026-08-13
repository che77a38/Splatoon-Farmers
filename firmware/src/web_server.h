#pragma once

#include <Arduino.h>

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

#include "config_store.h"
#include "wifi_manager.h"

namespace farmers {

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
  // auto-opens the portal page when the user joins the AP.
  void startDns();
  void stopDns();

  ConfigStore* config_ = nullptr;
  WifiManager* wifi_ = nullptr;
  AsyncWebServer* server_ = nullptr;
  DNSServer* dns_ = nullptr;
  // Whether the user has submitted new WiFi credentials and a restart
  // is pending. Reset at boot by reading the counter.
  bool restart_pending_ = false;
  uint32_t restart_at_ms_ = 0;
};

}  // namespace farmers
