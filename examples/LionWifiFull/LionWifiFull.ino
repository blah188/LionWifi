// LionWifi — full example (ESP8266 / ESP32 with the synchronous web server)
//
// Builds on LionWifiBasic and demonstrates:
//   * all five event callbacks (connected / disconnected / time-set /
//     log-clear / ping),
//   * a custom ping interval + a ping handler that logs heap health,
//   * a custom "/status" route that renders WifiConnector::StatusHtml() and
//     streams it with ServerStream (no big String held in RAM),
//   * reading connector state (Connected/TimeSet/GetMinFreeMemory).
//
// This example uses the SYNCHRONOUS web server (ServerStream needs it), so on
// ESP32 build with -D NO_ASYNC_WEB_SERVER. See LionWifiBasic for the async setup.
//
// Build flags (see platformio.ini): -D USE_SPIFFS, plus on ESP32
// -D NO_ASYNC_WEB_SERVER. Override WiFi creds with -D WIFI_SSID / -D WIFI_PASSWORD.

#include <Logger.h>
#include <WifiConnector.h>
#include <ServerStream.h>

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
#error "LionWifiFull uses the synchronous web server + ServerStream — build with -D NO_ASYNC_WEB_SERVER on ESP32 (or see LionWifiBasic)."
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
WebServer server(80); // synchronous (NO_ASYNC_WEB_SERVER)
#else
ESP8266WebServer server(80);
#endif

WifiConnector *_connector;

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

    _connector->Setup();

    // --- Custom /status route: render StatusHtml() straight to the response ---
    // ServerStream flushes in ~1 KB chunks, so the page never lives in RAM whole.
    server.on("/status", []()
    {
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "text/html", "");
        ServerStream out(server);
        out.print(F("<!doctype html><html><body><h3>LionWifi status</h3>"));
        _connector->StatusHtml(out);
        out.print(F("<p><a href=\"/spiffs/ls\">files</a> &middot; <a href=\"/log/tail\">log</a></p>"));
        out.print(F("</body></html>"));
        out.flush(); // send the final partial chunk
#ifndef ESP32
        server.chunkedResponseFinalize(); // ESP8266: terminate the chunked response
#endif
    });
}

void loop()
{
#if !defined(ESP32) || defined(NO_WIFI_TASK)
    _connector->Loop();
#endif
}
