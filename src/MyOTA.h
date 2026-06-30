#pragma once

// =============================================================================
// MyOta — thin ArduinoOTA wrapper used by LionWifi: wires up start/end/progress/
// error callbacks (logging through the global LionLogger `Logger`), unmounts/
// remounts the filesystem around a filesystem OTA, and logs progress every 25%.
//
// Internal LionWifi header: it uses LIONWIFI_FS (the selected filesystem), which
// FsBrowser.h defines — so FsBrowser.h must be included before this header
// (WifiConnector.h does so).
//
// Single-instance, non-copyable: the constructor registers ArduinoOTA callbacks
// that capture `this`, so a copy would leave dangling captures. Call Begin()
// once WiFi is connected, then Loop() each iteration. LionWifi owns one of these.
// =============================================================================

#include <ArduinoOTA.h>
#include <Logger.h>

#ifndef LIONWIFI_FS
#error "Include FsBrowser.h before MyOTA.h (it selects the filesystem / defines LIONWIFI_FS)."
#endif

class MyOta
{
private:
    int _otaPercent = -1;
    bool _sketchUpload = false;

public:
    MyOta()
    {
        ArduinoOTA.onStart([this]()
        {
            _sketchUpload = ArduinoOTA.getCommand() == U_FLASH;
            Logger.Log_P(ILogger::LvlInfo, PSTR("OTA: Start updating %S"), _sketchUpload ? F("sketch") : F("filesystem"));
            if (!_sketchUpload) // unmount FS so the new FS image can be written
                LIONWIFI_FS.end();
            _otaPercent = 0;
        });
        ArduinoOTA.onEnd([this]()
        {
            Logger.Log_P(ILogger::LvlInfo, PSTR("OTA: End"));
            if (!_sketchUpload)
                LIONWIFI_FS.begin();
        });
        ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total)
        {
            if (!total)
                return;
            // Integer-safe percent: avoids divide-by-zero when total < 100
            // (small images / an early callback before the size is known).
            int p = (int)((uint64_t)progress * 100 / total);
            if (p % 25 == 0 && p != _otaPercent)
            {
                _otaPercent = p;
                Logger.Log_P(ILogger::LvlDebug, PSTR("OTA: %d%%"), p);
            }
        });
        ArduinoOTA.onError([this](ota_error_t error)
        {
            Logger.Log_P(ILogger::LvlError, PSTR("Error[%u]: "), error);
            if (error == OTA_AUTH_ERROR)
                Logger.Log_P(ILogger::LvlError, PSTR("Auth Failed"));
            else if (error == OTA_BEGIN_ERROR)
                Logger.Log_P(ILogger::LvlError, PSTR("Begin Failed"));
            else if (error == OTA_CONNECT_ERROR)
                Logger.Log_P(ILogger::LvlError, PSTR("Connect Failed"));
            else if (error == OTA_RECEIVE_ERROR)
                Logger.Log_P(ILogger::LvlError, PSTR("Receive Failed"));
            else if (error == OTA_END_ERROR)
                Logger.Log_P(ILogger::LvlError, PSTR("End Failed"));
        });
    }

    // Registers ArduinoOTA callbacks capturing `this`; copying would dangle them.
    MyOta(const MyOta &) = delete;
    MyOta &operator=(const MyOta &) = delete;

    // Call after WIFI connected
    void Begin()
    {
        ArduinoOTA.begin();    
#ifdef ESP32        
        ArduinoOTA.setTimeout(40000);
#endif        
    }

    void Loop()
    {
        ArduinoOTA.handle();
    }
};