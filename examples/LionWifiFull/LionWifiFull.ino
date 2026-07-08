// LionWifi — full example (ESP8266 / ESP32, sync OR async web server)
//
// Builds on LionWifiBasic and demonstrates:
//   * all five event callbacks (connected / disconnected / time-set /
//     log-clear / ping),
//   * a custom ping interval + a ping handler that logs heap health,
//   * a custom "/status" route that renders WifiConnector::StatusHtml() and
//     streams it — via ServerStream on the synchronous server, or an
//     AsyncResponseStream on ESPAsyncWebServer (both are Arduino `Print`s, so
//     the page body is built once in renderStatus()),
//   * reading connector state (Connected / TimeSet / GetMinFreeMemory).
//
// Works on every supported web-server backend:
//   * ESP8266                          — ESP8266WebServer
//   * ESP32 default                    — AsyncWebServer (ESPAsyncWebServer + AsyncTCP)
//   * ESP32 + -D NO_ASYNC_WEB_SERVER   — core WebServer
//
// Build flags (see platformio.ini): -D USE_SPIFFS or -D LFS. Override WiFi creds
// with -D WIFI_SSID / -D WIFI_PASSWORD.

#include <Logger.h>

// ---- Restyling the file-browser page (/spiffs/ls) ---------------------------
// These macros are read inside FsBrowser.h, so they must be #define'd BEFORE
// <WifiConnector.h> (which includes it). In an .ino that means right here; in a
// PlatformIO project you'd usually pass the same thing via build_flags instead
// (e.g. -D NO_FS_BROWSER_CSS). All are OFF here so the example shows the default
// look — uncomment ONE to try it.
//
// (a) Turn styling off entirely — no <style>, plain browser table, saves ~2 KB:
// #define NO_FS_BROWSER_CSS
//
// (b) Keep the built-in sheet but re-theme it — prepend var overrides, then
//     append the exposed default (adjacent string literals concatenate):
// #define FS_BROWSER_CSS ":root{--fs-accent:#0aa;--fs-danger:#e11}" DEFAULT_FS_BROWSER_CSS
//
// (c) Replace the stylesheet completely with your own (flash cost = your string):
// #define FS_BROWSER_CSS "body{font-family:monospace;margin:1rem}" \
//                        ".fs-table{border-collapse:collapse}" \
//                        ".fs-table td,.fs-table th{border:1px solid #999;padding:4px 8px}" \
//                        ".act-del{color:#c00}"

#include <WifiConnector.h>

// ServerStream is the synchronous-server streaming sink (not used on async).
#if !defined(ESP32) || defined(NO_ASYNC_WEB_SERVER)
#include <ServerStream.h>
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_PASSWORD"
#endif

// ---- The three globals LionWifi declares `extern` and the consumer defines ---
MyLogger Logger(ILogger::SerialPort | ILogger::Spiffs, ILogger::LvlDebug);

#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
WebServer server(80);
#else
AsyncWebServer server(80);
#endif
#else // ESP8266
ESP8266WebServer server(80);
#endif

WifiConnector *_connector;

// Status page body — written to any Print sink (ServerStream / AsyncResponseStream).
static void renderStatus(Print &out)
{
    out.print(F("<!doctype html><html><body><h3>LionWifi status</h3>"));
    _connector->StatusHtml(out);
    out.print(F("<p><a href=\"/spiffs/ls\">files</a> &middot; <a href=\"/log/tail\">log</a></p></body></html>"));
}

void setup()
{
    Serial.begin(115200);
    Logger.Setup();

    _connector = new WifiConnector(WIFI_SSID, WIFI_PASSWORD);

    // --- All five event callbacks --------------------------------------------
    _connector->RegisterConnectedEvent([]()
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("[cb] connected — http://%s/status"), WiFi.localIP().toString().c_str());
    });
    _connector->RegisterDisconnectedEvent([]()
    {
        Logger.Log_P(ILogger::LvlWarning, PSTR("[cb] disconnected — reconnecting"));
    });
    _connector->RegisterTimeSetEvent([]()
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("[cb] NTP time acquired (uptime now tracked)"));
    });
    _connector->RegisterClearLogEvent([]()
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("[cb] old logs cleared"));
    });
    // Replaces the default periodic "Ping" log line with our own heap report.
    _connector->RegisterPingEvent([]()
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("[ping] connected=%d timeSet=%d minFreeHeap=%u"),
                     _connector->Connected(), _connector->TimeSet(), _connector->GetMinFreeMemory());
    });
    _connector->SetPingTime(60000); // fire the ping callback once a minute

    // --- OTA hooks: quiesce a heavy peripheral for the duration of an update ---
    // (e.g. an ESP32-HUB75 I2S-DMA matrix that would otherwise starve the OTA).
    // LionWifi keeps owning the FS unmount/remount; these just bracket it.
    _connector->RegisterOtaStartEvent([](bool sketchUpload)
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("[ota] start (%S) — stopping peripherals"),
                     sketchUpload ? F("sketch") : F("filesystem"));
        // if (display) display->stopDMAoutput();
    });
    _connector->RegisterOtaProgressEvent([](unsigned int progress, unsigned int total)
    {
        // Fires on every chunk; throttle here if you drive a UI.
        (void)progress; (void)total;
    });
    _connector->RegisterOtaEndEvent([](bool ok)
    {
        // ok=true → update done (device reboots shortly); ok=false → failed,
        // no auto-reboot, so restore whatever was quiesced above.
        Logger.Log_P(ILogger::LvlInfo, PSTR("[ota] end ok=%d"), ok);
        // if (!ok && display) display->restartDMAoutput();
    });

    _connector->Setup();

    // --- Custom /status route: render StatusHtml() straight to the response ---
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // Async: AsyncResponseStream is a Print; the response is sent in one call.
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        AsyncResponseStream *response = request->beginResponseStream("text/html");
        renderStatus(*response);
        request->send(response);
    });
#else
    // Sync: stream chunks via ServerStream (the page never lives in RAM whole).
    server.on("/status", []()
    {
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "text/html", ""); // headers only — NOT send_P("") (see FsBrowser)
        ServerStream out(server);
        renderStatus(out);
        out.flush();                       // emit the final partial chunk, then terminate
#ifdef ESP32
        server.sendContent("");            // ESP32 WebServer: terminating "0\r\n\r\n" chunk
#else
        server.chunkedResponseFinalize();  // ESP8266
#endif
    });
#endif
}

void loop()
{
#if !defined(ESP32) || defined(NO_WIFI_TASK)
    _connector->Loop();
#endif
}
