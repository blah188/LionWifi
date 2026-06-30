#pragma once

#include <ArduinoOTA.h>
#include <Logger.h>

class MyOta
{
private:
    ILogger *_logger = nullptr;
    int _otaPercent = -1;
    bool _sketchUpload = false;

public:
    MyOta(ILogger *logger)
    {
        _logger = logger;

        ArduinoOTA.onStart([this]() 
        {
            _sketchUpload = ArduinoOTA.getCommand() == U_FLASH;
            // NOTE: if updating FS this would be the place to unmount FS using FS.end()
            if (_logger)
                _logger->Log_P(ILogger::LvlInfo, PSTR("OTA: Start updating %S"), _sketchUpload?F("sketch"):F("filesystem"));
            if (!_sketchUpload)
            {
#ifdef LFS
                LittleFS.end();
#endif                
#ifdef USE_SPIFFS
                SPIFFS.end();
#endif                
            }
            _otaPercent = 0;
        });
        ArduinoOTA.onEnd([this]() 
        {
            if (_logger)
                _logger->Log_P(ILogger::LvlInfo, PSTR("OTA: End"));
            if (!_sketchUpload)
            {
#ifdef LFS
                LittleFS.begin();
#endif                
#ifdef USE_SPIFFS
                SPIFFS.begin();
#endif                
            }
        });
        ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) 
        {
            if (_logger && total)
            {
                // Integer-safe percent: avoids divide-by-zero when total < 100
                // (small images / an early callback before the size is known).
                int p = (int)((uint64_t)progress * 100 / total);
                if (p % 25 == 0 && p != _otaPercent)
                {
                    _otaPercent = p;
                    _logger->Log_P(ILogger::LvlDebug, PSTR("OTA: %d%%"), p);
                }
            }
        });
        ArduinoOTA.onError([this](ota_error_t error) 
        {
            if (!_logger)
                return;
            _logger->Log_P(ILogger::LvlError, PSTR("Error[%u]: "), error);
            if (error == OTA_AUTH_ERROR) 
                _logger->Log_P(ILogger::LvlError, PSTR("Auth Failed"));
            else if (error == OTA_BEGIN_ERROR)
                _logger->Log_P(ILogger::LvlError, PSTR("Begin Failed"));
            else if (error == OTA_CONNECT_ERROR)
                _logger->Log_P(ILogger::LvlError, PSTR("Connect Failed"));
            else if (error == OTA_RECEIVE_ERROR) 
                _logger->Log_P(ILogger::LvlError, PSTR("Receive Failed"));
            else if (error == OTA_END_ERROR)
                _logger->Log_P(ILogger::LvlError, PSTR("End Failed"));
        });
    }

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

//extern MyOta *myOta;