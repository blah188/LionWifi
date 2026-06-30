# Changelog

All notable changes to LionWifi are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions use semver.

## 1.0.0 — 2026-06-30

First public release. Extracted and hardened from a private monorepo
(`LocalLibs/WifiConnector`).

### Added
- `WifiConnector` — multi-SSID connect with auto-reconnect, AP rotation on
  timeout, optional router-ping reboot watchdog, NTP time-set, periodic log
  maintenance, connect/disconnect/time-set/log-clear/ping event callbacks, a
  shared `WiFiClient`/`HTTPClient`, and an HTML status/uptime page.
- `FsBrowser` — web filesystem browser (SPIFFS/LittleFS, optional SD): listing
  with upload, file tail/download/delete, log views, format route, optional
  HTTP Basic auth.
- `MyOta` — ArduinoOTA wrapper (unmounts/remounts the FS around a filesystem OTA).
- `examples/LionWifiBasic` with a sample `data/index_nosd.html` home page.
- Documented every build-time `#define` and HTTP endpoint in the header banner.

### Changed
- Dropped the bundled `nonstd::function` dependency — callbacks now use
  `std::function`.
- Unified filesystem handling behind a single `LIONWIFI_FS` macro; a build now
  must define exactly one of `USE_SPIFFS` / `LFS` (enforced with `#error`).
- Removed the injected `ILogger*` — diagnostics use the global LionLogger `Logger`.
- Default web-auth credentials are now `admin`/`admin` (override via build flags);
  the NTP TZ offset and server are build flags (`NTP_TZ_OFFSET_SEC`/`NTP_SERVER`).
- `GetContentType` returns a flash string; `sort` renamed to `DirEntrySort`;
  helper methods normalized to PascalCase.

### Fixed
- OTA progress divide-by-zero for tiny/early images.
- Boot-reboot loop from an un-armed fatal-reconnect timer.
- ESP32 cross-task race on the shared client (router ping uses a private client).
- `.png` files were served as `text/plain` (unreachable MIME branch).
- ESP8266 log "tail" sent a full-file `Content-Length` (client hang on big logs).
- Upload now reports a failure instead of a silent truncated file when the FS fills.
- Uninitialized members, a signed log-clear overflow, missing `localtime()`
  NULL-checks, and several format-specifier mismatches.
- WiFi PSK is no longer written to the logs.

### Security
- Non-copyable `WifiConnector` / `FsBrowser` / `MyOta` (single-instance owners).
