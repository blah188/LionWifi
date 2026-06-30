// LionWifi — basic example (ESP8266 / ESP32)
//
// Brings up WiFi, ArduinoOTA and the web file browser. Set your SSID/password
// (here or via -D WIFI_SSID / -D WIFI_PASSWORD in platformio.ini), build &
// upload, then open http://<device-ip>/ in a browser:
//   * /            — home page (upload data/index_nosd.html to the FS first,
//                    or just go straight to /spiffs/ls)
//   * /spiffs/ls   — list / upload / download / tail / delete files
//   * /log         — the device log; /restart — reboot
//
// Required build flags (see platformio.ini): a filesystem (-D USE_SPIFFS) and,
// on ESP32, -D NO_ASYNC_WEB_SERVER so the core WebServer is used (no extra
// libs). Drop NO_ASYNC_WEB_SERVER to use the async server — then add
// ESPAsyncWebServer + AsyncTCP to your lib_deps.

#include <Logger.h>
#include <WifiConnector.h>

// WiFi credentials — override from platformio.ini, e.g.:
//   build_flags = -D WIFI_SSID=\"MyNet\" -D WIFI_PASSWORD=\"secret\"
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

void setup()
{
    Serial.begin(115200);
    Logger.Setup(); // mounts the filesystem and starts logging

    _connector = new WifiConnector(WIFI_SSID, WIFI_PASSWORD);

    _connector->RegisterConnectedEvent([]()
    {
        Logger.Log_P(ILogger::LvlInfo, PSTR("WiFi up — open http://%s/"), WiFi.localIP().toString().c_str());
    });

    _connector->Setup();
}

void loop()
{
#if !defined(ESP32) || defined(NO_WIFI_TASK)
    // On ESP8266 (and ESP32 with NO_WIFI_TASK) you pump the connector yourself.
    // On the default ESP32 build it runs in its own FreeRTOS task, so loop() is free.
    _connector->Loop();
#endif
}
