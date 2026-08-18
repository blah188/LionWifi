# LionWifi

WiFi connection manager + ArduinoOTA + web filesystem browser + status page for
**ESP8266** and **ESP32** (Arduino framework).

One object pumps the whole "keep this device on the network and serviceable"
job: connect to the first reachable of up to three SSIDs, auto-reconnect, rotate
APs on timeout, (optionally) ping the router and reboot a wedged link, set the
clock over NTP, rotate logs, run ArduinoOTA, and serve a SPIFFS/LittleFS/SD file
browser with log views — plus connect/disconnect/time-set/log-clear/ping
callbacks.

> **Heads-up — this is opinionated application glue, not a tiny utility.** It
> expects the rest of the Lion\* family (LionLogger, LionArray, LionStreams, and
> LionTask/LionRtosTask) and that **you provide three globals**. Read the
> contract below before wiring it in.

## Install

PlatformIO:

```ini
lib_deps = leva/LionWifi
```

The Lion\* dependencies (LionArray, LionLogger, LionStreams, and LionTask on
ESP8266 / LionRtosTask on ESP32) are resolved automatically. The **ESP32 async**
web path additionally needs `ESPAsyncWebServer` + `AsyncTCP` — add those
yourself, or build with `-D NO_ASYNC_WEB_SERVER` to use the core `WebServer`.

## The contract — three globals you must define

LionWifi only *declares* these; your sketch defines them:

```cpp
#include <Logger.h>
#include <WifiConnector.h>

MyLogger Logger(ILogger::SerialPort | ILogger::Spiffs, ILogger::LvlDebug); // LionLogger

#ifdef ESP32
  AsyncWebServer server(80);     // or WebServer with -D NO_ASYNC_WEB_SERVER
#else
  ESP8266WebServer server(80);
#endif

WifiConnector *_connector;
```

## Usage

```cpp
void setup() {
    Serial.begin(115200);
    Logger.Setup();                 // mounts the filesystem, starts logging

    _connector = new WifiConnector("ssid", "pwd" /*, "ssid2","pwd2", "ssid3","pwd3" */);
    _connector->RegisterConnectedEvent([]{ /* ... */ });
    _connector->Setup();
}

void loop() {
#if !defined(ESP32) || defined(NO_WIFI_TASK)
    _connector->Loop();             // ESP8266: pump from loop()
#endif                              // default ESP32: runs in its own FreeRTOS task
}
```

Then browse `http://<device-ip>/`. Upload `examples/LionWifiBasic/data/index_nosd.html`
to the filesystem as `/index_nosd.html` for a home page, or go straight to
`/spiffs/ls`.

## Events / callbacks

Register these on the `WifiConnector` (before or after `Setup()`):

| Method | Fires |
| --- | --- |
| `RegisterConnectedEvent(void())` | WiFi connected |
| `RegisterDisconnectedEvent(void())` | WiFi lost |
| `RegisterTimeSetEvent(void())` | NTP time acquired |
| `RegisterClearLogEvent(void())` | old logs cleared |
| `RegisterPingEvent(void())` | periodic tick (`SetPingTime(ms)`) |
| `RegisterOtaStartEvent(void(bool sketchUpload))` | OTA begins, after LionWifi unmounts the FS |
| `RegisterOtaProgressEvent(void(unsigned progress, unsigned total))` | every OTA progress callback (raw bytes) |
| `RegisterOtaEndEvent(void(bool ok))` | OTA finished — `ok=true` success, `ok=false` error |

The OTA hooks let a consumer quiesce a heavy peripheral (e.g. stop an
ESP32-HUB75 I2S-DMA matrix that would otherwise starve the OTA transfer) for the
duration of an update, while LionWifi keeps owning the FS unmount/remount and
progress/error logging. `sketchUpload` is `true` for a sketch (`U_FLASH`) and
`false` for a filesystem image (fixed for the whole session). The end hook fires
on success **and** on error; on error there is no auto-reboot, so restore what
you quiesced. See `examples/LionWifiFull`.

## OTA (firmware updates)

Two independent mechanisms:

- **ArduinoOTA** (always on) — the usual `espota`/PlatformIO `upload_protocol = espota`
  flow. Note it's a **reverse** connection: the host invites the device, then the
  **device connects back** to the host. That fails across NAT / separate subnets /
  VPN segments where the device can't reach the host.
- **HTTP OTA** (opt-in, `-D LIONWIFI_HTTP_OTA`) — a `/update` endpoint that accepts
  a **forward** firmware POST (host → device), so it works wherever the device's
  web UI is reachable, including across NAT. Upload from a browser (open
  `http://<device>/update`, which offers a sketch and a filesystem form) or curl:

  ```bash
  # sketch
  curl -u user:pass -F "fw=@.pio/build/<env>/firmware.bin" http://<device>/update
  # filesystem image (SPIFFS/LittleFS)
  curl -u user:pass -F "filesystem=@.pio/build/<env>/spiffs.bin" http://<device>/update
  ```

  Or drive it from PlatformIO with a dedicated env that reuses your normal build
  but uploads over HTTP (so `espota` stays the default for `[env:release]`):
  ```ini
  [env:release-http]
  extends = env:release            ; same board / flags / lib_deps as the real build
  upload_protocol = custom
  upload_port = 192.168.1.50       ; device IP, exposed to the command as $UPLOAD_PORT
  upload_command = curl -u user:pass -F "fw=@$SOURCE" http://$UPLOAD_PORT/update
  ```
  Then `pio run -e release-http -t upload` builds and flashes over HTTP; for a
  filesystem image, `pio run -e release-http -t buildfs` then curl the built
  `spiffs.bin`/`littlefs.bin` with `-F "filesystem=@..."`. Notes:

  - **Bootstrap once over `espota`/USB** — `/update` only exists after a build with
    `-D LIONWIFI_HTTP_OTA` is on the device; after that, HTTP OTA reflashes itself.
  - PlatformIO warns that an IP `upload_port` "looks like `espota`" — harmless, the
    `custom` protocol still runs your `upload_command`. (Drop `upload_port` and
    hard-code the IP in the command to silence it.)

  Hand-rolled on every backend (ESP8266, ESP32-sync, ESP32-async): it drives the
  `Update` object directly, so the `RegisterOta*` hooks fire and progress is logged
  through the global `Logger` (`HTTP OTA: start … (sketch|FS)` / `… OK`) everywhere.
  The FS target is chosen by `?fs=1` (and, on the sync servers, the `filesystem`
  upload field name); it unmounts the FS and flashes the FS partition. Auth via
  `WEB_SERVER_AUTH_*` (or `NO_AUTH`); the device reboots on success.

  > **Note:** sketch OTA is tested; **filesystem-image OTA is not yet verified on
  > hardware** — treat it as experimental until confirmed.

## Filesystem — pick exactly one

Define **one** of these (a build with zero or both is a compile `#error`):

```ini
build_flags = -D USE_SPIFFS    ; or: -D LFS   (LittleFS)
```

## Build-time configuration

Override via `build_flags`. The full list lives in the header banner of
`WifiConnector.h`; the ones you most likely want:

| Define | Default | Purpose |
|---|---|---|
| `USE_SPIFFS` / `LFS` | — | Filesystem (define exactly one) |
| `WEB_SERVER_AUTH_USER` / `WEB_SERVER_AUTH_PASSWORD` | `admin`/`admin` | HTTP Basic auth — **change these** |
| `NO_AUTH` | off | Disable HTTP auth entirely |
| `LIONWIFI_HTTP_OTA` | off | Add a `/update` firmware-upload endpoint (forward POST) |
| `NTP_TZ_OFFSET_SEC` / `NTP_SERVER` | `7*3600` / `pool.ntp.org` | Time zone offset / NTP host |
| `WIFI_CONNECT_TIMEOUT` | `20000` | Per-AP attempt (ms) before rotating SSID |
| `WIFI_FATAL_CONNECT_TIMEOUT` | `180000` | Reboot after this long disconnected (ms) |
| `PING_ROUTER` | off | Router IP/host → enable ping-and-reboot watchdog |
| `QUIET_WIFI_LOGS` | off | Suppress connect/reconnect log lines |
| `NO_WIFI_TASK` (ESP32) | off | Run `Loop()` from your `loop()` instead of a task |
| `NO_ASYNC_WEB_SERVER` (ESP32) | off | Use core `WebServer` instead of ESPAsyncWebServer |
| `USE_SD_CARD` [+ `SDFAT`] | off | Also browse an SD card (SdFat with `SDFAT`) |
| `LIONWIFI_NAME_MAX` | 63 | Max listed filename length (bytes); raise for long UTF-8 names |
| `NO_MEMSTAT_IN_STATUS` | off | Drop heap/frag stats from the status page |
| `NO_WIFI_STAT_IN_STATUS` | off | Drop the WiFi RSSI/quality/channel line from the status page (shown by default) |
| `FS_BROWSER_CSS` | built-in | Replace the file-browser stylesheet (string literal) |
| `NO_FS_BROWSER_CSS` | off | Drop the file-browser stylesheet — saves ~2 KB flash |

The status page's WiFi line carries a `wifi-status` CSS class (alongside
`global-status`) so you can style it; it is emitted only while connected.

### Styling the file browser

The directory page (`/spiffs/ls`, `/sd/ls`) ships a small light/dark stylesheet
that costs **~2.0 KB of flash** (the CSS string is ~2012 bytes). Three ways to
change it, cheapest first:

1. **Re-theme** — every color is a CSS custom property (`--fs-bg`, `--fs-fg`,
   `--fs-card`, `--fs-line`, `--fs-muted`, `--fs-accent`, `--fs-danger`; each has
   a dark value). Prepend a `:root{…}` override and keep the default rules:
   ```ini
   build_flags = -D FS_BROWSER_CSS='":root{--fs-accent:#0aa}" DEFAULT_FS_BROWSER_CSS'
   ```
   (or just define `FS_BROWSER_CSS` in a force-included config header).
2. **Replace** the whole sheet with your own — `-D FS_BROWSER_CSS='"…css…"'`.
   Flash cost becomes the length of *your* string. Keep the class names
   (`.fs-wrap`, `.fs-table`, `td.name/.size/.time/.actions`, `tr.dir`, `.fs-link`,
   `.act`, `.act-del`, `.fs-summary`, `.fs-home`, `.fs-btn`) so the markup matches.
3. **Turn it off** — `-D NO_FS_BROWSER_CSS` emits no `<style>`; the page falls
   back to the browser's plain default table (still fully functional) and you
   reclaim the full ~2 KB. The full class list is documented in `FsBrowser.h`.

Copy-paste examples for all three (as `#define`s or `build_flags`) are in
`examples/LionWifiFull` — the macros must be set **before** `<WifiConnector.h>`.

## HTTP endpoints

`/` (home) · `/spiffs/ls` (list + upload) · `/tail/<f>` · `/download/<f>` ·
`/spiffs/<f>` · `/ren/<f>?to=<new>` (refuses to overwrite an existing target) ·
`/del<f>` · `/log`, `/log/tail`, `/spiffs/log[/tail]`
(LionLogger's current-day log) · `/restart` · `/format` (ESP8266 / ESP32-sync) ·
`/update` (firmware upload, with `LIONWIFI_HTTP_OTA`) ·
`/logout` · `/favicon.ico` · `/lion-tasks` (ESP8266). With `USE_SD_CARD`:
`/sd/ls`, `/sd/tail/<f>`, `/sd/download/<f>`, `/sd/ren/<f>?to=<new>`,
`/sd/del/<f>` (flat — no subfolders).

## SD cards

Enable with `-D USE_SD_CARD` (Arduino SD) or `-D USE_SD_CARD -D SDFAT` (SdFat,
add `greiman/SdFat`). **You initialize the card yourself** — LionLogger only
touches the internal filesystem (see the `LionWifiSd` example). With `SDFAT`,
define the `SdFat SD;` global the library extern-declares, build SdFat in its
default mode (`FsFile`, FAT+exFAT), and add `-D USE_UTF8_LONG_NAMES=1` so
non-ASCII filenames aren't shown as `?`.

**`SDFAT` is supported only on the ESP32 async server** (it streams via a chunked
response; `FsFile` is not an `fs::FS`). On ESP8266 or `-D NO_ASYNC_WEB_SERVER`,
use the Arduino SD library (`USE_SD_CARD` without `SDFAT`) — the build `#error`s
otherwise. SD listings are flat (root only — no subfolder navigation).

## Notes & limitations

- **Single-instance, non-copyable.** Create one via `new` and assign to
  `_connector`; it owns the WiFi/HTTP clients, the routes, and (ESP32) a task.
- On the **ESP32 async** path the directory listing is built in one `String`
  before sending (no per-row chunking yet); fine for typical filesystems.
- The `/format` route is omitted on the ESP32 async path (formatting inside an
  async callback trips the task watchdog).
- **State-changing routes use GET** (`/del…`, `/format`, `/restart`) and there is
  no CSRF token. Keep HTTP Basic auth enabled (don't set `NO_AUTH`) and/or run the
  device on a trusted network — treat the web UI as an admin console.
- `tail` is for text/logs: it serves the last 8 KB as `text/plain`, so a binary
  file's tail is truncated at the first NUL. Use **Download** for binary files.
- Listed filenames longer than `LIONWIFI_NAME_MAX` bytes are truncated (and their
  links then 404); raise the flag for long UTF-8 names.

## License

0BSD — see [LICENSE](LICENSE).
