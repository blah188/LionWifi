// LionWifi — SD-card example (ESP8266 / ESP32)
//
// Adds an SD-card browser (/sd/ls, /sd/tail/<f>, /sd/download/<f>, /sd/del/<f>)
// on top of the internal-filesystem browser. SD listing is flat (root, no
// subfolder navigation). Works
// with either SD backend:
//   * Arduino SD library   — default (build with -D USE_SD_CARD)
//   * SdFat (Bill Greiman)  — build with -D USE_SD_CARD -D SDFAT (add greiman/SdFat
//                             to lib_deps). FsBrowser then declares `extern SdFat SD;`
//                             which THIS sketch defines below. The default SdFat
//                             config (FsFile, FAT+exFAT) is what LionWifi expects.
//                             For non-ASCII names (Cyrillic, accents) in listings,
//                             build SdFat with -D USE_UTF8_LONG_NAMES=1 (otherwise
//                             it renders them as '?').
//
// IMPORTANT: LionLogger does NOT initialize the SD card (it only uses the
// internal SPIFFS/LittleFS). You must call SD.begin() yourself — done in setup().
//
// Build flags (see platformio.ini): an internal FS (-D USE_SPIFFS or -D LFS),
// -D USE_SD_CARD [ -D SDFAT ], and on ESP32 -D NO_LOG_CLEARING (LionLogger needs
// ASYNC_LOG to clear logs on ESP32). Override the SD chip-select with -D SD_CS_PIN.

#include <Logger.h>
#include <WifiConnector.h>

#ifdef SDFAT
SdFat SD; // FsBrowser declares `extern SdFat SD;` for the SdFat backend — define it here.
#endif

// SD chip-select pin (override with -D SD_CS_PIN=...).
#ifndef SD_CS_PIN
#ifdef ESP32
#define SD_CS_PIN 5
#else
#define SD_CS_PIN 15 // D8 on a Wemos D1 mini
#endif
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

void setup()
{
    Serial.begin(115200);
    Logger.Setup(); // mounts the INTERNAL FS (SPIFFS/LittleFS) — NOT the SD card

    // Initialize the SD card ourselves (LionLogger no longer does this).
#ifdef SDFAT
    // SdFat's begin(csPin) uses an aggressive default clock; give it an explicit
    // config at a conservative 20 MHz (override with -D SD_SCK_MHZ_VAL=N).
#ifndef SD_SCK_MHZ_VAL
#define SD_SCK_MHZ_VAL 10
#endif
    bool sdOk = SD.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(SD_SCK_MHZ_VAL)));
#else
    bool sdOk = SD.begin(SD_CS_PIN);
#endif
    if (!sdOk)
        Logger.Log_P(ILogger::LvlError, PSTR("SD init failed (CS=%d) — /sd/* will be empty"), SD_CS_PIN);
    else
        Logger.Log_P(ILogger::LvlInfo, PSTR("SD card ready (CS=%d)"), SD_CS_PIN);

    _connector = new WifiConnector(WIFI_SSID, WIFI_PASSWORD);
    _connector->Setup(); // registers both /spiffs/ls and /sd/ls routes
}

void loop()
{
#if !defined(ESP32) || defined(NO_WIFI_TASK)
    _connector->Loop();
#endif
}
