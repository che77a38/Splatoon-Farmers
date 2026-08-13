#include "wifi_manager.h"

#include <WiFi.h>

namespace farmers {

namespace {

// Backoff schedule. Caps at 60 s so a long-lived outage still keeps
// trying. 1s, 2s, 4s, 8s, 16s, 32s, 60s, 60s, ...
constexpr uint32_t kBackoffTable[] = {
    1000, 2000, 4000, 8000, 16000, 32000, 60000,
};
constexpr uint8_t kBackoffTableSize =
    sizeof(kBackoffTable) / sizeof(kBackoffTable[0]);

bool isCredentialError(wl_status_t status) {
  // arduino-esp32 2.0.17 does not expose WL_WRONG_PASSWORD; treat any
  // non-empty status that means "your saved credentials are wrong" as a
  // credential error. WL_CONNECT_FAILED covers bad password and bad
  // security mode. WL_NO_SSID_AVAIL covers SSID gone (router removed).
  return status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
}

}  // namespace

WifiManager::WifiManager() = default;

uint32_t WifiManager::backoffMs(uint8_t attempt) {
  if (attempt == 0) return 0;
  if (attempt - 1 >= kBackoffTableSize) return kBackoffTable[kBackoffTableSize - 1];
  return kBackoffTable[attempt - 1];
}

void WifiManager::begin(ConfigStore* config) {
  config_ = config;

  // Build a stable AP SSID using the chip's MAC tail so two boards in
  // the same room don't collide.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char tail[5];
  snprintf(tail, sizeof(tail), "%02X%02X", mac[4], mac[5]);
  ap_ssid_ = String("SplatoonFarmers-") + tail;

  if (config_->hasWifiCredentials()) {
    startStaWithBackoff();
  } else {
    startAp();
  }
}

void WifiManager::startStaWithBackoff() {
  mode_ = WifiMode::kStaConnecting;
  sta_attempt_ = 0;
  next_sta_try_ms_ = millis();
  WiFi.mode(WIFI_STA);
  // Pass try_connect=false (5th arg) so begin() returns immediately
  // instead of blocking on the association. Status is polled in tick();
  // this keeps setup() non-blocking so the Serial banner prints before
  // the radio spends seconds trying to associate.
  WiFi.begin(config_->getWifiSsid().c_str(),
            config_->getWifiPassword().c_str(), 0, nullptr, /*try_connect=*/false);
  // begin(ssid, pass, ch, bssid, false) does not start the association.
  // Kick off the first connect attempt here so the radio actually
  // tries to associate; subsequent retries are handled in tick().
  WiFi.reconnect();
  last_sta_event_ms_ = millis();
}

void WifiManager::startAp() {
  credential_failures_ = 0;
  mode_ = WifiMode::kAp;
  WiFi.mode(WIFI_AP);
  // Empty password keeps the AP discoverable without a one-time secret,
  // matching the industry pattern for "configure me" SSIDs. The captive
  // portal authenticates the device by being on the same network; the
  // threat model is a friendly household, not a coffee shop.
  WiFi.softAP(ap_ssid_.c_str());
  Serial.printf("[WiFi] AP up: SSID=%s, IP=%s\n",
                ap_ssid_.c_str(),
                WiFi.softAPIP().toString().c_str());
}

void WifiManager::tick() {
  if (mode_ == WifiMode::kStaConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      onStaGotIp();
      return;
    }
    if (millis() < next_sta_try_ms_) return;
    // Trigger WiFi events. WiFi.onEvent() callbacks (onStaDisconnected)
    // own the credential-failure counter so this tick only schedules the
    // next try.
    if (sta_attempt_ == 0) {
      // First try is in flight; just wait for the event.
      sta_attempt_ = 1;
      return;
    }
    sta_attempt_ += 1;
    const uint32_t wait = backoffMs(sta_attempt_);
    next_sta_try_ms_ = millis() + wait;
    Serial.printf("[WiFi] reconnect attempt #%u in %u ms\n", sta_attempt_,
                  wait);
    WiFi.reconnect();
  }
}

void WifiManager::onStaGotIp() {
  mode_ = WifiMode::kSta;
  credential_failures_ = 0;
  sta_attempt_ = 0;
  last_ip_ = WiFi.localIP().toString();
  Serial.printf("[WiFi] connected: SSID=%s, IP=%s\n",
                WiFi.SSID().c_str(), last_ip_.c_str());
}

void WifiManager::onStaDisconnected(wl_status_t status) {
  // The framework dispatches this from the WiFi event task. Defer the
  // bookkeeping to the next tick to keep the ISR-free path short.
  if (isCredentialError(status)) {
    credential_failures_ += 1;
    Serial.printf("[WiFi] credential error #%u (status=%d)\n",
                  credential_failures_, (int)status);
    if (credential_failures_ >= kMaxCredentialFailures) {
      Serial.println("[WiFi] 3 credential failures -> dropping to AP mode");
      config_->clearWifiCredentials();
      startAp();
      return;
    }
  }
  // Bounce back into connecting state with the next backoff slot.
  mode_ = WifiMode::kStaConnecting;
  // First attempt: zero delay (the connect already happened). Subsequent
  // attempts: schedule the next try via tick().
  next_sta_try_ms_ = millis();
  last_sta_event_ms_ = millis();
}

String WifiManager::localIp() const {
  if (mode_ == WifiMode::kSta) return last_ip_;
  if (mode_ == WifiMode::kAp) return WiFi.softAPIP().toString();
  return String();
}

uint8_t WifiManager::apClients() const {
  return WiFi.softAPgetStationNum();
}

void WifiManager::resetCredentials() {
  if (config_) config_->clearWifiCredentials();
  // Tear down whatever mode we are in and start fresh as AP.
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  startAp();
}

uint8_t WifiManager::statusCode() const {
  switch (mode_) {
    case WifiMode::kSta:
      return 0;
    case WifiMode::kStaConnecting:
      return 1;
    case WifiMode::kAp:
      // Code 4 distinguishes "AP because no creds" (2) from "AP after
      // 3 failed attempts" (3). The frontend doesn't need the
      // distinction yet so we collapse both to AP.
      return 4;
  }
  return 4;
}

const char* WifiManager::statusMessage() const {
  switch (statusCode()) {
    case 0: return "connected";
    case 1: return "connecting";
    case 2: return "no-credentials";
    case 3: return "bad-credentials";
    case 4: return "ap-active";
    default: return "unknown";
  }
}

}  // namespace farmers
