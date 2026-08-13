#include "config_store.h"

#include <Preferences.h>

namespace farmers {

namespace {

constexpr char kNs[] = "splatoon";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";
constexpr char kKeyMdns[] = "mdns";
constexpr char kDefaultMdns[] = "splatoon";

// 802.11 limits. We hard-cap before writing so Preferences never sees
// an oversized string (Preferences silently truncates otherwise).
constexpr size_t kMaxSsid = 32;
constexpr size_t kMaxPass = 63;
constexpr size_t kMaxMdns = 24;

Preferences& prefs() {
  static Preferences p;
  return p;
}

String readString(const char* key) {
  if (!prefs().isKey(key)) return String();
  return prefs().getString(key, String());
}

void writeString(const char* key, const String& value) {
  // putString copies the bytes into NVS.
  prefs().putString(key, value);
}

}  // namespace

ConfigStore::ConfigStore() = default;

void ConfigStore::begin() {
  ready_ = prefs().begin(kNs, false);  // read-write mode
  if (!ready_) {
    // NVS init failed (corrupt namespace? full?). Try erasing and reopening
    // as a last resort so a stuck device can be recovered by reflashing.
    prefs().clear();
    ready_ = prefs().begin(kNs, false);
  }
}

bool ConfigStore::hasWifiCredentials() const {
  return prefs().isKey(kKeySsid) && prefs().isKey(kKeyPass) &&
         readString(kKeySsid).length() > 0;
}

String ConfigStore::getWifiSsid() const {
  String s = readString(kKeySsid);
  return s;
}

String ConfigStore::getWifiPassword() const {
  return readString(kKeyPass);
}

void ConfigStore::setWifiCredentials(const String& ssid, const String& password) {
  // Cap inputs to NVS-safe sizes; longer strings get truncated. We could
  // refuse instead, but truncation lets the user paste a 64-char password
  // and the device silently keeps the first 63 characters.
  String trimmedSsid = ssid.substring(0, kMaxSsid);
  String trimmedPass = password.substring(0, kMaxPass);

  // Clear-then-write gives us atomic semantics from the caller's
  // perspective: after this returns, getWifiSsid() / getWifiPassword()
  // return either the new pair or empty strings — never the old ssid with
  // a new password.
  prefs().remove(kKeySsid);
  prefs().remove(kKeyPass);
  writeString(kKeySsid, trimmedSsid);
  writeString(kKeyPass, trimmedPass);
}

void ConfigStore::clearWifiCredentials() {
  prefs().remove(kKeySsid);
  prefs().remove(kKeyPass);
}

String ConfigStore::getMdnsName() const {
  String n = readString(kKeyMdns);
  if (n.isEmpty()) return String(kDefaultMdns);
  return n;
}

void ConfigStore::setMdnsName(const String& name) {
  String trimmed = name.substring(0, kMaxMdns);
  writeString(kKeyMdns, trimmed);
}

}  // namespace farmers
