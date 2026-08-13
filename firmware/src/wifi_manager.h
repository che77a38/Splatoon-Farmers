#pragma once

#include <Arduino.h>

#include <WiFi.h>

#include "config_store.h"

namespace farmers {

// WiFi operating mode. Drives the start decision in setup() and the
// reconnect / re-provision logic in tick().
enum class WifiMode : uint8_t {
  kStaConnecting,  // trying to join the saved AP
  kSta,            // joined, IP acquired
  kAp,             // own AP up; user must provision
};

class WifiManager {
 public:
  WifiManager();

  // Pull saved credentials and either start STA or fall back to AP.
  // Idempotent — call once from setup(). Safe to call again after
  // resetCredentials() to bounce the connection.
  void begin(ConfigStore* config);

  // Drive the reconnect / AP-fallback state machine. Call from loop().
  // Cheap when state is steady (no WiFi API calls in kSta).
  void tick();

  // Mode + address accessors. localIp() is empty until kSta is reached.
  WifiMode mode() const { return mode_; }
  String localIp() const;
  String apSsid() const { return ap_ssid_; }
  uint8_t apClients() const;
  // mDNS host label, e.g. "splatoon" — users visit
  // http://splatoon.local instead of typing the IP.
  String mdnsName() const { return mdns_name_; }

  // Forget saved credentials + jump to AP. Used by the captive portal's
  // "reset" button and by the long-press BOOT path.
  void resetCredentials();

  // Snapshot of the connection state for serial / web UI banners.
  // ReasonCodes: 0 = connected, 1 = connecting, 2 = no creds, 3 = bad
  // password / SSID missing (credential error), 4 = AP active.
  uint8_t statusCode() const;
  const char* statusMessage() const;

  // mDNS setup. Called once after the radio is up; safe to call from
  // either STA (announces the IP) or AP (announces the softAP IP) mode.
  // Returns true if mDNS responder started; false on name conflict or
  // no mode yet — the caller can fall back to "splatoon-XXXX" via
  // restartWithUniqueMdnsName().
  bool startMdns();
  // On a name collision (e.g. another device on the LAN already claims
  // "splatoon") restart with a per-device suffix derived from the MAC
  // tail. Returns the final hostname chosen.
  String ensureUniqueMdnsName();

 private:
  void startStaWithBackoff();
  void startAp();
  void onStaGotIp();
  void onStaDisconnected(wl_status_t status);

  static uint32_t backoffMs(uint8_t attempt);

  ConfigStore* config_ = nullptr;
  WifiMode mode_ = WifiMode::kAp;
  String ap_ssid_;
  String mdns_name_ = "splatoon";
  String last_ip_;

  // Backoff state
  uint8_t sta_attempt_ = 0;
  uint32_t next_sta_try_ms_ = 0;
  uint32_t last_sta_event_ms_ = 0;

  // Credential-error counter. WL_CONNECT_FAILED or WL_NO_SSID_AVAIL
  // bumps this. Three in a row drops to AP. Other failures (no IP yet,
  // disconnected mid-session) do NOT bump this.
  uint8_t credential_failures_ = 0;
  static constexpr uint8_t kMaxCredentialFailures = 3;
};

}  // namespace farmers
