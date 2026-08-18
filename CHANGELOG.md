# Changelog

All notable changes to LionWifi are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions use semver.

## 1.3.0 — 2026-08-18

### Added
- **File rename** in the FS browser: a `Ren` action in the listing (JS prompt
  for the new name) backed by `/ren/<f>?to=<new>` (internal FS) and
  `/sd/ren/<f>?to=<new>` (SD). The target gets a leading `/` if missing; an
  existing target is never overwritten — the refusal comes back as a **409
  with the reason** (target exists / source missing / FS busy). Internal-FS
  renames go under the Logger FS semaphore, like delete. Works on both server
  flavors (sync ESP8266 / ESP32 `NO_ASYNC_WEB_SERVER`, and ESP32 async).

## 1.2.2 — 2026-08-13

### Fixed
- `/tail/<f>` and `/log/tail` on the sync server (ESP8266) returned an **empty
  body** for large files. The tail (up to `TailSize`, 8 KB) was read into a
  `char[]` and passed to `send(200, type, buf)`, which copied it into a `String`
  — a second large allocation that fails on a fragmented ESP8266 heap, so the
  response body was 0-length (while the debug log still printed the intended byte
  count). Now the tail is streamed in 512-byte chunks with a correct
  `Content-Length` and no large buffer at all.

## 1.2.1 — 2026-08-01

### Changed
- File-browser listing (`/spiffs/ls`, `/sd/ls`) on the ESP32 async server now
  streams the HTML through `beginChunkedResponse` instead of building the whole
  page into one buffer. Memory use is now independent of file count (a few KB of
  generator state instead of ~20–35 KB of contiguous heap for a large listing),
  so the page no longer risks a failed allocation / crash on a fragmented heap.
  The sync (ESP8266 / `NO_ASYNC_WEB_SERVER`) path already streamed via
  `ServerStream` and is unchanged in behaviour; both share new render helpers.
- Breadcrumb debug logs around listing (request received, file count, streaming
  start) to aid field diagnosis.

### Fixed
- Apply the NTP timezone offset (`configTime`) immediately at boot in `Setup()`,
  so timestamps are in local time from the first log line rather than only after
  the first successful NTP sync.

### Added
- **ESP32 RTC clock persistence** (opt-out via `-D LIONWIFI_NO_RTC_CLOCK`;
  on by default on ESP32). The wall clock is checkpointed to `RTC_NOINIT` slow
  memory each loop and restored very early on boot via
  `WifiConnector::RestoreClockFromRtc()`, so time survives a **software** reset
  (OTA, `/restart`, crash) without waiting for NTP. Lost on true power loss (the
  magic no longer matches → restore is skipped), which is correct. Works around
  ESP-IDF not persisting SNTP time across a reset. The snapshot variables are
  defined once in the new `WifiConnector.cpp` translation unit.

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
