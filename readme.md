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
| `NTP_TZ_OFFSET_SEC` / `NTP_SERVER` | `7*3600` / `pool.ntp.org` | Time zone offset / NTP host |
| `WIFI_CONNECT_TIMEOUT` | `20000` | Per-AP attempt (ms) before rotating SSID |
| `WIFI_FATAL_CONNECT_TIMEOUT` | `180000` | Reboot after this long disconnected (ms) |
| `PING_ROUTER` | off | Router IP/host → enable ping-and-reboot watchdog |
| `QUIET_WIFI_LOGS` | off | Suppress connect/reconnect log lines |
| `NO_WIFI_TASK` (ESP32) | off | Run `Loop()` from your `loop()` instead of a task |
| `NO_ASYNC_WEB_SERVER` (ESP32) | off | Use core `WebServer` instead of ESPAsyncWebServer |
| `USE_SD_CARD` [+ `SDFAT`] | off | Also browse an SD card |

## HTTP endpoints

`/` (home) · `/spiffs/ls` (list + upload) · `/tail/<f>` · `/download/<f>` ·
`/spiffs/<f>` · `/del<f>` · `/log`, `/log/tail`, `/spiffs/log[/tail]`
(LionLogger's current-day log) · `/restart` · `/format` (ESP8266 / ESP32-sync) ·
`/logout` · `/favicon.ico` · `/lion-tasks` (ESP8266). With `USE_SD_CARD`:
`/sd/ls`, `/sd/tail/<f>`, `/sd/download/<f>`, `/sd/del/<f>`, `/sd/mkdir`.

## Notes & limitations

- **Single-instance, non-copyable.** Create one via `new` and assign to
  `_connector`; it owns the WiFi/HTTP clients, the routes, and (ESP32) a task.
- On the **ESP32 async** path the directory listing is built in one `String`
  before sending (no per-row chunking yet); fine for typical filesystems.
- The `/format` route is omitted on the ESP32 async path (formatting inside an
  async callback trips the task watchdog).

## License

0BSD — see [LICENSE](LICENSE).
