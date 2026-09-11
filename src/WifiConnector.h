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
//   * WebServerType server(80);      // the web server. WebServerType is a typedef this
//     header provides, so that line is literally portable; it resolves to
//        ESP8266:                     ESP8266WebServer
//        ESP32 (default, async):      AsyncWebServer            // ESPAsyncWebServer
//        ESP32 + NO_ASYNC_WEB_SERVER: WebServer
//     Use it in your own signatures too, and a port stops touching your code.
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
//   WIFI_BEST_AP             Associate with the STRONGEST access point carrying the
//                            configured SSID instead of the first one found. OFF by
//                            default (both cores). Needed wherever several points share
//                            an SSID — mesh, extender, second router: the default picks
//                            whichever answers first, which is a coin toss. A hub was
//                            found sitting on an extender 20 m away through a wall at
//                            RSSI -82 while another point stood one metre from it; the
//                            flag turned that into -41. Costs a full channel sweep (a
//                            second or two) per association ATTEMPT, retries included.
//                            ESP32: WIFI_ALL_CHANNEL_SCAN + sort by signal, the core does
//                            the rest. ESP8266: no such knob exists there, so the library
//                            scans itself (SSID-filtered, so only matching points are
//                            collected) and hands begin() the winner's channel+BSSID; the
//                            result list is freed right after, but it is still an
//                            allocation — mind a node already short on heap.
//                            Picks by SIGNAL, and signal is not path quality: it will
//                            happily choose a loud point whose uplink is broken.
//
//   Router ping watchdog (opt-in):
//   PING_ROUTER              Router IP/host string — enables periodic TCP ping;
//                            reboots after PING_ROUTER_MAX_FAILURES (default 4)
//                            misses, every PING_ROUTER_INTERVAL ms (default 30000).
//
//   Gratuitous ARP (opt-in, default OFF):
//   LIONWIFI_GARP_INTERVAL_MS  Broadcast an unsolicited "this IP is at this MAC"
//                            ARP on every association and then every N ms
//                            (0 = feature compiled out, the default). Cures the
//                            case where a node re-associates to a DIFFERENT access
//                            point and stays invisible to some peers for minutes:
//                            the node's own traffic is unicast to the router, so it
//                            only refreshes that one path, while the ARP caches of
//                            other nodes and the forwarding tables of other points
//                            keep pointing at the old radio. A broadcast reaches the
//                            whole L2 domain at once. 120000 is a sane value; going
//                            much below that just burns airtime (broadcasts go out
//                            at the lowest basic rate and wake every client).
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
//                            Both of the above become MANDATORY when LionLogger is
//                            built without ASYNC_LOG: each one otherwise adds a
//                            second task that logs, and synchronous logging tolerates
//                            exactly one. Enforced by #error further down.
//   CORE_WIFI                FreeRTOS core for the WiFi task (default 1).
//   DISABLE_11N              Force 802.11b/g (some APs misbehave with 11n).
//   WIFI_BW20                Force a 20 MHz channel (HT20) on the station link — steadier on
//                            a weak or noisy one than the default 40 MHz.
//   MAX_WIFI_POWER           Set max TX power for weak links (that is also the default).
//   WIFI_TX_POWER            Set TX power explicitly, as a wifi_power_t enumerator. Meant
//                            for LOWERING it: small boards with weak decoupling fail to
//                            associate at full power (-D WIFI_TX_POWER=WIFI_POWER_8_5dBm).
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

// The sync web server is pumped by Loop(), and without NO_WIFI_TASK that runs in the
// connector's own FreeRTOS task — so your route handlers execute there, concurrently with
// loop(). That is fine for a sketch whose handlers touch nothing shared, which is why this
// is a warning and not an #error; but it is never what a port from ESP8266 wants, where
// every handler was written assuming a single context. (And a leftover
// `_connector->Loop()` in loop() then pumps the same socket from two contexts at once.)
#if defined(ESP32) && defined(NO_ASYNC_WEB_SERVER) && !defined(NO_WIFI_TASK)
#warning "LionWifi: NO_ASYNC_WEB_SERVER without NO_WIFI_TASK - web handlers run in the connector's task, concurrently with loop(). Define NO_WIFI_TASK to keep them in loop()."
#endif

// Synchronous logging (ESP32 without ASYNC_LOG) is only valid when the entire
// firmware logs from ONE FreeRTOS task: Log_P() then writes the file in the
// caller's context through a shared File member, and a second logging context
// closes that handle underneath the first (see the warning at the top of
// Logger.h). Both configurations below add exactly such a context, and both do it
// with LionWifi's OWN logging - reconnects, OTA progress, the file browser - so
// "my handlers don't log" is not an escape. Both are known at compile time, hence
// #error rather than a corrupted log file three days later.
#if defined(ESP32) && !defined(ASYNC_LOG)
#ifndef NO_WIFI_TASK
#error "LionWifi: ESP32 without ASYNC_LOG requires NO_WIFI_TASK - the connector's FreeRTOS task logs (reconnects, OTA), which is a second synchronous-logging context. Define NO_WIFI_TASK, or enable ASYNC_LOG."
#endif
#ifndef NO_ASYNC_WEB_SERVER
#error "LionWifi: ESP32 without ASYNC_LOG requires NO_ASYNC_WEB_SERVER - async route handlers (and the library's own) run in the AsyncTCP task, which is a second synchronous-logging context. Define NO_ASYNC_WEB_SERVER, or enable ASYNC_LOG."
#endif
#endif

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

// Gratuitous ARP. OFF by default: it costs airtime on every node that enables it,
// and it only pays off where several access points share the SSID. See the flag
// list at the top of this file.
#ifndef LIONWIFI_GARP_INTERVAL_MS
#define LIONWIFI_GARP_INTERVAL_MS 0ul
#endif

#if LIONWIFI_GARP_INTERVAL_MS > 0
// etharp_gratuitous() builds an ARP request whose sender and target IP are both
// ours — the standard "here I am" broadcast. Present in both cores' lwIP.
#include <lwip/etharp.h>
#ifdef ESP32
#include <lwip/tcpip.h> // tcpip_callback: lwIP has its own task here, see SendGratuitousArp
#endif
#endif

// Async responses that end in a reboot (/restart, successful /update) fire it from
// onDisconnect, so the client actually receives the reply first. If the client never
// closes the connection, this is how long Loop() waits before rebooting anyway —
// otherwise a keep-alive'ing browser could park the device on the old firmware.
#ifndef REBOOT_FALLBACK_MS
#define REBOOT_FALLBACK_MS 5000ul
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

// The concrete web-server class of this build. Use it in your own signatures
// (`Init(WebServerType &server)`, handler helpers, ...) instead of naming a platform class:
// porting a sketch between ESP8266 and ESP32 then changes build flags, not code. Same idea
// as LIONWIFI_FS for the filesystem.
#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
typedef WebServer WebServerType;
#else
typedef AsyncWebServer WebServerType;
#endif
#else
typedef ESP8266WebServer WebServerType;
#endif

extern WebServerType server;

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
#if LIONWIFI_GARP_INTERVAL_MS > 0
    uint32_t _lastGarpTime = 0;
    // Which access point we last announced ourselves on, to spot a silent roam.
    uint8_t _lastBssid[6] = {0, 0, 0, 0, 0, 0};
    int32_t _lastChannel = 0;
#endif
    MyOta *_myOta = nullptr;
    Array<String *> _ssids, _passwords;
    int _curApIdx = 0;
    uint32_t _minFreeMemory = 1000000; // seed high so the first heap sample always wins
    uint32_t _minFreeStack = 0xFFFFFFFF; // same, for the stack low-water mark
#if defined(ESP32) && !defined(NO_WIFI_TASK)
    bool _loopFromSketchWarned = false; // warn once about an ignored Loop() from loop()
#endif

    // Free stack of the context that pumps Loop(): the ESP8266 cont stack, or on ESP32 the
    // task running this. Note the differing units of the underlying calls — ESP-IDF's
    // uxTaskGetStackHighWaterMark() reports BYTES (vanilla FreeRTOS reports words).
    static uint32_t FreeStackBytes()
    {
#ifdef ESP32
        return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
#else
        return (uint32_t)ESP.getFreeContStack();
#endif
    }
    FsBrowser *_fsBrowser = nullptr;
    std::function<void()> _conEvent, _disconEvent, _timeSetEvent, _clearLogEvent, _pingEvent;
    // OTA hooks — forwarded to _myOta (which invokes them from ArduinoOTA's
    // callbacks). start's bool = sketch upload (vs FS image); end's bool = ok.
    std::function<void(bool)> _otaStartEvent, _otaEndEvent;
    std::function<void(unsigned int, unsigned int)> _otaProgressEvent;
    WiFiClient *_client = nullptr;
    HTTPClient *_httpClient = nullptr;
    bool _otaStarted = false;
#ifdef ESP32
    // Filled by the WiFi event callback, printed by Loop(). The callback runs in the
    // Arduino event task, and the log must be written from ONE task only — so the
    // callback just leaves the reason here and does not touch the logger.
    volatile uint8_t _discReason = 0;
    volatile bool _discReasonPending = false;
#endif
#ifdef LIONWIFI_HTTP_OTA
    bool _httpOtaAuthOk = false; // latched when the upload starts; response gated on it
#endif
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // Deadline for the fallback reboot; 0 = none pending. See scheduleReboot().
    uint32_t _rebootAt = 0;
    const char *_rebootWhy = nullptr;

    // A reboot that must not cut the response short. onDisconnect() reboots as soon as
    // the client is gone; this deadline covers the case where it never disconnects.
    // Both paths land on ESP.restart(), whichever comes first.
    void scheduleReboot(const char *why)
    {
        _rebootWhy = why;
        _rebootAt = millis() + REBOOT_FALLBACK_MS;
        if (!_rebootAt)
            _rebootAt = 1; // millis() wrapped exactly onto 0 — keep "pending" truthy
    }
#endif

#if LIONWIFI_GARP_INTERVAL_MS > 0
    // Broadcast "our IP is at our MAC", unasked. Every ARP cache and every bridge
    // forwarding table in the L2 domain then learns where we are NOW — which plain
    // traffic does not achieve: ours is unicast to the router, so it refreshes that
    // one path only, and a peer sitting behind another access point keeps sending
    // to the radio we already left. One 42-byte frame, no allocation, no blocking.
    // atAssociation: called from the connect path, where a "changed" access point is
    // simply the one we just joined and must not be reported as a roam.
    void SendGratuitousArp(bool atAssociation = false)
    {
        // Plain assignment: the caller's test is a (millis() - _lastGarpTime)
        // difference, which survives the rollover on its own.
        _lastGarpTime = millis();

        // netif_default is the station interface in a station-only sketch, which is
        // every sketch this library serves.
#ifdef ESP32
        // lwIP has its own task here, and calling etharp_* from ours corrupts its
        // state. tcpip_callback hands the work over. NOTHING may log inside that
        // callback — it runs in the tcpip task.
        tcpip_callback([](void *)
                       {
            if (netif_default)
                etharp_gratuitous(netif_default); },
                       nullptr);
#else
        // No separate lwIP task on the ESP8266 — the whole Arduino core calls lwIP
        // straight from loop().
        if (netif_default)
            etharp_gratuitous(netif_default);
#endif

        // Which point are we announcing ourselves on? The SDK re-associates on its own
        // after a deauth and can land on a DIFFERENT BSSID carrying the same SSID without
        // Connect() ever running — and if that reassociation beats one Loop() pass,
        // _connected never flips, so not even "Connected to" appears. A silent roam is
        // precisely the fault this feature exists for, so it must not go unrecorded, and
        // this two-minute tick is the cheapest detector available.
        //
        // Logging follows the fleet's dedup rule: a bare tag while nothing changes, a full
        // line when it does. (720 bare tags a day are ~10 KB; the full line is what one
        // actually greps for afterwards.)
        const uint8_t *bssid = WiFi.BSSID();
        int32_t channel = WiFi.channel();
        bool changed = !bssid || channel != _lastChannel || memcmp(bssid, _lastBssid, sizeof(_lastBssid)) != 0;
        if (bssid)
            memcpy(_lastBssid, bssid, sizeof(_lastBssid));
        _lastChannel = channel;

        if (atAssociation)
            Logger.Log_P(ILogger::LvlInfo, PSTR("GARP armed on %02x:%02x:%02x:%02x:%02x:%02x ch %d, every %lums"),
                         _lastBssid[0], _lastBssid[1], _lastBssid[2], _lastBssid[3], _lastBssid[4], _lastBssid[5],
                         (int)channel, (unsigned long)LIONWIFI_GARP_INTERVAL_MS);
        else if (changed)
            Logger.Log_P(ILogger::LvlInfo, PSTR("GARP: AP CHANGED to %02x:%02x:%02x:%02x:%02x:%02x ch %d, RSSI %d"),
                         _lastBssid[0], _lastBssid[1], _lastBssid[2], _lastBssid[3], _lastBssid[4], _lastBssid[5],
                         (int)channel, (int)WiFi.RSSI());
        else
            Logger.Log_P(ILogger::LvlDebug, PSTR("GARP"));
    }
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
        out.printf_P(PSTR("Heap: <b>%u</b>B (loop min <b>%u</b>) | Frag: <b>%d%%</b> | Stack: <b>%u</b>B (min <b>%u</b>)"),
                      freeHeap, _minFreeMemory, frag, freeStack, _minFreeStack);
#else
        out.printf_P(PSTR("Heap: <b>%u</b>B (min <b>%u</b>) | Largest: <b>%s</b> (%u%%) | Stack min: <b>%u</b>B"),
                      freeHeap, _minFreeMemory, FsBrowser::FileSize(maxAlloc).c_str(), largestPct, _minFreeStack);
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
                // send() лишь СТАВИТ ответ в очередь async-стека: ребут через delay()
                // обрывал соединение до отправки — curl ждал ответа до своего таймаута.
                // Ребутимся по onDisconnect: Connection:close закрывает соединение
                // сервером сразу после доставки ответа клиенту.
                if (ok)
                {
                    request->onDisconnect([]() { ESP.restart(); });
                    // ...и не ждать закрытия вечно: клиент, держащий соединение, иначе
                    // оставил бы устройство работать на СТАРОЙ прошивке неограниченно.
                    scheduleReboot(PSTR("HTTP OTA"));
                }
                request->send(resp);
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
                // total = длина POST-запроса: чуть больше бинарника (multipart-обвязка,
                // <1%), но даёт честные проценты — Update.begin() шёл с SIZE_UNKNOWN.
                if (_otaProgressEvent) _otaProgressEvent((unsigned int)(index + len), (unsigned int)request->contentLength());
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
#ifdef ESP32
                    // total = длина POST (multipart-обвязка даёт погрешность <1%).
                    if (_otaProgressEvent) _otaProgressEvent((unsigned int)up.totalSize, (unsigned int)server.clientContentLength());
#else
                    // ESP8266WebServer не отдаёт Content-Length наружу — total неизвестен.
                    if (_otaProgressEvent) _otaProgressEvent((unsigned int)up.totalSize, 0);
#endif
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

#ifdef ESP32
        // A failed association is otherwise completely silent: the log shows nothing but
        // "Connecting/Reconnecting" repeating forever, and that covers several unrelated
        // faults. The SDK does say which (wifi_err_reason_t):
        //   201 NO_AP_FOUND             — the access point is not seen at all (coverage,
        //                                 channel, wrong band)
        //   15  4WAY_HANDSHAKE_TIMEOUT  — it is seen but the handshake never completes:
        //                                 a weak link, or a supply sagging on TX peaks
        //   2/4 AUTH_EXPIRE/ASSOC_EXPIRE— the access point drops us (loaded extender,
        //                                 full client table)
        //   205 CONNECTION_FAIL         — association did not finish
        // The callback runs in the Arduino event task, so it only records the reason;
        // Loop() prints it from our own task (the log tolerates a single writer).
        WiFi.onEvent([this](arduino_event_id_t, arduino_event_info_t info)
                     {
                         const uint8_t reason = (uint8_t)info.wifi_sta_disconnected.reason;
                         // ASSOC_LEAVE means the station left of its own accord — that is
                         // our own disconnect(true) in Connect()/Reconnect(), fired on every
                         // retry. Reporting it would bury the reasons that mean something.
                         if (reason == WIFI_REASON_ASSOC_LEAVE)
                             return;
                         _discReason = reason;
                         _discReasonPending = true;
                     },
                     ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

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
        // Connection reuse OFF, and this is not a preference — with a SHARED client it is
        // broken by construction. HTTPClient::connect() reuses whatever socket the client
        // holds the moment connected() is true and never compares the host, while
        // begin(client, url) forces _canReuse = true and calls disconnect(true), which under
        // reuse deliberately leaves the previous socket open. So the request meant for the
        // next host goes into the socket of the previous one. And even talking to the same
        // host again it fails: a small ESP web server answers "Connection: keep-alive" with
        // a two-second idle timeout, so by the time a poller comes back the peer has closed
        // and the write lands nowhere — an HTTPC_ERROR_READ_TIMEOUT (-11) that looks exactly
        // like a slow device. Every request now opens its own connection, which is what the
        // peers expect anyway.
        _httpClient->setReuse(false);

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
            // send() only QUEUES the response on the async stack: the old delay(100) +
            // ESP.restart() tore the connection down before it was delivered, and the
            // browser silently retried the (idempotent) GET once the device was back —
            // one click produced two reboots. Reboot from onDisconnect instead, with
            // Connection:close so the server hangs up right after delivery.
            AsyncWebServerResponse *resp = request->beginResponse(200, __text_plain__F, F("Restarting..."));
            resp->addHeader(F("Connection"), F("close"));
            request->onDisconnect([]() { ESP.restart(); });
            scheduleReboot(PSTR("/restart"));
            request->send(resp); });
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
            LoopBody();
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
    }
#endif

    // Public entry point. In a build where the connector owns a FreeRTOS task, a call from
    // the sketch's loop() would pump everything from TWO contexts at once — two parsers
    // reading the same socket, half a request each. That shows up as nonsense in the log
    // ("Invalid request: Referer: ...", remoteIP() reported as 0.0.0.0, the same URI handled
    // twice, "Connection reset by peer") and is easy to mistake for broken authentication.
    // Consumers keep one `_connector->Loop()` in loop() for every platform, so this is a
    // no-op here rather than an error.
    void Loop()
    {
#if defined(ESP32) && !defined(NO_WIFI_TASK)
        if (!_loopFromSketchWarned) // once: Loop() is called thousands of times a second
        {
            _loopFromSketchWarned = true;
            Logger.Log_P(ILogger::LvlWarning,
                         PSTR("WifiConnector::Loop() from the sketch IGNORED: this build pumps it "
                              "from its own task. Define NO_WIFI_TASK to drive it from loop()."));
        }
        return;
#else
        LoopBody();
#endif
    }

private:
    void LoopBody()
    {
        SaveClockToRtc(); // snapshot time into RTC memory (no-op without LIONWIFI_RTC_CLOCK / before sync)

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        // Fallback for a scheduled reboot whose onDisconnect never fired (client kept
        // the connection open). Signed compare so a millis() wrap can't defer it.
        if (_rebootAt && (int32_t)(millis() - _rebootAt) >= 0)
        {
            Logger.Log_P(ILogger::LvlWarning, PSTR("Rebooting (%S): client never closed the connection"),
                         _rebootWhy ? _rebootWhy : PSTR("?"));
            ESP.restart();
        }
#endif

        if (!_on)
        {
#ifndef ESP32
            ESP.wdtFeed();
            return;
#endif
        }

#ifdef ESP32
        // Print what the event callback recorded — see the WiFi.onEvent() comment in
        // Setup() for the reason codes worth knowing.
        if (_discReasonPending)
        {
            _discReasonPending = false;
            Logger.Log_P(ILogger::LvlWarning, PSTR("WiFi disconnect reason %u"), (unsigned)_discReason);
        }
#endif

        if (WiFi.status() == WL_CONNECTED)
        {
            if (!_connected)
            {
                _connected = true;
                _lastConnectedTime = millis();
                // RSSI right at association: the single number that separates "weak link"
                // from every other explanation. Above -65 dBm is comfortable, -75 marginal,
                // below -80 is where handshakes start timing out.
                Logger.Log_P(ILogger::LvlInfo, PSTR("Connected to %s; IP address: %s; RSSI %d"),
                             _ssids[_curApIdx]->c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
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
#if LIONWIFI_GARP_INTERVAL_MS > 0
                // The association is exactly the moment the network's idea of where
                // we live goes stale — announce before anything else needs us.
                SendGratuitousArp(true);
#endif
                if (_conEvent)
                    _conEvent();
            }
            _myOta->Loop();
#if defined(NO_ASYNC_WEB_SERVER) || !defined(ESP32)
#if defined(LIONWIFI_WEB_TRACE) || defined(LIONWIFI_WEB_STALL_LOG_MS)
            {
                // handleClient() is opaque from the outside: page handlers, the FS browser
                // and OTA all live inside it, and on ESP32 a socket write to a client that
                // stopped reading blocks with no ceiling. Timing the call is what separates
                // "the web stalled the loop" from every other cause — and that is all it
                // can do.
                //
                // The request CANNOT be named from here, so do not try: _handleRequest()
                // clears _currentUri when it finishes (WebServer.cpp), and remoteIP() on the
                // by-then-closed client calls getpeername() on a dead fd, ignores the
                // failure and returns an uninitialised address. That printed convincing
                // public IPs out of stack garbage — worse than no information at all. To
                // attribute a stall to a page, instrument the handlers themselves.
                const uint32_t webStart = millis();
                server.handleClient();
                const uint32_t webMs = millis() - webStart;
#ifdef LIONWIFI_WEB_TRACE
                // Only the slow ones: handleClient() runs every loop iteration (about a
                // millisecond even with no request), so tracing every call drowns the port.
                if (webMs >= LIONWIFI_WEB_TRACE_MS)
                    Serial.printf_P(PSTR("WEB %lu ms\n"), (unsigned long)webMs);
#endif
#ifdef LIONWIFI_WEB_STALL_LOG_MS
                if (webMs >= LIONWIFI_WEB_STALL_LOG_MS)
                    Logger.Log_P(ILogger::LvlWarning, PSTR("WEB STALL %lu ms"), (unsigned long)webMs);
#endif
            }
#else
            server.handleClient();
#endif
#endif
#if LIONWIFI_GARP_INTERVAL_MS > 0
            // Repeat while connected: the association-time announcement is lost on
            // anything that reboots or flushes its tables afterwards, and a point
            // that re-learns us wrongly (a client roaming past, a bridge aging out)
            // has to be corrected before somebody actually needs us. Doubles as the
            // silent-roam detector — see the logging at the end of SendGratuitousArp().
            if (millis() - _lastGarpTime >= LIONWIFI_GARP_INTERVAL_MS)
                SendGratuitousArp();
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
            TrackStackLowWater();
        }
#else
        uint32_t free = ESP.getFreeHeap();
        uint32_t maxAlloc = ESP.getMaxAllocHeap();

        if (free < _minFreeMemory)
        {
            _minFreeMemory = free;
            Logger.Log_P(ILogger::LvlInfo, PSTR("====> heap = %lu (max %lu)"), (unsigned long)_minFreeMemory, (unsigned long)maxAlloc);
        }
        TrackStackLowWater();
        vTaskDelay(1);
#endif
    }

    // Lowest free stack seen so far, logged whenever it drops. The status page shows the
    // CURRENT free stack, which says nothing about the rare deep excursion that actually
    // overflows — only a low-water mark catches those, and it costs one comparison per
    // heap check. (Found a 1136 -> 624 byte drop on ESP8266 that way, in a path nobody
    // suspected.) NO_MEMSTAT_IN_STATUS compiles it out together with the rest of the stats.
    void TrackStackLowWater()
    {
#ifndef NO_MEMSTAT_IN_STATUS
        uint32_t freeStack = FreeStackBytes();
        if (freeStack && freeStack < _minFreeStack)
        {
            _minFreeStack = freeStack;
            Logger.Log_P(ILogger::LvlInfo, PSTR("----> New free stack = %lu"), (unsigned long)_minFreeStack);
        }
#endif
    }

protected:
    void ChangeIdx()
    {
        if (++_curApIdx >= _ssids.Length())
            _curApIdx = 0;
    }
    // Station mode plus link tuning. Called before EVERY association attempt, the
    // retries included: a full teardown (disconnect(true) stops the radio) may drop
    // these, and they only take effect on the next association anyway.
    void ApplyRadioSettings()
    {
#ifdef ESP32
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
        // Radio settings below must sit HERE: after WiFi.mode() (before it esp_wifi is not
        // initialised yet and the calls fail) and before WiFi.begin() (applied later, they
        // only take effect on the NEXT reconnect).
#ifdef DISABLE_11N // force 802.11b/g only (some APs are flaky with 11n on ESP32)
        // WIFI_IF_STA, not WIFI_IF_AP: this used to configure the access-point interface,
        // which a station-only sketch never brings up — so the flag quietly did nothing to
        // the link it was meant to fix. No consumer in the fleet had it set, so nobody was
        // relying on the old no-op behaviour.
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
#endif
#ifdef WIFI_BEST_AP // pick the STRONGEST access point with this SSID, not the first found
        // The default is WIFI_FAST_SCAN, which stops at the first matching access point and
        // connects to it. With several points sharing one SSID (a mesh, an extender, or just
        // two routers) that is a coin toss: a hub sat on an extender 20 m away through a
        // wall at RSSI -82 while another point stood one metre from it. Sorting by signal is
        // already the default, but it has nothing to sort until the scan covers all channels.
        // Measured after the switch: -82 -> -41.
        // Cost: a full channel sweep on every association attempt, a second or two.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL); // the default, set explicitly
#endif
#ifdef WIFI_BW20 // force a 20 MHz channel width (HT20) on the station link
        // A narrower channel collects less noise, so on a weak link it holds better: at
        // -76 dBm against an access point running 40 MHz on a busy channel, HT20 buys real
        // SNR. Pair it with MAX_WIFI_POWER when the link is weak rather than noisy.
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
#endif
        // TX power. The default is already the maximum, so MAX_WIFI_POWER is mostly a
        // statement of intent; WIFI_TX_POWER is the interesting one, and it exists to be
        // set LOWER. Small boards (the ESP32-C3 SuperMini and its kin) have weak supply
        // decoupling and misbehave at full power — failing to associate, or associating and
        // dropping — where 8.5dBm connects happily. Pass any wifi_power_t enumerator, e.g.
        // -D WIFI_TX_POWER=WIFI_POWER_8_5dBm.
#ifdef WIFI_TX_POWER
        WiFi.setTxPower(WIFI_TX_POWER);
#elif defined(MAX_WIFI_POWER)
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
#endif
#else
            WiFi.mode(WIFI_STA);
            WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
    }

#if defined(WIFI_BEST_AP) && !defined(ESP32)
    // The ESP8266 core has no setScanMethod/setSortMethod — WiFi.begin() there always
    // takes the first matching point the SDK stumbles over. So the choice has to be made
    // by hand: scan every channel, keep the loudest match, and hand begin() the channel
    // and BSSID it found. Returns false when the SSID is nowhere to be seen, and the
    // caller then falls back to a plain begin() (better a coin toss than no association).
    bool FindBestAp(const char *ssid, int32_t &channel, uint8_t *bssidOut)
    {
        // Blocking scan (~2 s, all channels). The SDK yields inside, so the watchdog is
        // fed, but the sketch does stand still for it — the same price the ESP32 branch
        // pays with WIFI_ALL_CHANNEL_SCAN, once per association attempt.
        //
        // The 4th argument is an SSID filter, handed straight to the SDK's scan_config:
        // the sweep still covers every channel, but probes are directed and only matching
        // points come back. Without it the result list holds every access point in the
        // air — a neighbourhood's worth of them, all allocated on a 15 KB heap for
        // nothing. (Stations never show up in a scan either way: nodes are clients, they
        // send no beacons.)
        int found = WiFi.scanNetworks(false, false, 0, (uint8 *)ssid);

        // Read the entries through getNetworkInfo() rather than the WiFi.xxx(i) accessors.
        // Not a style choice: ESP8266WiFiClass inherits channel() from BOTH the generic and
        // the scan class and resolves the clash with `using ESP8266WiFiGenericClass::channel`
        // alone (ESP8266WiFi.h) — so the scan-index form WiFi.channel(i) is not visible at
        // all and does not compile. getNetworkInfo() hands over every field at once.
        int best = -1;
        int32_t bestRssi = 0;
        for (int i = 0; i < found; i++)
        {
            String foundSsid;
            uint8_t enc;
            int32_t rssi, ch;
            uint8_t *bssid = nullptr; // points into the scan results, valid until scanDelete()
            bool hidden;
            if (!WiFi.getNetworkInfo(i, foundSsid, enc, rssi, bssid, ch, hidden))
                continue;
            // The SSID test is redundant while the filter above works — but it is the only
            // thing standing between a silently ignored filter and a stranger's access point.
            if (foundSsid != ssid || (best >= 0 && rssi <= bestRssi))
                continue;
            best = i;
            bestRssi = rssi;
            channel = ch;
            if (bssid)
                memcpy(bssidOut, bssid, 6);
        }

        if (best >= 0)
            Logger.Log_P(ILogger::LvlInfo, PSTR("Best AP for %s: ch %d, RSSI %d, %02x:%02x:%02x:%02x:%02x:%02x (%d with this SSID)"),
                         ssid, (int)channel, (int)bestRssi,
                         bssidOut[0], bssidOut[1], bssidOut[2], bssidOut[3], bssidOut[4], bssidOut[5], found);
        else
            Logger.Log_P(ILogger::LvlWarning, PSTR("No AP with SSID %s in scan — plain begin()"), ssid);

        // Mandatory: the SDK holds the result list until told to drop it, and this runs on
        // a node with ~15 KB of heap.
        WiFi.scanDelete();
        return best >= 0;
    }
#endif

    // Single place where the association is actually started, so Connect() and Reconnect()
    // cannot drift apart on which point they aim at.
    void BeginStation()
    {
        const char *ssid = _ssids[_curApIdx]->c_str();
        const char *pwd = _passwords[_curApIdx]->c_str();
#if defined(WIFI_BEST_AP) && !defined(ESP32)
        int32_t channel = 0;
        uint8_t bssid[6];
        // Pinning the BSSID binds this ONE attempt. If that point is gone, the attempt
        // times out, the state machine calls Reconnect(), and the next scan picks again.
        if (FindBestAp(ssid, channel, bssid))
        {
            WiFi.begin(ssid, pwd, channel, bssid, true);
            return;
        }
#endif
        WiFi.begin(ssid, pwd);
    }

    void Connect()
    {
        _lastConnectStartTime = millis();
        // Order matters, and it is NOT the intuitive one. Tuning must come BEFORE the
        // teardown: disconnect(true) stops the radio, but the configured mode stays
        // WIFI_MODE_STA, and WiFiGenericClass::mode() returns early when the mode already
        // matches (WiFiGeneric.cpp:1252) -- so a WiFi.mode(WIFI_STA) after the teardown is
        // a no-op and the esp_wifi_* calls land on a stopped stack, silently (nobody checks
        // their return codes). Tried the other way round: association then failed with
        // reason 2 (AUTH_EXPIRE) and 39 (TIMEOUT), retry after retry.
        ApplyRadioSettings();
        WiFi.persistent(false);
        WiFi.disconnect(true);
#ifndef QUIET_WIFI_LOGS
        // Never log the PSK — SSID only.
        Logger.Log_P(ILogger::LvlInfo, PSTR("Connecting to %s"), _ssids[_curApIdx]->c_str());
#endif
        BeginStation();
    }
    void Reconnect()
    {
        _lastConnectStartTime = millis();
#ifndef QUIET_WIFI_LOGS
        // Never log the PSK — SSID only.
        Logger.Log_P(ILogger::LvlInfo, PSTR("Reconnecting to %s"), _ssids[_curApIdx]->c_str());
#endif
#ifdef ESP32
        // Same sequence as Connect(), and deliberately so. A retry used to be a bare
        // begin(), but on ESP32 begin() on top of an association attempt that is still
        // running does NOT restart it -- least of all with a different SSID: the previous
        // attempt keeps going to its own timeout and the new credentials are ignored. Seen
        // on an ESP32-C3 as an endless "Changing AP / Reconnecting" ladder every
        // WIFI_CONNECT_TIMEOUT, minutes on end, with an access point in the same room.
        // For why tuning precedes the teardown, see the comment in Connect().
        //
        // ESP8266 keeps the bare begin() below: its SDK handles a repeated begin() on its
        // own, the endless-ladder symptom has never been seen there, and a fleet of 8266
        // nodes has been reconnecting this way for years. A teardown there would also mean
        // powering the radio down (disconnect(true) = wifioff) on a path that works.
        ApplyRadioSettings();
        WiFi.persistent(false);
        WiFi.disconnect(true); // true = radio off; begin() below brings it back up
#endif
        BeginStation();
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
