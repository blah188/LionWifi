#pragma once

// =============================================================================
// LionWifi — WiFi connection manager + ArduinoOTA + web file browser (FsBrowser)
// + status/uptime page, for ESP8266 and ESP32 (Arduino framework).
//
// Pumps the WiFi state machine: connects to the first reachable of up to three
// SSIDs, auto-reconnects, rotates APs on timeout, (optionally) pings the router
// and reboots on a wedged link, sets time over NTP, rotates logs, and serves a
// SPIFFS/LittleFS/SD file browser with OTA. Fires user callbacks on connect /
// disconnect / time-set / log-clear / ping.
//
// ---- Required globals the CONSUMER must define (this header only declares them):
//   * MyLogger Logger(...);          // from LionLogger; .Setup() called by you
//   * <WebServerType> server(80);    // the web server, extern'd below:
//        ESP8266:                     ESP8266WebServer server(80);
//        ESP32 (default, async):      AsyncWebServer   server(80);  // ESPAsyncWebServer
//        ESP32 + NO_ASYNC_WEB_SERVER: WebServer        server(80);
//   * WifiConnector *_connector;     // extern'd at the bottom of this header
//
// ---- Dependencies (PlatformIO): LionArray, LionLogger, LionStreams; plus
//      LionTask on ESP8266 / LionRtosTask on ESP32. The ESP32 async web path
//      additionally needs ESPAsyncWebServer + AsyncTCP.
//
// ---- Compile-time configuration (define via build_flags). Required:
//   USE_SPIFFS | LFS        Filesystem — define EXACTLY ONE (LFS = LittleFS).
//
//   Web auth (defaults admin/admin — OVERRIDE for real deployments):
//   WEB_SERVER_AUTH_USER     Basic-auth user      (default "admin")
//   WEB_SERVER_AUTH_PASSWORD Basic-auth password  (default "admin")
//   NO_AUTH                  Disable HTTP auth entirely (open access).
//
//   Time / NTP:
//   NTP_TZ_OFFSET_SEC        UTC offset in seconds (default 7*3600, no DST)
//   NTP_SERVER               NTP host             (default "pool.ntp.org")
//   MIN_VALID_EPOCH          "time is set" threshold (default 2019-01-01)
//
//   WiFi state machine (ms):
//   WIFI_CONNECT_TIMEOUT     Per-AP attempt before rotating SSID (default 20000)
//   WIFI_FATAL_CONNECT_TIMEOUT  Reboot after this long disconnected (default 180000)
//   WIFI_CLIENT_TIMEOUT      Shared WiFi/HTTP client timeout      (default 3000)
//   FORBID_WIFI_MONITOR      Disable the fatal-timeout auto-reboot.
//   QUIET_WIFI_LOGS          Suppress the connect/reconnect/AP-change log lines.
//
//   Router ping watchdog (opt-in):
//   PING_ROUTER              Router IP/host string — enables periodic TCP ping;
//                            reboots after PING_ROUTER_MAX_FAILURES (default 4)
//                            misses, every PING_ROUTER_INTERVAL ms (default 30000).
//
//   Logging / housekeeping (most gated by LionLogger features):
//   LOG_CLEAR_EVERY_HOURS    Log-maintenance cadence (default 4)
//   LOG_CLEAR_DAYS           Keep dated logs this many days (default 14)
//   MAX_LOG_BYTES, LOG_CLEAR_FREE_SPACE, FS_LOW_SPACE_THRESHOLD/TARGET,
//   FS_SPACE_CHECK_INTERVAL_MS                Free-space / size watchdog tuning.
//   HEAP_CHECK_INTERVAL_MS   Min gap between heap-stats samples (ESP8266, default
//                            1000; getHeapStats walks the heap with IRQs off).
//   NO_MEMSTAT_IN_STATUS     Drop heap/frag stats from the status page.
//   NO_WIFI_STAT_IN_STATUS   Drop the WiFi RSSI/quality/channel line from the
//                            status page (shown by default).
//   LOG_FAVICON              Also log favicon.ico requests.
//
//   HTTP OTA (opt-in):
//   LIONWIFI_HTTP_OTA        Add a /update endpoint (browser/curl firmware upload,
//                            a forward POST). Works across NAT/subnets where
//                            ArduinoOTA's reverse connection can't reach back.
//                            Hand-rolled on every backend: the RegisterOta* hooks
//                            fire and progress is logged via Logger. The GET page
//                            flashes either the sketch or a filesystem image
//                            (?fs=1). Basic auth via WEB_SERVER_AUTH_* / NO_AUTH.
//
//   ESP32-specific:
//   NO_WIFI_TASK             Run Loop() from your loop() instead of a FreeRTOS task.
//   NO_ASYNC_WEB_SERVER      Use the sync WebServer instead of ESPAsyncWebServer.
//   CORE_WIFI                FreeRTOS core for the WiFi task (default 1).
//   DISABLE_11N              Force 802.11b/g (some APs misbehave with 11n).
//   MAX_WIFI_POWER           Set max TX power for weak links.
//
//   FsBrowser extras:
//   USE_SD_CARD [+ SDFAT]    Also browse an SD card (SdFat when SDFAT is set).
//   USE_FILE_TIME            Show created/modified timestamps in listings.
//   LIONWIFI_NAME_MAX        Max listed filename length (default 63); raise for
//                            longer LittleFS/SD names (costs N+1 bytes/entry).
//
// ---- HTTP endpoints registered (see FsBrowser::AddRoutes + WifiConnector::Setup):
//   GET /                         Home page (index.html w/ SD, else index_nosd.html)
//   GET /spiffs/ls   POST upload  Filesystem listing + multipart file upload
//   GET /tail/<f> /download/<f> /spiffs/<f>   View last 8 KB / download / raw file
//   GET /del<f>                   Delete a file (then redirect to listing)
//   GET /log /log/tail /spiffs/log[/tail]     LionLogger's current-day log file
//                                 (full / last 8 KB). Added by WifiConnector::Setup,
//                                 served via FsBrowser from Logger.GetLogFileName().
//   GET /restart                  Reboot the device
//   GET/POST /update              Sketch or FS-image upload + reboot (with -D LIONWIFI_HTTP_OTA)
//   GET /format                   Format the FS (ESP8266 / ESP32-sync only)
//   GET /logout                   Clear HTTP Basic auth (401)
//   GET /favicon.ico              Served from the filesystem
//   GET /lion-tasks               LionTask debug dump (ESP8266)
//   GET /sd/ls /sd/tail/<f> /sd/download/<f> /sd/del/<f>            (USE_SD_CARD)
//
// Single-instance, non-copyable: owns OS handles (WiFiClient/HTTPClient), the
// web routes and (on ESP32) a FreeRTOS task. Create one via `new` and assign it
// to the global `_connector`.
// =============================================================================

#include <functional>

#include <Logger.h>
#include <Array.h>

#ifdef ESP32
#include <esp32/rom/rtc.h>
#include <RtosTask.h>
#include <esp_wifi.h>
#include <HTTPClient.h>

#ifdef NO_ASYNC_WEB_SERVER
#include <WebServer.h>
#else
#include <ESPAsyncWebServer.h>
#endif

// FreeRTOS core the WiFi task is pinned to (ESP32 only).
#ifndef CORE_WIFI
#define CORE_WIFI 1
#endif

#else // No ESP32
#include <LionTask.h>
extern "C"
{
#include "user_interface.h"
}
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#endif

#include <FsBrowser.h> // defines LIONWIFI_FS (the selected filesystem)
#include <MyOTA.h>      // uses LIONWIFI_FS, so it must come after FsBrowser.h

#ifdef LIONWIFI_HTTP_OTA
// Browser/curl firmware upload at /update (a FORWARD POST, unlike ArduinoOTA's
// reverse connection) — works across NAT/subnets where espota can't reach back.
// Hand-rolled on every backend (drives the Update object directly) so the OTA
// hooks fire and progress is logged through the global Logger uniformly.
#ifdef ESP32
#include <Update.h>
#else
#include <flash_hal.h> // FS_start / FS_end bound the FS partition (to size an FS-image OTA)
// (the Update object itself comes from the ESP8266 core via ESP8266WiFi.h above)
#endif
// The /update GET page: one form flashes the sketch, the other a filesystem image
// (?fs=1 + field name "filesystem" select the FS target on every backend).
#define LIONWIFI_OTA_FORM_HTML                                                 \
    "<h3>Firmware</h3>"                                                        \
    "<form method='POST' action='/update' enctype='multipart/form-data'>"     \
    "<input type='file' name='fw'><input type='submit' value='Flash'></form>" \
    "<h3>Filesystem</h3>"                                                      \
    "<form method='POST' action='/update?fs=1' enctype='multipart/form-data'>" \
    "<input type='file' name='filesystem'><input type='submit' value='Flash'></form>"
#endif

#ifndef LOG_CLEAR_EVERY_HOURS
#define LOG_CLEAR_EVERY_HOURS 4
#endif

#ifndef LOG_CLEAR_DAYS
#define LOG_CLEAR_DAYS 14
#endif

#ifndef WIFI_CONNECT_TIMEOUT
#define WIFI_CONNECT_TIMEOUT 20000ul
#endif

#ifndef WIFI_FATAL_CONNECT_TIMEOUT
#define WIFI_FATAL_CONNECT_TIMEOUT 180000ul
#endif

#ifndef PING_ROUTER_INTERVAL
#define PING_ROUTER_INTERVAL 30000ul
#endif

#ifndef PING_ROUTER_MAX_FAILURES
#define PING_ROUTER_MAX_FAILURES 4
#endif

// Default web-auth credentials. These are intentionally generic placeholders —
// OVERRIDE THEM with -D WEB_SERVER_AUTH_USER=\"...\" / -D WEB_SERVER_AUTH_PASSWORD=\"...\"
// for any real deployment. (Auth can be disabled entirely with -D NO_AUTH.)
#ifndef WEB_SERVER_AUTH_USER
#define WEB_SERVER_AUTH_USER "admin"
#endif

#ifndef WEB_SERVER_AUTH_PASSWORD
#define WEB_SERVER_AUTH_PASSWORD "admin"
#endif

// NTP time sync. Override the TZ offset for your region (default +7h, no DST)
// and/or the NTP server via build flags.
#ifndef NTP_TZ_OFFSET_SEC
#define NTP_TZ_OFFSET_SEC (7 * 3600)
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

// Smallest epoch treated as "real NTP time has arrived" (2019-01-01 UTC).
// Below this, the clock is still the power-on default and time is not set.
#ifndef MIN_VALID_EPOCH
#define MIN_VALID_EPOCH 1546300800
#endif

// --- RTC-persisted wall clock (ESP32) -----------------------------------------
// ESP-IDF does NOT carry SNTP-corrected time across a reset — even a clean
// esp_restart reverts to the drifting raw RTC — so boot-time logs and LionLogger's
// day-file name render at the wrong time until SNTP re-syncs. We snapshot valid time
// into RTC_NOINIT memory (survives a SW reset; lost on power loss → magic mismatch)
// and restore it at boot via the static RestoreClockFromRtc().
// Enabled by default on ESP32; turn OFF with -D LIONWIFI_NO_RTC_CLOCK.
#if defined(ESP32) && !defined(LIONWIFI_NO_RTC_CLOCK)
#define LIONWIFI_RTC_CLOCK
#endif
#ifdef LIONWIFI_RTC_CLOCK
#include <sys/time.h>
extern uint32_t _lwRtcSavedEpoch; // defined once in WifiConnector.cpp
extern uint32_t _lwRtcSavedMagic;
#define LIONWIFI_RTC_TIME_MAGIC 0x4C57524Dul // 'LWRM'
#endif

#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
extern WebServer server;
#else
extern AsyncWebServer server;
#endif
#else
extern ESP8266WebServer server;
#endif

#ifndef WIFI_CLIENT_TIMEOUT
#define WIFI_CLIENT_TIMEOUT 3000
#endif

// On ESP32 (unless NO_WIFI_TASK) the connector runs its own FreeRTOS task, so it
// derives from RtosTask; everywhere else it is a plain pumped-from-loop() object.
#if defined(ESP32) && !defined(NO_WIFI_TASK)
class WifiConnector : RtosTask
#else
class WifiConnector
#endif
{
private:
    bool _connected = false, _timeSet = false, _on = true;
    uint32_t _lastPingTime = 0, _lastLogClearTime = 0, _lastConnectStartTime = 0, _pingEveryMs = 3 * 60 * 1000ul;
    uint32_t _lastConnectedTime = 0;
#ifdef FS_LOW_SPACE_THRESHOLD
    uint32_t _lastFreeSpaceCheck = 0;
#endif
#ifndef ESP32
    uint32_t _lastHeapCheckTime = 0;
#endif
#ifdef PING_ROUTER
    int _routerPingErrorsInRow = 0;
    int _routerPingSuccessesInRow = 0;
    uint32_t _lastRouterPingTime = 0;
#endif
    MyOta *_myOta = nullptr;
    Array<String *> _ssids, _passwords;
    int _curApIdx = 0;
    uint32_t _minFreeMemory = 1000000; // seed high so the first heap sample always wins
    FsBrowser *_fsBrowser = nullptr;
    std::function<void()> _conEvent, _disconEvent, _timeSetEvent, _clearLogEvent, _pingEvent;
    // OTA hooks — forwarded to _myOta (which invokes them from ArduinoOTA's
    // callbacks). start's bool = sketch upload (vs FS image); end's bool = ok.
    std::function<void(bool)> _otaStartEvent, _otaEndEvent;
    std::function<void(unsigned int, unsigned int)> _otaProgressEvent;
    WiFiClient *_client = nullptr;
    HTTPClient *_httpClient = nullptr;
    bool _otaStarted = false;
#ifdef LIONWIFI_HTTP_OTA
    bool _httpOtaAuthOk = false; // latched when the upload starts; response gated on it
#endif

public:
    void SetPingTime(uint32_t pt) { _pingEveryMs = pt; }
    uint32_t GetPingTime() { return _pingEveryMs; }
    uint32_t GetMinFreeMemory() { return _minFreeMemory; }
    bool IsOn() { return _on; }
    bool Connected() { return _connected; }
    bool TimeSet() { return _timeSet; }
    FsBrowser *Browser() { return _fsBrowser; }
#ifdef PING_ROUTER
    int GetRouterPingErrorsInRow() { return _routerPingErrorsInRow; }
    int GetRouterPingSuccessesInRow() { return _routerPingSuccessesInRow; }
#endif
    void TurnOn(bool on = true)
    {
        if (_on == on)
            return;
        if ((_on = on))
            Connect();
        else
            Disconnect();
    };

    WifiConnector(const char *ssid, const char *pwd, const char *ssid1 = NULL, const char *pwd1 = NULL, const char *ssid2 = NULL, const char *pwd2 = NULL)
    {
        _on = true;
        // Arm the fatal-reconnect window from boot, not from epoch 0. Otherwise
        // _lastConnectedTime stays 0 and (millis() - 0) crosses
        // WIFI_FATAL_CONNECT_TIMEOUT after ~3 min of uptime, rebooting a device
        // that simply hasn't connected yet (slow/out-of-range AP at power-on).
        _lastConnectedTime = millis();
        _ssids += new String(ssid);
        _passwords += new String(pwd);
        if (ssid1 && pwd1)
        {
            _ssids += new String(ssid1);
            _passwords += new String(pwd1);
        }
        if (ssid2 && pwd2)
        {
            _ssids += new String(ssid2);
            _passwords += new String(pwd2);
        }
    }

    // Single-instance manager: owns raw new'd handles (WiFiClient/HTTPClient/
    // MyOta/FsBrowser) and registered web routes with no destructor, so a copy
    // would alias and double-manage them. Non-copyable by design.
    WifiConnector(const WifiConnector &) = delete;
    WifiConnector &operator=(const WifiConnector &) = delete;

    // Call FIRST in setup(), before any timestamped logging. On ESP32 it sets the TZ
    // offset early (so boot logs before Setup() are in local time) and, when
    // LIONWIFI_RTC_CLOCK is enabled, restores the last-saved wall clock from RTC
    // memory. Static — usable before the WifiConnector object exists.
    static void RestoreClockFromRtc()
    {
#ifdef ESP32
        configTime(NTP_TZ_OFFSET_SEC, 0, NTP_SERVER); // TZ before first log; SNTP also (re)inits on connect
#ifdef LIONWIFI_RTC_CLOCK
        if (_lwRtcSavedMagic == LIONWIFI_RTC_TIME_MAGIC && _lwRtcSavedEpoch > (uint32_t)MIN_VALID_EPOCH)
        {
            struct timeval tv = { (time_t)_lwRtcSavedEpoch, 0 };
            settimeofday(&tv, NULL);
        }
#endif
#endif
    }

    // Snapshot the current valid wall clock into RTC memory (cheap SRAM write, no
    // flash). No-op unless LIONWIFI_RTC_CLOCK and time is set. Called each Loop().
    static void SaveClockToRtc()
    {
#ifdef LIONWIFI_RTC_CLOCK
        time_t now = time(NULL);
        if ((uint32_t)now > (uint32_t)MIN_VALID_EPOCH)
        {
            _lwRtcSavedEpoch = (uint32_t)now;
            _lwRtcSavedMagic = LIONWIFI_RTC_TIME_MAGIC;
        }
#endif
    }

    void RegisterConnectedEvent(std::function<void()> evt)
    {
        _conEvent = evt;
    }
    void RegisterDisconnectedEvent(std::function<void()> evt)
    {
        _disconEvent = evt;
    }
    void RegisterTimeSetEvent(std::function<void()> evt)
    {
        _timeSetEvent = evt;
    }
    void RegisterClearLogEvent(std::function<void()> evt)
    {
        _clearLogEvent = evt;
    }
    void RegisterPingEvent(std::function<void()> evt)
    {
        _pingEvent = evt;
    }
    // OTA hooks. Safe to call before or after Setup(): stored here and pushed to
    // _myOta on creation.
    //   start(bool sketchUpload) — after LionWifi unmounts the FS (sketchUpload:
    //                              true = sketch, false = filesystem image).
    //   end(bool ok)             — on completion (ok=true) and on error (ok=false).
    //   progress(prog, total)    — every ArduinoOTA progress callback, raw bytes.
    void RegisterOtaStartEvent(std::function<void(bool)> evt)
    {
        _otaStartEvent = evt;
        if (_myOta)
            _myOta->SetOtaStartCallback(evt);
    }
    void RegisterOtaEndEvent(std::function<void(bool)> evt)
    {
        _otaEndEvent = evt;
        if (_myOta)
            _myOta->SetOtaEndCallback(evt);
    }
    void RegisterOtaProgressEvent(std::function<void(unsigned int, unsigned int)> evt)
    {
        _otaProgressEvent = evt;
        if (_myOta)
            _myOta->SetOtaProgressCallback(evt);
    }

    WiFiClient *SharedWifiClient()
    {
        return _client;
    }

    // Derive the boot epoch from the monotonic clock (now - uptime) instead of a
    // value latched at first sync. Self-heals if the clock is stepped afterwards
    // (e.g. SNTP correcting an ESP32 RTC that jumped forward during an OTA reflash),
    // so uptime never freezes or goes negative. (Wraps once per millis() rollover,
    // ~49.7 d — a cosmetic blip on the status page.)
    time_t GetStartupTime() { return _timeSet ? time(NULL) - (time_t)(millis() / 1000UL) : 0; }

    void StopSharedWifiClient()
    {
#ifdef ESP32
        _client->stop();
#else
        _client->abort();
#endif
    }

    HTTPClient *SharedHttpClient(const String &url)
    {
        return SharedHttpClient(url.c_str());
    }

    HTTPClient *SharedHttpClient(const char *url = NULL)
    {
        if (url)
            return _httpClient->begin(*_client, url) ? _httpClient : NULL;
        return _httpClient;
    }

    static void FormatTimespan(time_t ts, Print &out)
    {
        if (ts < 120)
        {
            out.print(ts);
            out.print('s');
        }
        else if (ts < 7200)
        {
            out.print(ts / 60);
            out.print('m');
        }
        else if (ts < 48 * 3600ul)
        {
            out.print(ts / 3600);
            out.print('h');
        }
        else if (ts < 6 * 30 * 24 * 3600ul)
        {
            out.print(ts / (24 * 3600ul));
            out.print('d');
        }
        else if (ts < 24 * 30 * 24 * 3600ul)
        {
            out.print(ts / (30 * 24 * 3600ul));
            out.print(F(" months"));
        }
        else
        {
            out.print(ts / (365 * 24 * 3600ul));
            out.print(F(" years"));
        }
    }

    static void FormatTime(time_t ts, Print &out)
    {
        struct tm *timeinfo = localtime(&ts);
        if (!timeinfo) // localtime() can return NULL for an out-of-range ts
            return;
        out.printf_P(PSTR("%02d:%02d:%02d"),
                     timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    }

    static void FormatDateTime(time_t ts, Print &out)
    {
        struct tm *timeinfo = localtime(&ts);
        if (!timeinfo) // localtime() can return NULL for an out-of-range ts
            return;
        out.printf_P(PSTR("%02d/%02d/%04d %02d:%02d:%02d"),
                     timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    }

    static const __FlashStringHelper *SdkVersion()
    {
#ifdef PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_190703
        return F("2.2.x Jul 3 (default)");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK221)
        return F("2.21");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK3)
        return F("pre-3.0");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_191122)
        return F("v2.2.x Nov 22");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_191105)
        return F("v2.2.x Nov 5");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_191024)
        return F("v2.2.x Oct 24");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_190313)
        return F("v2.2.x Mar 13");
#elif defined(PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK305)
        return F("3.05 (experimental)");
#elif defined(ESP32)
        return F("ESP32");
#else
        return F("Unknown");
#endif
    }

    static const __FlashStringHelper *ResetReason(uint32_t reason)
    {
        switch (reason)
        {
#ifdef ESP32
        case 1:
            return F("POWERON_RESET"); /**<1, Vbat power on reset*/
        case 3:
            return F("SW_RESET"); /**<3, Software reset digital core*/
        case 4:
            return F("OWDT_RESET"); /**<4, Legacy watch dog reset digital core*/
        case 5:
            return F("DEEPSLEEP_RESET"); /**<5, Deep Sleep reset digital core*/
        case 6:
            return F("SDIO_RESET"); /**<6, Reset by SLC module, reset digital core*/
        case 7:
            return F("TG0WDT_SYS_RESET"); /**<7, Timer Group0 Watch dog reset digital core*/
        case 8:
            return F("TG1WDT_SYS_RESET"); /**<8, Timer Group1 Watch dog reset digital core*/
        case 9:
            return F("RTCWDT_SYS_RESET"); /**<9, RTC Watch dog Reset digital core*/
        case 10:
            return F("INTRUSION_RESET"); /**<10, Instrusion tested to reset CPU*/
        case 11:
            return F("TGWDT_CPU_RESET"); /**<11, Time Group reset CPU*/
        case 12:
            return F("SW_CPU_RESET"); /**<12, Software reset CPU*/
        case 13:
            return F("RTCWDT_CPU_RESET"); /**<13, RTC Watch dog Reset CPU*/
        case 14:
            return F("EXT_CPU_RESET"); /**<14, for APP CPU, reseted by PRO CPU*/
        case 15:
            return F("RTCWDT_BROWN_OUT_RESET"); /**<15, Reset when the vdd voltage is not stable*/
        case 16:
            return F("RTCWDT_RTC_RESET"); /**<16, RTC Watch dog reset digital core and rtc module*/
#else
        case REASON_DEFAULT_RST:
            return F("Normal");
        case REASON_WDT_RST:
            return F("Hard Wdt");
        case REASON_EXCEPTION_RST:
            return F("Exception");
        case REASON_SOFT_WDT_RST:
            return F("Soft Wdt");
        case REASON_SOFT_RESTART:
            return F("Soft restart");
        case REASON_DEEP_SLEEP_AWAKE:
            return F("Deep awake");
        case REASON_EXT_SYS_RST:
            return F("Ext sys");
#endif
        }
        return F("Unknown");
    }

    void StatusHtml(Print &out)
    {
#ifndef NO_MEMSTAT_IN_STATUS
        // Collect before rendering to minimize memory errors
#ifndef ESP32
        uint32_t freeHeap, maxAlloc;
        uint8_t frag;
        ESP.getHeapStats(&freeHeap, &maxAlloc, &frag);
        uint32_t freeStack = ESP.getFreeContStack();
#else
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        // On ESP32 the heap is segmented (WiFi/BT/RTOS allocations split DRAM),
        // so the largest contiguous block is naturally a fraction of free heap
        // even at boot — reporting it as "fragmentation %" is misleading. Show
        // the largest block and its share of free heap instead. Clamp the share
        // to <=100 (maxAlloc can momentarily exceed the free-heap accounting).
        uint32_t largestPct = freeHeap > 0 ? (uint32_t)maxAlloc * 100 / freeHeap : 100;
        if (largestPct > 100)
            largestPct = 100;
#endif
#endif

        time_t now = time(NULL);
        time_t startup = GetStartupTime();
        out.print(F("<div class='global-status'><span>Now <b>"));
        FormatTime(now, out);
        out.print(F("</b></span>&nbsp;<span>Restarted <b>"));
        FormatDateTime(startup, out);
        out.print(F("</b> (uptime <b>"));
        FormatTimespan(now - startup, out);
        out.printf_P(PSTR("</b>)</span>&nbsp;<span>Built <b>%S %S</b></span>&nbsp;<span>SDK: <b>%S(%s)</b></span></div>"),
                     F(__DATE__), F(__TIME__), SdkVersion(), LIONWIFI_FS_NAME);

#ifndef NO_MEMSTAT_IN_STATUS        
        // Memory stats
        out.print(F("<div class='global-status memory-status'>"));
#ifndef ESP32
        out.printf_P(PSTR("Heap: <b>%u</b>B (loop min <b>%u</b>) | Frag: <b>%d%%</b> | Stack: <b>%u</b>B"),
                      freeHeap, _minFreeMemory, frag, freeStack);
#else
        out.printf_P(PSTR("Heap: <b>%u</b>B (min <b>%u</b>) | Largest: <b>%s</b> (%u%%)"),
                      freeHeap, _minFreeMemory, FsBrowser::FileSize(maxAlloc).c_str(), largestPct);
#endif
        out.print(F("</div>"));
#endif

#ifndef NO_WIFI_STAT_IN_STATUS
        // WiFi radio diagnostics: a weak RSSI means retransmits → slow/failing
        // OTA. Color-coded verdict so a bad link is obvious at a glance.
        if (_connected)
        {
            int rssi = (int)WiFi.RSSI();
            const __FlashStringHelper *rq;
            const char *rcolor;
            if (rssi >= -67)      { rq = F("good");            rcolor = "green"; }
            else if (rssi >= -75) { rq = F("ok");              rcolor = "green"; }
            else if (rssi >= -82) { rq = F("weak, OTA slow");  rcolor = "orange"; }
            else                  { rq = F("POOR, OTA fails"); rcolor = "red"; }
            out.printf_P(PSTR("<div class='global-status wifi-status'><span>WiFi <b>%s</b></span>&nbsp;<span>RSSI <b>%d</b> dBm &mdash; <b style='color:%s'>%S</b></span>&nbsp;<span>ch <b>%d</b></span></div>"),
                         WiFi.SSID().c_str(), rssi, rcolor, rq, WiFi.channel());
        }
#endif
    }

#ifdef LIONWIFI_HTTP_OTA
    // Registers /update: GET shows an upload form, POST flashes the firmware and
    // reboots. FORWARD upload (curl/browser → device), so it works across NAT /
    // subnets where ArduinoOTA's reverse connection can't reach back. HTTP Basic
    // auth (WEB_SERVER_AUTH_*), or open with -D NO_AUTH. Fires the OTA hooks on
    // the async path. Call from Setup() after the browser routes are registered.
    void SetupHttpOta()
    {
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            request->send(200, F("text/html"), F(LIONWIFI_OTA_FORM_HTML)); });
        server.on(
            "/update", HTTP_POST,
            [this](AsyncWebServerRequest *request)
            {
                if (!_httpOtaAuthOk) { request->requestAuthentication(); return; }
                bool ok = !Update.hasError();
                if (_otaEndEvent) _otaEndEvent(ok);
                AsyncWebServerResponse *resp = request->beginResponse(200, __text_plain__F, ok ? F("Update OK - rebooting") : F("Update FAILED"));
                resp->addHeader(F("Connection"), F("close"));
                request->send(resp);
                if (ok) { delay(100); ESP.restart(); }
            },
            [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
            {
                if (!index) // first chunk: authenticate, then open the update
                {
#ifdef NO_AUTH
                    _httpOtaAuthOk = true;
#else
                    _httpOtaAuthOk = request->authenticate(WEB_SERVER_AUTH_USER, WEB_SERVER_AUTH_PASSWORD);
#endif
                    if (!_httpOtaAuthOk) return; // reject unauthenticated uploads outright
                    bool fs = request->hasParam("fs"); // ?fs=1 → flash a filesystem image
                    Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP OTA: start %s (%s)"), filename.c_str(), fs ? "FS" : "sketch");
                    if (_otaStartEvent) _otaStartEvent(!fs); // sketchUpload = !fs
                    if (fs)
                    {
                        LIONWIFI_FS.end(); // unmount before writing the FS partition
                        Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS);
                    }
                    else
                        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
                }
                if (!_httpOtaAuthOk) return;
                if (Update.write(data, len) != len)
                    Logger.Log_P(ILogger::LvlError, PSTR("HTTP OTA: short write"));
                if (_otaProgressEvent) _otaProgressEvent((unsigned int)(index + len), 0);
                if (final)
                {
                    if (Update.end(true))
                        Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP OTA: %u bytes, OK"), (unsigned int)(index + len));
                    else
                        Logger.Log_P(ILogger::LvlError, PSTR("HTTP OTA: end failed"));
                }
            });
#else // sync server (ESP8266 / ESP32 core WebServer) — hand-rolled for uniform logs + hooks
        server.on(F("/update"), HTTP_GET, [this]()
                  {
            if (!_fsBrowser->DoAuth()) return;
            server.send(200, F("text/html"), F(LIONWIFI_OTA_FORM_HTML)); });
        server.on(
            F("/update"), HTTP_POST,
            [this]() // onRequest: runs after the upload — send result + reboot
            {
                if (!_httpOtaAuthOk) { server.requestAuthentication(); return; }
                bool ok = !Update.hasError();
                if (_otaEndEvent) _otaEndEvent(ok);
                server.send(200, __text_plain__F, ok ? F("Update OK - rebooting") : F("Update FAILED"));
                delay(100);
                if (ok) ESP.restart();
            },
            [this]() // upload handler — drives Update chunk by chunk
            {
                HTTPUpload &up = server.upload();
                if (up.status == UPLOAD_FILE_START)
                {
#ifdef NO_AUTH
                    _httpOtaAuthOk = true;
#else
                    _httpOtaAuthOk = server.authenticate(WEB_SERVER_AUTH_USER, WEB_SERVER_AUTH_PASSWORD);
#endif
                    if (!_httpOtaAuthOk) return; // reject unauthenticated uploads outright
                    bool fs = server.hasArg("fs") || up.name == "filesystem"; // FS-image target
                    Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP OTA: start %s (%s)"), up.filename.c_str(), fs ? "FS" : "sketch");
                    if (_otaStartEvent) _otaStartEvent(!fs); // sketchUpload = !fs
                    if (fs)
                    {
                        LIONWIFI_FS.end(); // unmount before writing the FS partition
#ifdef ESP32
                        Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS);
#else
                        Update.begin((size_t)FS_end - (size_t)FS_start, U_FS);
#endif
                    }
                    else
                    {
#ifdef ESP32
                        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
#else
                        Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000, U_FLASH);
#endif
                    }
                }
                else if (up.status == UPLOAD_FILE_WRITE && _httpOtaAuthOk)
                {
                    if (Update.write(up.buf, up.currentSize) != up.currentSize)
                        Logger.Log_P(ILogger::LvlError, PSTR("HTTP OTA: short write"));
                    if (_otaProgressEvent) _otaProgressEvent((unsigned int)up.totalSize, 0);
                }
                else if (up.status == UPLOAD_FILE_END && _httpOtaAuthOk)
                {
                    if (Update.end(true))
                        Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP OTA: %u bytes, OK"), (unsigned int)up.totalSize);
                    else
                        Logger.Log_P(ILogger::LvlError, PSTR("HTTP OTA: end failed"));
                }
            });
#endif
        Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP OTA ready at /update"));
    }
#endif

    void Setup()
    {
        // Apply the TZ offset immediately at boot — not only in configTime() on
        // connect (below). On ESP32 the RTC keeps running across a SW reset, so
        // until WiFi connects localtime() would render boot-time logs (and
        // LionLogger's localtime-derived day-file name) in UTC, scattering entries
        // into the wrong day file around local midnight. Re-called with the server
        // on connect to (re)start SNTP; here it just sets the TZ env.
        configTime(NTP_TZ_OFFSET_SEC, 0, NTP_SERVER);

        _myOta = new MyOta();
        // Push any OTA hooks registered before Setup().
        if (_otaStartEvent)
            _myOta->SetOtaStartCallback(_otaStartEvent);
        if (_otaEndEvent)
            _myOta->SetOtaEndCallback(_otaEndEvent);
        if (_otaProgressEvent)
            _myOta->SetOtaProgressCallback(_otaProgressEvent);

#if defined(ESP32) && !defined(NO_WIFI_TASK)
        RtosTask::Setup("WiFi", CORE_WIFI);
#else
#ifndef ESP32
            server.begin();
            Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP server started"));
            ESP.wdtEnable(5000);
#endif
#endif

        _client = new WiFiClient();
        _client->setTimeout(WIFI_CLIENT_TIMEOUT);
        _httpClient = new HTTPClient();
        _httpClient->setTimeout(WIFI_CLIENT_TIMEOUT);

        // LionLogger integration: expose the current-day log file over HTTP
        // (/log[/tail], /spiffs/log[/tail]) plus a /restart route. The generic
        // file browser routes themselves are added by _fsBrowser->AddRoutes() below.
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        server.on("/log/tail", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), request, false, true); });
        server.on("/spiffs/log/tail", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), request, false, true); });
        server.on("/log", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), request, false, false); });
        server.on("/spiffs/log", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), request, false, false); });

        server.on("/restart", HTTP_GET, [this](AsyncWebServerRequest *request)
                  {
            if (!_fsBrowser->DoAuth(request)) return;
            request->send(200, __text_plain__F, F("Restarting..."));
            delay(100);
            ESP.restart(); });
#else
#if !defined(ESP32)
            server.on(F("/lion-tasks"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            auto tasks = LionTask::GetDebugDump();
            server.send(200, __text_plain__F, tasks); });
#endif
            server.on(F("/log"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), false, false); });
            server.on(F("/spiffs/log"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), false, false); });
            server.on(F("/log/tail"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), false, true); });
            server.on(F("/spiffs/log/tail"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            _fsBrowser->SendFileResponse(Logger.GetLogFileName(true), false, true); });

            server.on(F("/restart"), [this]()
                      {
            if (!_fsBrowser->DoAuth()) return;
            server.send(200, __text_plain__F, F("Restarting..."));
            delay(100);
            ESP.restart(); });
#endif

        _fsBrowser = new FsBrowser(server, WEB_SERVER_AUTH_USER, WEB_SERVER_AUTH_PASSWORD);
        _fsBrowser->AddRoutes();

#ifdef LIONWIFI_HTTP_OTA
        SetupHttpOta(); // browser/curl firmware upload at /update (forward POST)
#endif

#if !defined(ESP32) || defined(NO_WIFI_TASK)
        if (_on)
            Connect();
#endif
    }

#if defined(ESP32) && !defined(NO_WIFI_TASK)
    virtual void TaskBody()
    {
        if (_on)
            Connect();
        while (true)
        {
            Loop();
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
    }
#endif

    void Loop()
    {
        SaveClockToRtc(); // snapshot time into RTC memory (no-op without LIONWIFI_RTC_CLOCK / before sync)

        if (!_on)
        {
#ifndef ESP32
            ESP.wdtFeed();
            return;
#endif
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            if (!_connected)
            {
                _connected = true;
                _lastConnectedTime = millis();
                Logger.Log_P(ILogger::LvlInfo, PSTR("Connected to %s; IP address: %s"), _ssids[_curApIdx]->c_str(), WiFi.localIP().toString().c_str());
                configTime(NTP_TZ_OFFSET_SEC, 0, NTP_SERVER);
#if defined(ESP32)
                server.begin();
                Logger.Log_P(ILogger::LvlInfo, PSTR("HTTP server started"));
#endif
                if (!_otaStarted) // begin OTA once; re-begin on each reconnect re-advertises mDNS needlessly
                {
                    _myOta->Begin();
                    _otaStarted = true;
                }
                if (_conEvent)
                    _conEvent();
            }
            _myOta->Loop();
#if defined(NO_ASYNC_WEB_SERVER) || !defined(ESP32)
            server.handleClient();
#endif
#ifdef PING_ROUTER
            if (millis() - _lastRouterPingTime > PING_ROUTER_INTERVAL)
            {
                _lastRouterPingTime = millis();
                // Use a private, short-lived client — never the shared _client,
                // which a consumer may be using concurrently (esp. on ESP32 where
                // this runs in the WiFi task while handlers run elsewhere).
                WiFiClient pingClient;
                pingClient.setTimeout(WIFI_CLIENT_TIMEOUT);
                if (pingClient.connect(PING_ROUTER, 80))
                {
                    _routerPingErrorsInRow = 0;
                    ++_routerPingSuccessesInRow;
                }
                else
                {
                    _routerPingSuccessesInRow = 0;
                    Logger.Log_P(ILogger::LvlWarning, PSTR("Router ping failed in %lums"), millis() - _lastRouterPingTime);
                    if (++_routerPingErrorsInRow > PING_ROUTER_MAX_FAILURES)
                    {
                        Logger.Log_P(ILogger::LvlInfo, PSTR("!!! Rebooting device as router ping failed %d times"), _routerPingErrorsInRow);
                        delay(300);
                        ESP.restart();
                    }
                }
#ifdef ESP32
                pingClient.stop();
#else
                pingClient.abort();
#endif
            }
#endif
        }
        else
        {
            if (_connected)
            {
                _connected = false;
                _lastConnectedTime = millis();
#ifndef QUIET_WIFI_LOGS
                Logger.Log_P(ILogger::LvlInfo, PSTR("WiFI disconnected (status = %d), trying to reconnect..."), WiFi.status());
#endif
                if (_disconEvent)
                    _disconEvent();
                Reconnect();
            }
#ifndef FORBID_WIFI_MONITOR
            else if (millis() - _lastConnectedTime > WIFI_FATAL_CONNECT_TIMEOUT)
            {
                Logger.Log_P(ILogger::LvlInfo, PSTR("!!! Rebooting device as disconnected for %lus"), (millis() - _lastConnectedTime) / 1000UL);
                delay(300);
                ESP.restart();
            }
#endif
            else if (millis() - _lastConnectStartTime > WIFI_CONNECT_TIMEOUT)
            {
                ChangeIdx();
#ifndef QUIET_WIFI_LOGS
                Logger.Log_P(ILogger::LvlInfo, PSTR("Changing AP to %s after %lus"), _ssids[_curApIdx]->c_str(), WIFI_CONNECT_TIMEOUT / 1000ul);
#endif
                Reconnect();
            }
        }

        if (!_timeSet)
        {
            time_t now;
            time(&now);
            if (now > MIN_VALID_EPOCH)
            {
                randomSeed(now);
                _timeSet = true;
                struct tm *timeinfo = localtime(&now);
                if (timeinfo)
                    Logger.Log_P(ILogger::LvlInfo, PSTR("+++Restarted %02d/%02d/%04d %02d:%02d:%02d+++, built %S %S"),
                                 timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, F(__DATE__), F(__TIME__));

#ifdef ESP32
                RESET_REASON reason = rtc_get_reset_reason(0);
                Logger.Log_P(ILogger::LvlInfo, PSTR("Restart reason %S"),
                             ResetReason(reason));
#else
                struct rst_info *rtc_info = system_get_rst_info();
                Logger.Log_P(ILogger::LvlInfo, PSTR("Restart reason %S, exception %d, epc1=0x%08x, epc2=0x%08x, epc3=0x%08x, excvaddr=0x%08x, depc=0x%08x"),
                             ResetReason(rtc_info->reason), rtc_info->exccause, rtc_info->epc1, rtc_info->epc2, rtc_info->epc3, rtc_info->excvaddr, rtc_info->depc);
#endif                             

                if (_timeSetEvent)
                    _timeSetEvent();
            }
        }
#ifdef FS_LOW_SPACE_THRESHOLD
        // Wide-level free-space watchdog: updates Logger's low-space flag (read by the
        // status LED) and, if FS_LOW_SPACE_AUTOCLEAN is enabled, trims oldest logs.
        // Runs in all modes (a near-full FS silently fails small writes even offline).
#ifndef FS_SPACE_CHECK_INTERVAL_MS
#define FS_SPACE_CHECK_INTERVAL_MS 60000ul
#endif
        if (!_lastFreeSpaceCheck || millis() - _lastFreeSpaceCheck > FS_SPACE_CHECK_INTERVAL_MS)
        {
            _lastFreeSpaceCheck = millis();
            Logger.EnsureFreeSpace(FS_LOW_SPACE_THRESHOLD, FS_LOW_SPACE_TARGET);
        }
#endif
        // Periodic log maintenance. The dateless-log size cap runs in ALL modes
        // (those logs accumulate even without NTP time, e.g. offline/guest); the
        // date-based clearing needs a valid date, so it only runs once time is set.
        if (!_lastLogClearTime || millis() - _lastLogClearTime > (uint32_t)LOG_CLEAR_EVERY_HOURS * 3600UL * 1000UL) // Startup or time passed
        {
            _lastLogClearTime = millis();
#ifdef LOG_CLEARING_ENABLED
#ifdef MAX_LOG_BYTES
            Logger.CapDatelessLogIfNeeded(MAX_LOG_BYTES);
#endif
            if (_timeSet)
            {
                Logger.ClearOldLogs(LOG_CLEAR_DAYS, false, true);
#ifdef LOG_CLEAR_FREE_SPACE
                Logger.ClearOldestLogIfNeeded(LOG_CLEAR_FREE_SPACE);
#endif
                if (_clearLogEvent)
                    _clearLogEvent();
            }
#endif // LOG_CLEARING_ENABLED
        }

        if (millis() - _lastPingTime >= _pingEveryMs) // ping time...
        {
            if (_pingEvent)
                _pingEvent();
            else
            {
#ifdef PING_ROUTER
                Logger.Log_P(ILogger::LvlDebug, PSTR("Ping [%lums] %d/%d ok/bad"), millis() - _lastPingTime, _routerPingSuccessesInRow, _routerPingErrorsInRow);
#else
                Logger.Log_P(ILogger::LvlDebug, PSTR("Ping [%lums]"), millis() - _lastPingTime);
#endif
            }

            _lastPingTime = millis();
        }

#ifndef ESP32
        ESP.wdtFeed();

        // getHeapStats() walks the whole umm heap with interrupts disabled — throttle
        // it (like the FS-space check above) instead of running it every Loop() pass.
#ifndef HEAP_CHECK_INTERVAL_MS
#define HEAP_CHECK_INTERVAL_MS 1000ul
#endif
        if (!_lastHeapCheckTime || millis() - _lastHeapCheckTime > HEAP_CHECK_INTERVAL_MS)
        {
            _lastHeapCheckTime = millis();
            uint32_t hfree, hmax;
            uint8_t hfrag;
            ESP.getHeapStats(&hfree, &hmax, &hfrag);
            if (hfree < _minFreeMemory)
            {
                _minFreeMemory = hfree;
                Logger.Log_P(ILogger::LvlInfo, PSTR("====> New free heap = %lu (max %lu, frag %d)"), (unsigned long)_minFreeMemory, (unsigned long)hmax, hfrag);
            }
        }
#else
        uint32_t free = ESP.getFreeHeap();
        uint32_t maxAlloc = ESP.getMaxAllocHeap();

        if (free < _minFreeMemory)
        {
            _minFreeMemory = free;
            Logger.Log_P(ILogger::LvlInfo, PSTR("====> heap = %lu (max %lu)"), (unsigned long)_minFreeMemory, (unsigned long)maxAlloc);
        }
        vTaskDelay(1);
#endif
    }

protected:
    void ChangeIdx()
    {
        if (++_curApIdx >= _ssids.Length())
            _curApIdx = 0;
    }
    void Connect()
    {
        _lastConnectStartTime = millis();
#ifdef ESP32
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
#ifdef DISABLE_11N // force 802.11b/g only (some APs are flaky with 11n on ESP32)
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
#endif
#ifdef MAX_WIFI_POWER // crank TX power to the maximum for weak-signal links
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
#endif
#else
            WiFi.mode(WIFI_STA);
            WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
        WiFi.persistent(false);
        WiFi.disconnect(true);
#ifndef QUIET_WIFI_LOGS
        // Never log the PSK — SSID only.
        Logger.Log_P(ILogger::LvlInfo, PSTR("Connecting to %s"), _ssids[_curApIdx]->c_str());
#endif
        WiFi.begin(_ssids[_curApIdx]->c_str(), _passwords[_curApIdx]->c_str());
    }
    void Reconnect()
    {
        _lastConnectStartTime = millis();
#ifndef QUIET_WIFI_LOGS
        // Never log the PSK — SSID only.
        Logger.Log_P(ILogger::LvlInfo, PSTR("Reconnecting to %s"), _ssids[_curApIdx]->c_str());
#endif
        WiFi.begin(_ssids[_curApIdx]->c_str(), _passwords[_curApIdx]->c_str());
    }
    void Disconnect()
    {
        WiFi.mode(WIFI_STA);
#ifdef ESP32
        WiFi.setSleep(WIFI_PS_NONE);
#else
            WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
        WiFi.persistent(false);
        WiFi.disconnect(true);
        Logger.Log_P(ILogger::LvlInfo, PSTR("Force disconnected"));
    }
};

extern WifiConnector *_connector;
