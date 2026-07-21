# Changelog

All notable changes to LionWifi are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions use semver.

## 1.2.0 — 2026-07-21

### Added
- HTTP OTA (opt-in, `-D LIONWIFI_HTTP_OTA`): a `/update` endpoint that flashes
  firmware from a **forward** POST (browser or `curl -F "fw=@firmware.bin"`),
  unlike ArduinoOTA's reverse connection — so it works across NAT / separate
  subnets / VPN segments where `espota` can't connect back to the host.
  Hand-rolled on every backend (ESP8266, ESP32-sync, ESP32-async) so it drives
  the `Update` object directly: the `RegisterOta*` hooks fire and progress is
  logged through the global `Logger` uniformly (`HTTP OTA: start …` / `… OK`).
  The GET page offers two forms — sketch and **filesystem image**; the FS target
  is selected by `?fs=1` (and, on the sync servers, the `filesystem` field name),
  which unmounts the FS and flashes the FS partition. HTTP Basic auth via
  `WEB_SERVER_AUTH_*` (or `-D NO_AUTH`); reboots on success. (Sketch OTA is tested;
  filesystem-image OTA is not yet hardware-verified — treat it as experimental.)

## 1.1.1 — 2026-07-08

### Added
- File-browser page (`/spiffs/ls`, `/sd/ls`) now ships a small built-in
  stylesheet (light + dark theme, ~2 KB flash) and is fully restyleable via
  documented, stable CSS classes (`.fs-wrap`, `.fs-table`, `td.name/.size/.time/
  .actions`, `tr.dir`, `.fs-link`, `.act`, `.act-del`, `.fs-summary`, `.fs-home`,
  `.fs-btn`). Configure with build flags: `-D FS_BROWSER_CSS="..."` replaces the
  sheet (the default is exposed as `DEFAULT_FS_BROWSER_CSS` so you can prepend
  overrides and append it back), `-D NO_FS_BROWSER_CSS` drops it entirely and
  reclaims the ~2 KB. Copy-paste examples in `examples/LionWifiFull`.

### Fixed
- ESP8266: `ESP.getHeapStats()` ran on every `Loop()` pass — a full umm-heap walk
  with interrupts disabled, thousands of times/sec. Now throttled (default 1 s,
  `-D HEAP_CHECK_INTERVAL_MS`), matching the FS-space check; `ESP.wdtFeed()` still
  runs every pass. (#5)
- Uptime/"Restarted" on the status page could freeze or go negative when the clock
  was stepped after boot (e.g. SNTP correcting an ESP32 RTC that jumped forward
  during an OTA reflash). `GetStartupTime()` now derives the boot epoch from the
  monotonic clock (`now - uptime`), so it self-heals. (#4)

### Changed
- File-browser markup modernized (`thead`/`tbody`/`tfoot`, `<meta viewport>`,
  dropped `<font>` tags). Folder rows had an extra unlabeled `DIR` cell that
  misaligned their columns; folders now show `DIR` in the Size column and line up.

## 1.1.0 — 2026-07-04

### Added
- OTA event hooks on `WifiConnector`, forwarded to `MyOta` (and safe to register
  before or after `Setup()`): `RegisterOtaStartEvent(void(bool sketchUpload))`,
  `RegisterOtaProgressEvent(void(unsigned progress, unsigned total))`,
  `RegisterOtaEndEvent(void(bool ok))`. Lets a consumer quiesce heavy peripherals
  (e.g. an ESP32-HUB75 I2S-DMA matrix) during OTA without replacing LionWifi's
  own ArduinoOTA callbacks. Start fires after the FS unmount; end fires on
  success (`ok=true`) and on error (`ok=false`). (#2)
- `StatusHtml()` now renders a WiFi line (SSID, RSSI in dBm with a color-coded
  good/ok/weak/poor verdict, and channel) after the memory stats. Shown by
  default; disable with `-D NO_WIFI_STAT_IN_STATUS`. Carries a `wifi-status` CSS
  class for styling and is emitted only while connected. (Consumers that
  rendered this themselves after `StatusHtml()` — e.g. TempViewer_esp32 — can
  drop their copy.)

### Fixed
- ESP32 async without `USE_SD_CARD`: the root path `"/"` was never registered, so
  requesting `/` 404'd on `/index.html` instead of serving `index_nosd.html`. Now
  registered in the ESP32-async branch, mirroring the sync path. (#1)

### Changed
- `StatusHtml()` no longer emits a trailing `<br>` after the memory-status block,
  so consumers appending their own rows get consistent spacing (control vertical
  gaps via CSS instead). Consumers that added a matching `<br>` to compensate can
  drop it. (#3)

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
