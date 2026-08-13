#pragma once

#include <stdint.h>

#include <Arduino.h>

// Persistent key-value store backed by ESP32 NVS (Non-Volatile Storage).
// Backed by Arduino's Preferences wrapper. We expose a tiny typed surface
// so callers do not have to know about Preferences' open/close semantics
// or its bytes-vs-string quirks.
//
// The store survives power cycles and reflashes. Data is written through
// the NVS wear-leveling layer; small frequent writes are safe but we still
// keep the call count low (one write per provisioning event).
//
// The NVS namespace is fixed ("splatoon") and intentionally short so each
// key string fits in the 15-byte NVS key limit. Keys:
//   ssid   -> ssid string (up to 32 chars; 802.11 max is 32 bytes)
//   pass   -> password string (up to 63 chars; WPA2 max is 63 bytes)
//   mdns   -> mDNS host label (default "splatoon")
namespace farmers {

class ConfigStore {
 public:
  ConfigStore();

  // Open the underlying NVS namespace. Idempotent. Must be called once at
  // setup() before any getter / setter.
  void begin();

  // True if a non-empty SSID + password are stored. AP-mode provisioning
  // uses this to decide whether to start in STA or fall back to AP.
  bool hasWifiCredentials() const;

  String getWifiSsid() const;
  String getWifiPassword() const;
  // Write both fields atomically (Preferences has no transaction; we
  // clear-then-write so a partial write is at worst "empty" not "half").
  void setWifiCredentials(const String& ssid, const String& password);

  // Wipe ssid + password. mDNS name is preserved.
  void clearWifiCredentials();

  // mDNS host label (default "splatoon"). Survives credential clears so
  // the user's chosen name persists across re-provisioning.
  String getMdnsName() const;
  void setMdnsName(const String& name);

 private:
  bool ready_ = false;
};

}  // namespace farmers
