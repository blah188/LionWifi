# Changelog

All notable changes to LionWifi are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions use semver.

## 1.6.0 — 2026-09-11

### Added
- **`WIFI_BEST_AP` now works on the ESP8266 too** (still off by default, on both cores).
  That core has no `setScanMethod`/`setSortMethod`, so the ESP32 trick of asking the stack
  for an all-channel scan is unavailable: the library now scans itself before each
  association, keeps the loudest point carrying the SSID, and passes its channel and BSSID
  to `begin()`. When the SSID is nowhere in the scan it falls back to a plain `begin()` —
  better a coin toss than no association at all. The pin binds one attempt only; a point
  that has gone away simply times out and the next retry scans again. Costs the same
  ~2 s sweep per attempt as the ESP32 branch, plus the scan result list — which is why the
  scan passes an SSID filter down to the SDK's `scan_config` instead of collecting every
  access point in the neighbourhood; it is freed immediately via `scanDelete()` either way,
  but it is a real allocation on a node with ~15 KB of heap.
  `Connect()` and `Reconnect()` now share one `BeginStation()` so they cannot aim at
  different points.
- **`LIONWIFI_GARP_INTERVAL_MS`** — broadcast a gratuitous ARP ("this IP is at this MAC")
  on every association and then every N ms. **OFF by default** (0 compiles the feature out);
  it costs airtime on every node that enables it and only pays off where several access
  points share the SSID. Cures the case where a node re-associates to a DIFFERENT point and
  then stays invisible to part of the network for minutes: the node's own traffic is unicast
  to the router, so it refreshes that one path, while the ARP caches of other nodes and the
  forwarding tables of the other points keep pointing at the radio it left. Observed on a
  window-opener node: after a network blip at 22:49 it re-associated to another point and
  was unreachable for ~12 hours while cheerfully publishing MQTT the whole time; a later
  reboot moved it again and it took 10.5 minutes for one of the pollers to see it. A
  broadcast reaches the whole L2 domain at once. 120000 ms is a sane interval — much below
  that just burns airtime, since broadcasts go out at the lowest basic rate and wake every
  client. On ESP32 the send is handed to the tcpip task via `tcpip_callback` (calling
  `etharp_*` from another task corrupts lwIP state); on ESP8266 it is a direct call, as the
  whole Arduino core there talks to lwIP from `loop()`.
  Each tick also compares the current BSSID and channel against the last announced pair,
  which makes it a **silent-roam detector**: the SDK re-associates by itself after a deauth
  and can land on a different point carrying the same SSID without `Connect()` running at
  all — and when that beats one `Loop()` pass, `_connected` never flips, so not even the
  "Connected to" line appears. Logging follows the usual dedup rule: `GARP` (debug) while
  nothing changes, `GARP: AP CHANGED to <bssid> ch N, RSSI x` (info) when it does, and
  `GARP armed on <bssid> ch N, every Nms` on association.

## 1.5.0 — 2026-09-10

### Added
- **`WIFI_BEST_AP`** — connect to the strongest access point carrying the configured SSID
  instead of the first one the scan happens to find (ESP32). The default `WIFI_FAST_SCAN`
  stops at the first match, and sorting by signal — already the default — has nothing to
  sort until the scan covers every channel. With several points sharing an SSID this is a
  coin toss: a hub was found sitting on an extender 20 m away through a wall at RSSI -82
  while another point stood one metre from it; the flag turned that into -41 immediately.
  Costs a full channel sweep (a second or two) per association attempt.
- **Association failures now say why.** A `WiFi.onEvent` hook records the
  `wifi_err_reason_t` from `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` and `Loop()` prints it as
  `WiFi disconnect reason N` — 201 `NO_AP_FOUND` (not seen at all), 15
  `4WAY_HANDSHAKE_TIMEOUT` (seen, handshake never completes: weak link or a supply sagging
  on TX peaks), 2/4 `AUTH_EXPIRE`/`ASSOC_EXPIRE` (the access point drops us). Before this a
  failing connect logged nothing but `Connecting/Reconnecting` forever, which covers several
  unrelated faults. `ASSOC_LEAVE` (8) is filtered out: that is our own `disconnect()`.
  The callback runs in the Arduino event task, so it only records the code — the logger
  tolerates a single writing task, so printing happens in `Loop()`.
- **Web timing diagnostics, off by default.** `LIONWIFI_WEB_TRACE` prints `WEB <ms>` to
  Serial for calls slower than `LIONWIFI_WEB_TRACE_MS` (200) and times the individual chunks
  of a served file/log tail; `LIONWIFI_WEB_STALL_LOG_MS` puts slow calls in the log as
  `WEB STALL <ms>`. Deliberately no URI and no client IP: `_handleRequest()` clears
  `_currentUri` when it finishes, and `remoteIP()` on the closed client calls
  `getpeername()` on a dead fd, ignores the failure and returns an uninitialised address —
  which printed convincing public IPs out of stack garbage. Attribute a stall to a page by
  instrumenting the handler, not from here.
- **RSSI in the connect line** (`Connected to X; IP address: Y; RSSI -63`) — the one number
  that separates "weak link" from every other explanation, on both cores.
- **`WIFI_TX_POWER`** — sets the station TX power explicitly to any `wifi_power_t`
  enumerator, and it exists to be set LOWER (`-D WIFI_TX_POWER=WIFI_POWER_8_5dBm`). Small
  boards - the ESP32-C3 SuperMini and its relatives - have weak supply decoupling and
  misbehave at full power: they fail to associate, or associate and drop. `MAX_WIFI_POWER`
  remains, but it only states an intent, since maximum is already the default.
- **`WIFI_BW20`** — forces a 20 MHz channel (HT20) on the station link instead of the
  default 40. A narrower channel collects less noise, so a weak link holds better: at
  -76 dBm against an access point running 40 MHz on a busy channel this buys real SNR.
  Both radio settings are applied after `WiFi.mode()` (before it esp_wifi is not yet
  initialised) and before `WiFi.begin()` (applied later they would only take effect on the
  next reconnect).

### Fixed
- **Retries never restarted the association (ESP32).** `Reconnect()` called `WiFi.begin()`
  on top of an attempt that was still running, and on ESP32 that does not restart it — least
  of all with a different SSID: the previous attempt runs to its own timeout and the new
  credentials are ignored. On an ESP32-C3 this showed as an endless `Changing AP /
  Reconnecting` ladder, minutes on end, with an access point in the same room. It now tears
  the station down first, exactly as `Connect()` always did — which is why the FIRST attempt
  worked. **ESP8266 keeps the bare `begin()`**: its SDK copes with a repeated `begin()`, the
  symptom has never appeared there, and a teardown would power the radio down on a path that
  a fleet of nodes has used for years.
  The tuning now lives in one place (`ApplyRadioSettings()`) shared by the first attempt
  and every retry, and it runs BEFORE the teardown — which looks wrong but is not:
  `disconnect(true)` stops the radio while leaving the configured mode at `WIFI_MODE_STA`,
  and `WiFiGenericClass::mode()` returns early when the mode already matches
  (`WiFiGeneric.cpp:1252`), so tuning after a teardown lands on a stopped stack and is
  silently lost (no caller checks those return codes). Measured: with the order reversed,
  association failed with reason 2 (`AUTH_EXPIRE`) and 39 (`TIMEOUT`) retry after retry.
- **`DISABLE_11N` configured the wrong interface** and therefore did nothing: it called
  `esp_wifi_set_protocol(WIFI_IF_AP, ...)`, the access-point interface, which a station-only
  sketch never brings up. Now `WIFI_IF_STA`. No consumer in the fleet had the flag set, so
  nobody depended on the old no-op.
- **Shared HTTP client reused connections across hosts** (ESP32 only, and it did not need
  ESP32 to be wrong - only different). `SharedHttpClient(url)` hands the same `WiFiClient` to
  `HTTPClient::begin()`, and the ESP32 core's `begin()` sets `_canReuse = true` before
  `disconnect(true)`, which under reuse deliberately leaves the previous socket open "otherwise
  it will free some of the memory used by _client". `HTTPClient::connect()` then returns early
  on `connected()` alone and never compares the host, so a request meant for the next node was
  written into the socket of the previous one. Even against the same node it broke: a small ESP
  web server answers `Connection: keep-alive` with a two-second idle timeout, so a poller
  returning half a minute later wrote into a peer that had long closed and got
  HTTPC_ERROR_READ_TIMEOUT (-11) - indistinguishable from a slow device, and the reason a pile
  of -11/-1/EmptyInput/InvalidInput errors were blamed on the nodes for months. The ESP8266
  core does the opposite in the same function (`_canReuse = false` with a comment saying it is
  cleared so that disconnect closes), which is why the very same code was well-behaved there.
  The shared client now has `setReuse(false)`: one connection per request, which is what these
  peers expect anyway.
- **Did not link on RISC-V targets** (ESP32-C3 and its relatives): the directory scans and the
  chunked listing called `rtc_wdt_feed()`, and `soc/rtc_wdt.h` exists only for the original
  ESP32 — everywhere else the build ended with `undefined reference to rtc_wdt_feed`. The call
  now sits behind `LIONWIFI_WDT_FEED()`, which pokes the RTC watchdog only where that API
  exists and, on every ESP32 target, yields a tick to the idle task. The tick is the part that
  matters: the RTC watchdog is the bootloader's, while the one that can actually fire in these
  loops is the TASK watchdog, and that watches the idle task — poking anything does not satisfy
  it. It is needed on any core, too: `CORE_WIFI` is a build flag, so a consumer can pin this
  task to core 0, where a long scan starves the watched idle task into a panic.

## 1.4.0 — 2026-09-05

### Changed
- **Dependencies are declared by name, without the `leva` owner.** An owner-qualified
  requirement can only be satisfied by a registry package, so a consumer who replaces one of
  these libraries with `symlink://` for local work still got the registry copy installed
  beside it — and the installed directory shadows the link, silently building sources nobody
  is editing. Name-only requirements let an installed (symlinked) package of the right
  version satisfy them. The names are unique enough that resolution stays unambiguous, and
  `library.properties` has always listed them this way.

### Added
- **Compile-time guards for synchronous logging on ESP32.** LionLogger built without
  `ASYNC_LOG` writes the log file in the caller's own context through a shared `File`
  member, so the firmware may log from exactly one FreeRTOS task — a second one closes
  that handle underneath the first. LionWifi adds such a task in two configurations, and
  in both it is the library's *own* logging (reconnects, OTA progress, the file browser),
  so "my handlers don't log" is no defence. Both are visible to the preprocessor, so on
  ESP32 without `ASYNC_LOG` the header now `#error`s unless `NO_WIFI_TASK` and
  `NO_ASYNC_WEB_SERVER` are both defined, instead of leaving a log file to be corrupted
  in the field.

### Fixed
- **File upload crashed the sync server on ESP32** (`NO_ASYNC_WEB_SERVER`): a panic in
  `WebServer::_parseFormUploadAborted()`, preceded by nonsense in the log such as
  `Invalid request: GIF89a`. The upload callback was answering the client — `DoAuth()`
  sends a 401 on failure, and `UPLOAD_FILE_END` sent the 303 redirect — while the request
  body was still being parsed. That desynchronises the connection: the rest of the body is
  then read as the next request line, and the parser eventually takes the abort path, where
  the ESP32 core dereferences a null `_currentUpload`. This header already documented the
  rule for the async server ("the onUpload callback may not send the HTTP response"); the
  sync branch now follows it too — the callback records the outcome, and the route's
  `onRequest` handler (which runs once the whole body is consumed) answers.
  ESP8266 was unaffected: its WebServer tolerates the early response.
- **`UPLOAD_FILE_ABORTED` was not handled at all** in the sync branch, so an interrupted
  upload left the file handle open and kept the previous outcome recorded.
- **`Loop()` called from the sketch is now a no-op where the connector owns a task**
  (ESP32 without `NO_WIFI_TASK`). Consumers keep one `_connector->Loop()` in `loop()` for
  every platform; on such a build that pumped everything from two contexts at once — two
  parsers reading the same socket, half a request each. With the sync web server it looks
  like broken authentication: `Invalid request: Referer: ...` in the log, `remoteIP()`
  reported as `0.0.0.0`, the same URI handled twice, `Connection reset by peer`. Nothing
  hinted at the real cause, so the call is now refused — and it says so once in the log
  ("Loop() from the sketch IGNORED...") instead of failing silently.

### Added
- **`WebServerType`** — a typedef for the concrete web-server class of this build
  (`ESP8266WebServer` / `AsyncWebServer` / `WebServer`). Use it in your own signatures
  instead of naming a platform class, and porting a sketch changes build flags rather than
  code. Same idea as `LIONWIFI_FS` for the filesystem.
- **`LIONWIFI_NO_ARDUINO_OTA`** — compiles ArduinoOTA out. That is the espota upload path,
  and its `begin()` also starts an mDNS responder: together ~34KB of flash on ESP32, useless
  to a build that flashes through the HTTP OTA page. The class and its hooks stay and
  `Begin()`/`Loop()` become no-ops, so no consumer code changes.
- **Stack low-water mark**, logged when it drops (`----> New free stack = N`) and shown on
  the status page next to the current value. The current free stack says nothing about the
  rare deep excursion that actually overflows; only a low-water mark catches those. Costs
  one comparison per heap check, and compiles out with `NO_MEMSTAT_IN_STATUS`.
- **Compile-time warning for `NO_ASYNC_WEB_SERVER` without `NO_WIFI_TASK`**: that pair leaves
  the sync web server pumped from the connector's task, so route handlers run concurrently
  with `loop()`. It works for a sketch whose handlers touch nothing shared — hence a warning
  rather than an `#error` — but it is never what a port from ESP8266 wants.

## 1.3.4 — 2026-09-03

### Fixed
- **HTTP Basic auth answered `404: Not Found` instead of asking for a password** on the
  ESP32 async server, for every FsBrowser path served through `onNotFound` — `/tail/...`,
  `/download/...`, `/spiffs/...`, any static file. `HandleFileRead()` returned `false`
  after `DoAuth()` had already queued the 401 challenge, and `onNotFound()` treats
  `false` as "nothing was sent" and replied with its own 404, replacing the challenge.
  The browser therefore never prompted, and the same URL worked as soon as credentials
  for that realm happened to be cached — so the failure looked random, and a missing
  file and an unauthenticated request were indistinguishable. A failed auth now returns
  `true` ("response already sent"); the return value has no other consumer on the async
  path. The sync (ESP8266) path was never affected: its `onNotFound` sends nothing extra.

### Added
- **The async `DoAuth()` logs the 401** (`Need auth for <ip>: <url>`), as the sync path
  already did. Without it a 401 left no trace at all, which is what made the bug above
  read as "the log file disappeared from the filesystem".

## 1.3.3 — 2026-08-29

### Fixed
- **Web-handler stack usage on ESP8266.** File streaming (`/tail`, download) and file
  copy (`/cp`) read into a 512-byte buffer living on the *stack* of the web handler.
  With only 4 KB of cont stack that was expensive: serving an 8 KB log tail pushed the
  free-stack watermark from ~1120 down to ~620 bytes on a device that also drives
  device I/O from the same loop. The size is now `LIONWIFI_FS_CHUNK`, defaulting to
  **128 bytes on ESP8266** (ESP32 keeps 512 — its task stacks are large). The cost is
  more `read`/`sendContent` calls, not memory; measured free stack goes back to ~1010 B.

### Added
- **`LIONWIFI_FS_CHUNK`** (override with `-D`): buffer size used for streaming files
  out and for copying them. A static buffer is deliberately not used — it would spend
  that RAM permanently, which is exactly what is scarce on ESP8266.

## 1.3.2 — 2026-08-26

### Fixed
- **`/restart` rebooted twice per click** on the async (ESP32) server. The handler
  called `request->send()` and then `delay(100); ESP.restart()`, but `send()` only
  *queues* the response on the async stack — the reset tore the connection down
  before delivery, and the browser silently retried the (idempotent) GET as soon as
  the device was back, triggering a second reboot. Now the response carries
  `Connection: close` and the reboot fires from `onDisconnect()`, matching what
  1.3.1 already did for HTTP OTA.

### Added
- **Fallback reboot deadline** (`REBOOT_FALLBACK_MS`, default 5000 ms) for both
  reboot-after-response paths (`/restart` and a successful `/update`). If the client
  never closes the connection, `onDisconnect()` never fires — previously that left
  the device running indefinitely, in the OTA case on the *old* firmware. `Loop()`
  now reboots once the deadline passes and logs why.

## 1.3.1 — 2026-08-21

### Added
- **File copy** in the FS browser: a `Cp` action in the listing (JS prompt for
  the target name) backed by `/cp/<f>?to=<new>` (internal FS) and
  `/sd/cp/<f>?to=<new>` (SD). Same contract as rename: leading `/` added, an
  existing target is never overwritten — refusals come back as **409 with the
  reason**. Copies in 512-byte chunks; a failed/short write removes the partial
  target. Internal-FS copies run under the Logger FS semaphore. Works on both
  server flavors. The copy is synchronous in the web handler, so files over
  `LIONWIFI_FS_COPY_MAX` (default 64KB, `-D` to override) are refused — a
  multi-second flash write would stall the web task past its watchdog; the loop
  additionally yields every ~8KB as a safety net.

### Fixed
- **HTTP OTA (async): the client never received the "Update OK" response** — the
  handler rebooted 100 ms after `request->send()`, but on the async server send()
  only queues the response, so the device died before the bytes (and TCP FIN)
  went out; curl hung until its own timeout. The reboot now happens in the
  request's `onDisconnect` callback: `Connection: close` makes the server close
  the connection right after the response is delivered, then the ESP restarts.
- **HTTP OTA progress events reported `total = 0`** (`Update.begin()` runs with
  `UPDATE_SIZE_UNKNOWN`), so consumers computing percentages always got 0%.
  Now `total` is the POST request length — slightly larger than the binary
  (multipart overhead, <1%), good enough for a progress bar. Async server uses
  `request->contentLength()`; sync server uses `clientContentLength()` on ESP32.
  On ESP8266 (sync) the core exposes no Content-Length getter — `total` stays 0
  there. espota progress was always correct and is unchanged.

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
