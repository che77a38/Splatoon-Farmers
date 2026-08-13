#include "web_server.h"

#include <Arduino.h>

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "config_store.h"
#include "wifi_manager.h"

namespace farmers {

namespace {

// ASCII progress bar (used by /api/status). Cheap and self-contained.
String makeBar(int rssi) {
  // RSSI typically sits in [-100, -30]. Map to 0..5 segments.
  int segs = (rssi + 100) / 14;
  if (segs < 0) segs = 0;
  if (segs > 5) segs = 5;
  String s;
  for (int i = 0; i < 5; ++i) s += (i < segs) ? "█" : "░";
  return s;
}

}  // namespace

WebServer::WebServer() = default;

void WebServer::begin(ConfigStore* config, WifiManager* wifi) {
  config_ = config;
  wifi_ = wifi;

  // Mount LittleFS now so handler bodies can serve files from it. The
  // filesystem is empty at this commit (no index.html / app.js yet) but
  // the API routes still work, and the AP-mode provision page lives
  // here so the captive portal can render it.
  if (!LittleFS.begin(true /* formatOnFail */)) {
    Serial.println("[HTTP] LittleFS mount failed");
  }

  server_ = new AsyncWebServer(80);

  // Captive-portal DNS: in AP mode, every DNS query returns the softAP
  // IP so the OS auto-opens the portal page when the user joins the
  // AP. We start the DNS server only in AP mode; in STA mode the
  // mDNSResponder already answers the queries we need.
  server_->onNotFound([this](AsyncWebServerRequest* req) {
    onCaptiveRedirect(req);
  });

  // Provisioning page. In AP mode this is the landing page; in STA
  // mode it doubles as a "reconfigure" entry point the user can reach
  // by visiting /provision explicitly.
  server_->on("/provision", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (LittleFS.exists("/provision.html")) {
      req->send(LittleFS, "/provision.html", "text/html");
    } else {
      // Fallback: a one-line message so we never see a blank page if the
      // data partition hasn't been uploaded yet.
      req->send(200, "text/plain",
                "SplatoonFarmers provision endpoint. Upload data/provision.html to flash.\n");
    }
  });

  // /api/scan — returns JSON array of {ssid, rssi, secure, channel}
  // for the surrounding networks. ESP32 scan takes ~1-3 s so we set
  // a generous response timeout on the request.
  server_->on("/api/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onScan(req);
  });

  // /api/wifi — accept {ssid, pass} JSON, persist, schedule reboot.
  // mathieucarbou/AsyncWebServer 3.x's `on()` requires both an upload
  // handler and a body handler; we leave the upload handler empty
  // because the portal never uploads files, and the body handler
  // accumulates the JSON chunk stream.
  server_->on("/api/wifi", HTTP_POST,
              [](AsyncWebServerRequest* req) {},
              [](AsyncWebServerRequest* req, const String& filename,
                 size_t index, uint8_t* data, size_t len, bool final) {
                // File uploads are not used by the portal. This handler
                // is required by the API but is never exercised.
              },
              [this](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                     size_t index, size_t total) {
                onSetWifi(req, data, len, index, total);
              });

  // /api/reset — wipe credentials, fall back to AP mode next boot.
  server_->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
    onResetWifi(req);
  });

  // /api/status — JSON snapshot the captive portal polls.
  server_->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onRoot(req);
  });

  // Root path: in AP mode redirect to /provision, in STA mode the
  // static index.html (added in commit 5) will serve itself.
  server_->on("/", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (wifi_->mode() == WifiMode::kAp) {
      req->redirect("/provision");
    } else if (LittleFS.exists("/index.html")) {
      req->send(LittleFS, "/index.html", "text/html");
    } else {
      req->redirect("/provision");
    }
  });

  server_->begin();
  startDns();
  Serial.println("[HTTP] AsyncWebServer up on :80");
}

void WebServer::tick() {
  if (dns_) dns_->processNextRequest();

  if (restart_pending_ && millis() >= restart_at_ms_) {
    Serial.println("[HTTP] scheduled restart firing");
    ESP.restart();
  }
}

void WebServer::startDns() {
  // Only in AP mode — the softAP IP is the answer.
  if (wifi_->mode() != WifiMode::kAp) return;
  dns_ = new DNSServer();
  // Answer every query with the softAP IP. 53 is the standard DNS port.
  dns_->start(53, "*", wifi_->softApIp());
  Serial.println("[HTTP] captive DNS up: 53 -> " +
                wifi_->softApIp().toString());
}

void WebServer::stopDns() {
  if (dns_) {
    dns_->stop();
    delete dns_;
    dns_ = nullptr;
  }
}

void WebServer::onRoot(AsyncWebServerRequest* req) {
  // /api/status handler — build the JSON inline so we don't have to
  // worry about a separate status object staying in sync.
  const char* mode = (wifi_->mode() == WifiMode::kSta)        ? "sta" :
                     (wifi_->mode() == WifiMode::kStaConnecting) ? "sta-connecting" : "ap";
  String ip = wifi_->localIp();
  if (ip.isEmpty()) ip = wifi_->softApIp().toString();
  const char* status = (wifi_->statusCode() == 0) ? "connected" :
                       (wifi_->statusCode() == 1) ? "connecting" :
                       (wifi_->statusCode() == 2) ? "no-credentials" :
                       (wifi_->statusCode() == 3) ? "bad-credentials" : "ap-active";
  String json = "{";
  json += "\"mode\":\""; json += mode; json += "\",";
  json += "\"status\":\""; json += status; json += "\",";
  json += "\"ip\":\""; json += ip; json += "\",";
  json += "\"mdns\":\""; json += wifi_->mdnsName(); json += "\",";
  json += "\"ap_ssid\":\""; json += wifi_->apSsid(); json += "\",";
  json += "\"ap_clients\":"; json += wifi_->apClients();
  json += "}";
  req->send(200, "application/json", json);
}

void WebServer::onScan(AsyncWebServerRequest* req) {
  int n = wifi_->scanNetworks(/*async=*/false, /*show_hidden=*/true);
  String json = "[";
  for (int i = 0; i < n; ++i) {
    if (i) json += ",";
    json += "{\"ssid\":\""; json += wifi_->scanSsid(i); json += "\",";
    json += "\"rssi\":"; json += wifi_->scanRssi(i); json += ",";
    json += "\"secure\":"; json += (wifi_->scanEncryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    json += "}";
  }
  json += "]";
  // scanNetworks() allocates ~1KB per network; free it now.
  wifi_->scanDelete();
  req->send(200, "application/json", json);
}

void WebServer::onSetWifi(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                          size_t index, size_t total) {
  // We expect a single POST with a small JSON body. The first chunk
  // arrives with index=0; we accumulate and parse on total==len.
  // Stored at file scope (rather than a member) so the body outlives
  // the closure that consumes it. We never overlap two requests.
  static String body;
  if (index == 0) body = "";
  for (size_t i = 0; i < len; ++i) body += (char)data[i];
  if (index + len != total) return;  // wait for more
  if (body.length() == 0) {
    req->send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }

  // Tiny hand-rolled JSON parse: locate the ssid and pass values
  // between their double quotes. Avoids pulling in ArduinoJson for
  // a 4-key payload.
  String ssid;
  String pass;
  {
    String pat = "\"ssid\":\"";
    int s = body.indexOf(pat);
    if (s >= 0) {
      s += pat.length();
      int e = body.indexOf('"', s);
      if (e > s) ssid = body.substring(s, e);
    }
  }
  {
    String pat = "\"pass\":\"";
    int s = body.indexOf(pat);
    if (s >= 0) {
      s += pat.length();
      int e = body.indexOf('"', s);
      if (e > s) pass = body.substring(s, e);
    }
  }
  if (ssid.isEmpty()) {
    req->send(400, "application/json",
              "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }
  config_->setWifiCredentials(ssid, pass);
  req->send(200, "application/json",
            "{\"ok\":true,\"reboot_in_ms\":2000}");
  // Schedule a deferred restart so the HTTP response can flush first.
  restart_pending_ = true;
  restart_at_ms_ = millis() + 2000;
  Serial.printf("[HTTP] wifi set: SSID=%s, restarting in 2s\n",
                ssid.c_str());
}

void WebServer::onResetWifi(AsyncWebServerRequest* req) {
  config_->clearWifiCredentials();
  // Also kick the wifi manager back to AP right now (we don't need to
  // reboot to drop the link; AP mode can take over immediately).
  // The next reset will start fresh.
  req->send(200, "application/json", "{\"ok\":true}");
  restart_pending_ = true;
  restart_at_ms_ = millis() + 1500;
  Serial.println("[HTTP] wifi reset requested, restarting in 1.5s");
}

void WebServer::onCaptiveRedirect(AsyncWebServerRequest* req) {
  // Every request that doesn't match a known route redirects to the
  // provisioning page. This is what makes the OS auto-pop the portal
  // page when a phone joins the AP.
  if (wifi_->mode() == WifiMode::kAp) {
    req->redirect("http://" + wifi_->softApIp().toString() + "/provision");
  } else {
    req->send(404, "text/plain", "Not Found");
  }
}

}  // namespace farmers
