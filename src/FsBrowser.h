#pragma once

//#define ESP32
//#define USE_SD
//#define SDFAT

#ifdef ESP32
#include "soc/rtc_wdt.h"
//#include <AsyncTCP.h>
#ifdef NO_ASYNC_WEB_SERVER
#include <WebServer.h>
#else
//#include <ESPAsyncWebSrv.h>
#include <ESPAsyncWebServer.h>
#endif
#else
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#endif

#ifdef LFS
#ifdef ESP32
#include <LittleFS.h>
//#include <esp_littlefs.h>
//#define LittleFS LITTLEFS
#else
#include <LittleFS.h>
#endif
#endif
#ifdef USE_SPIFFS
#ifdef ESP32
#include <SPIFFS.h>
#else
#include <FS.h>
#endif
#endif
#ifdef USE_SD_CARD
#ifdef SDFAT
#include <SdFat.h>
extern SdFat SD;
#define SD_FILE_READ O_RDONLY
#define SD_FILE_WRITE (O_RDWR | O_CREAT | O_AT_END)
#define MySdFile File32
#else
#include <SPI.h>
#include <SD.h>
#define SD_FILE_READ "r"
#define SD_FILE_WRITE "w"
#define MySdFile File
#endif
#endif

#include <Array.h>
#include <Logger.h>
#include <StringStream.h>

#include "DirEntry.h"

extern const char __text_plain__P[];
#define __text_plain__F FPSTR(__text_plain__P)
extern const char __text_html__P[];
#define __text_html__F FPSTR(__text_html__P)
extern const char __slash_favicon_ico__P[];
#define __slash_favicon_ico__F FPSTR(__slash_favicon_ico__P)

struct FileSystemStats
{
    uint64_t totalSize, freeSize;
};

int sort(const void *cmp1, const void *cmp2);
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
void DebugDumpRequest(AsyncWebServerRequest *request);
#endif

class FsBrowser
{
    const uint16_t TailSize = 8192;

private:
#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
    WebServer &_server;
#else
    AsyncWebServer &_server;
#endif
#else
    ESP8266WebServer &_server;
#endif
    bool _doAuth = false;
    const char *_username, *_password;
    ILogger *_logger;
#if defined(USE_SD_CARD) && defined(SDFAT)  
    File32 _fsSdUploadFile;
#endif
    File _fsUploadFile;

public:
#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
    FsBrowser(ILogger *logger, WebServer &server, const char *username, const char *password) : _server(server)
#else
    FsBrowser(ILogger *logger, AsyncWebServer &server, const char *username, const char *password) : _server(server)
#endif
#else
    FsBrowser(ILogger *logger, ESP8266WebServer &server, const char *username, const char *password) : _server(server)
#endif
    {
        _logger = logger;
        if (username && password)
        {
            _username = username;
            _password = password;
            _doAuth = true;
        }
    }

    static String GetContentType(const String &filename)
    { // convert the file extension to the MIME type
        if (filename.endsWith(".html") || filename.endsWith(".htm"))
            return __text_html__F;
        else if (filename.endsWith(".css"))
            return F("text/css");
        else if (filename.endsWith(".js"))
            return F("application/javascript");
        else if (filename.endsWith(".ico"))
            return F("image/x-icon");
        else if (filename.endsWith(".ico"))
            return F("image/png");
        else if (filename.endsWith(".jpg"))
            return F("image/jpeg");
        else if (filename.endsWith(".gz"))
            return F("application/x-gzip");
        return __text_plain__F;
    }

    static String fileSize(uint64_t size)
    {
        if (size < 100000)
            return String((uint32_t)size);
        if (size < 100 * 1024 * 1024l)
            return String((uint32_t)(size / 1024l)) + F("k");
        return String((uint32_t)(size / 1024l / 1024l)) + F("m");
    }

    static void fileSize(uint64_t size, Stream &out)
    {
        if (size < 100000)
            out.print((uint32_t)size);
        else if (size < 100 * 1024 * 1024l)
            out.printf_P(PSTR("%ldk"), (uint32_t)(size / 1024l));
        else
            out.printf_P(PSTR("%ldm"), (uint32_t)(size / 1024l / 1024l));
    }

    static String fileTime(time_t unix)
    {
#ifdef USE_FILE_TIME
        static char buffer[64];
        buffer[63] = 0;
        struct tm *timeinfo;
        timeinfo = localtime(&unix);
        snprintf_P(buffer, 63, PSTR("%02d/%02d/%04d<br>%02d:%02d:%02d"), timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        return String(buffer);
        // static char buffer[64];
        // buffer[63] = 0;
        // TimeElements tm;
        // breakTime(unix, tm);
        // snprintf_P(buffer, 63, PSTR("%02d/%02d/%04d<br>%02d:%02d:%02d"), tm.Month, tm.Day, tm.Year + 1970, tm.Hour, tm.Minute, tm.Second);
        // return String(buffer);
#else
        return String("Unknown");
#endif
    }

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    bool DoAuth(AsyncWebServerRequest *request)
    {
#ifdef NO_AUTH
        return true; // Authentication disabled at build time — plain access.
#endif
        if (_doAuth && !request->authenticate(_username, _password)) // TODO: move to hash?
        {
            request->requestAuthentication();
            return false;
        }
        return true;
    }
#else
    bool DoAuth()
    {
#ifdef NO_AUTH
        return true; // Authentication disabled at build time — plain access.
#endif
        if (_doAuth && !_server.authenticate(_username, _password)) // TODO: move to hash?
        {
            _server.requestAuthentication();
            if (_logger)
                _logger->Log_P(ILogger::LvlWarning, PSTR("Need auth for %d.%d.%d.%d: %s"), _server.client().remoteIP()[0], _server.client().remoteIP()[1], _server.client().remoteIP()[2], _server.client().remoteIP()[3], _server.uri().c_str());
            return false;
        }
        return true;
    }
#endif

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    bool SendFileResponse(const char *name, AsyncWebServerRequest *request, bool sd, bool tail, bool forceDownload = false)
    {
        bool exists = false;
        if (!Logger.TryLockFsSemaphore())
        {
            request->send(429, __text_plain__F, "Server Busy");
            return false;
        }
#ifdef USE_SD_CARD
        if (sd) {
            String sdPath = name;
            if (sdPath.length() > 0 && sdPath[0] != '/') {
                sdPath = "/" + sdPath;
            }
            exists = SD.exists(sdPath.c_str());
        }
#endif
#ifdef LFS
        if (!sd)
            exists = LittleFS.exists(name);
#endif
#ifdef USE_SPIFFS
        if (!sd)
            exists = SPIFFS.exists(name);
#endif
        if (!exists)
        {
            Logger.UnlockFsSemaphore();
            if (_logger)
                _logger->Log_P(ILogger::LvlWarning, PSTR("sendFileResponse: %s file %s not found"), (sd ? F("SD") : F("SP")), name);
            request->send(404, __text_plain__F, String("Can't find ") + (sd ? F("SD") : F("SP")) + F(" file ") + name);
            return false;
        }
#if defined(USE_SD_CARD) && defined(SDFAT)
        File32 sdDataFile;
#endif
        File dataFile;
        if (tail) // Only tail
        {
            char *buf = new char[TailSize + 1];
#ifdef USE_SD_CARD
            if (sd)
            {
                String sdPath = name;
                if (sdPath.length() > 0 && sdPath[0] != '/') {
                    sdPath = "/" + sdPath;
                }
#ifdef SDFAT
                sdDataFile = SD.open(sdPath.c_str(), SD_FILE_READ);
#else
                dataFile = SD.open(sdPath.c_str(), SD_FILE_READ);
#endif
            }
            else
#endif
#ifdef LFS
                dataFile = LittleFS.open(name, "r");
#endif
#ifdef USE_SPIFFS
            dataFile = SPIFFS.open(name, "r");
#endif

#if defined(USE_SD_CARD) && defined(SDFAT)
            if (sdDataFile.size() > TailSize)
                sdDataFile.seek(sdDataFile.size() - TailSize);
            size_t read = sdDataFile.readBytes(buf, TailSize);
            sdDataFile.close();
#else
            if (dataFile.size() > TailSize)
                dataFile.seek(dataFile.size() - TailSize);
            size_t read = dataFile.readBytes(buf, TailSize);
            if (_logger)
                _logger->Log_P(ILogger::LvlDebug, PSTR("sendFileResponse: sending %d tail bytes (from pos %d) of file %s"), read, dataFile.size() - TailSize, name);
            dataFile.close();
#endif
            buf[read] = 0;
            request->send(200, __text_plain__F, buf);
            delete[] buf;
            Logger.UnlockFsSemaphore();
            return true;
        }

#ifdef USE_SD_CARD
        if (sd)
        {
#ifdef SDFAT
            sdDataFile = SD.open(name, SD_FILE_READ);
#else
            //dataFile = SD.open(name, SD_FILE_READ);
            String sdPath = name;
            if (sdPath.length() > 0 && sdPath[0] != '/') {
                sdPath = "/" + sdPath;
            }
            AsyncWebServerResponse *response = request->beginResponse(SD, sdPath.c_str(), GetContentType(name), forceDownload);
            request->send(response);
#endif
        }
        else
        {
#endif

#ifdef LFS
            AsyncWebServerResponse *response = request->beginResponse(LittleFS, name, GetContentType(name), forceDownload);
            request->send(response);
#endif
#ifdef USE_SPIFFS
            AsyncWebServerResponse *response = request->beginResponse(SPIFFS, name, GetContentType(name), forceDownload);
            request->send(response);
#endif
#ifdef USE_SD_CARD
        }
#endif
        Logger.UnlockFsSemaphore();

        if (_logger)
            _logger->Log_P(ILogger::LvlDebug, PSTR("sendFileResponse: %s(%s)"), name, sd ? "Sd" : "SP");
        return true;
    }
#else
    bool SendFileResponse(const char *name, bool sd, bool tail, bool forceDownload = false)
    {
        File dataFile;

#ifdef USE_SD_CARD
        if (sd)
            dataFile = SD.open(name, SD_FILE_READ);
        else
#endif
#ifdef LFS
            dataFile = LittleFS.open(name, "r");
#endif
#ifdef USE_SPIFFS
        dataFile = SPIFFS.open(name, "r");
#endif

        if (!dataFile)
        {
            if (_logger)
                _logger->Log_P(ILogger::LvlWarning, PSTR("sendFileResponse: %s file %s not found"), (sd ? F("SD") : F("SP")), name);
            _server.send(404, __text_plain__F, String("Can't find ") + (sd ? F("SD") : F("SP")) + F(" file ") + name);
            return false;
        }
        int size = dataFile.size();
        if (tail) // Only tail
        {
            if (size > TailSize)
            {
                dataFile.seek(dataFile.size() - TailSize);
                size = TailSize;
            }
        }
        if (forceDownload)
        {
// #ifndef LFS            
//             if (!sd)
//                 name = String(name).substring(1).c_str();
// #endif
            _server.sendHeader(F("Content-Disposition"), String(F("attachment; filename=")) + (name[0] == '/'?name+1:name));
            _server.sendHeader(F("Content-Transfer-Encoding"), F("binary"));
            _server.sendHeader(F("Expires"), F("0"));
            _server.sendHeader(F("Cache-Control"), F("must-revalidate, post-check=0, pre-check=0"));
            _server.sendHeader(F("Pragma"), F("public"));
        }
        size_t sent = _server.streamFile(dataFile, GetContentType(name));
        dataFile.close();
#ifndef LOG_FAVICON
        if (_logger && strcasecmp_P(name, __slash_favicon_ico__P) != 0 && strcasecmp_P(name, __slash_favicon_ico__P) != 0)
#endif        
            _logger->Log_P(ILogger::LvlDebug, PSTR("Sent %s(%s), %u bytes"), name, sd ? "Sd" : "SP", sent);
        return true;
    }
#endif

#ifdef ESP32
#else
#endif

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    bool HandleFileRead(String path, AsyncWebServerRequest *request, bool auth = true) // send the right file to the client (if it exists)
    {
        if (auth && !DoAuth(request))
            return false;
#else
    bool HandleFileRead(const String &path, bool auth = true) // send the right file to the client (if it exists)
    {
        if (auth && !DoAuth())
            return false;
#endif

        // if (_logger && !path.equalsIgnoreCase(__slash_favicon_ico__F))
        //     _logger->Log_P(ILogger::LvlDebug, PSTR("handleFileRead: %s"), path.c_str());
        if (path.startsWith("/del"))
        {
            String name = path.substring(4);
            Logger.Log_P(ILogger::LvlDebug, PSTR("about to delete %s"), name.c_str());

#ifdef LFS
            if (LittleFS.exists(name))
                LittleFS.remove(name);
            else
                return false;
#endif
#ifdef USE_SPIFFS
            bool del = false;
            if (Logger.TryLockFsSemaphore())
            {
                if (SPIFFS.exists(name))
                {
                    SPIFFS.remove(name);
                    del = true;
                }
                Logger.UnlockFsSemaphore();
            }
            if (!del)
                return false;
#endif
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
            request->redirect(F("/spiffs/ls"));
#else
            _server.sendHeader(F("Location"), F("/spiffs/ls")); // Redirect the client to the success page
            _server.send(303);
#endif
            return true;
        }

#ifdef USE_SD_CARD
        if (path.startsWith("/sd/del/"))
        {
            String name = path.substring(8);
            String sdPath = name;
            if (sdPath.length() > 0 && sdPath[0] != '/') {
                sdPath = "/" + sdPath;
            }
            if (SD.exists(sdPath.c_str()))
                SD.remove(sdPath.c_str());
            else
                return false;
#ifdef ESP32
            int pos = name.lastIndexOf('/');
            String folder;
            if (pos >= 0)
                folder = name.substring(0, pos+1);
            request->redirect(String("/sd/ls?folder=") + folder);
#else
            _server.sendHeader("Location", "/sd/ls"); // Redirect the client to the success page
            _server.send(303);
#endif
            return true;
        }
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        if (path.startsWith("/sd/tail"))
            return SendFileResponse(path.substring(9).c_str(), request, true, true);
        if (path.startsWith("/sd/download"))
            return SendFileResponse(path.substring(12).c_str(), request, true, true, true);
        if (path.startsWith("/sd"))
            return SendFileResponse(path.substring(3).c_str(), request, true, false);
#else
        if (path.startsWith("/sd/tail"))
            return SendFileResponse(path.substring(9).c_str(), true, true);
        if (path.startsWith("/sd/download"))
            return SendFileResponse(path.substring(12).c_str(), true, true, true);
        if (path.startsWith("/sd"))
            return SendFileResponse(path.substring(3).c_str(), true, false);
#endif
#endif

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        if (path.startsWith("/spiffs"))
            return SendFileResponse(path.substring(7).c_str(), request, false, false);
        if (path.startsWith("/tail"))
            return SendFileResponse(path.substring(5).c_str(), request, false, true);
        if (path.startsWith("/download"))
            return SendFileResponse(path.substring(9).c_str(), request, false, false, true);
        if (path.endsWith("/"))
            path += "index.html"; // If a folder is requested, send the index file
        return SendFileResponse(path.c_str(), request, false, false);
#else
        if (path.startsWith("/spiffs"))
            return SendFileResponse(path.substring(7).c_str(), false, false);
        if (path.startsWith("/tail"))
            return SendFileResponse(path.substring(5).c_str(), false, true);
        if (path.startsWith("/download"))
            return SendFileResponse(path.substring(9).c_str(), false, false, true);
        if (path.endsWith("/"))
        {
            SendFileResponse((path + F("index.html")).c_str(), false, false);// If a folder is requested, send the index file
            return true;
        }
        return SendFileResponse(path.c_str(), false, false);
#endif
        return true;
    }

    void ListSpiffsFiles(Array<DirEntry> &files)
    {
        if (!Logger.TryLockFsSemaphore())
            return;
#ifdef LFS
#ifdef ESP32
        File dir = LittleFS.open("/");
#else
        Dir dir = LittleFS.openDir("/");
#endif
#endif
#ifdef USE_SPIFFS
#ifdef ESP32
        File dir = SPIFFS.open("/");
#else
        Dir dir = SPIFFS.openDir("/");
#endif
#endif
#ifdef ESP32
        File file;
        while (file = dir.openNextFile())
#else
        while (dir.next())
#endif
        {
#ifdef ESP32
            rtc_wdt_feed();
            //Logger.Log_P(ILogger::LvlDebug, PSTR("Got %s, %ld bytes, %ld time"), file.name(), file.size(), file.getLastWrite());
            DirEntry entry(file.name());
            entry.size = file.size();
#ifdef USE_FILE_TIME
            entry.creationTime = entry.writeTime = file.getLastWrite();
#endif
#else
            DirEntry entry(dir.fileName().c_str());
            entry.size = dir.fileSize();
#ifdef USE_FILE_TIME
            entry.creationTime = dir.fileCreationTime();
            entry.writeTime = dir.fileTime();
#endif
#endif
            files += entry;
        }
        Logger.Log_P(ILogger::LvlDebug, PSTR("Listed %d files from %S"), files.Length(),
#ifdef LFS
                     PSTR("LFS")
#endif
#ifdef USE_SPIFFS
                         PSTR("SpifFS")
#endif
        );
        Logger.UnlockFsSemaphore();
    }

    void ListSdFiles(Array<DirEntry> &files, const char *folder)
    {
#ifdef USE_SD_CARD
#ifdef SDFAT
        Logger.Log_P(ILogger::LvlDebug, PSTR("Using SDFAT"));
        SdFile dir, file;
        if (dir.open(folder[0] ? folder : "/"))
        {
            while (file.openNext(&dir, O_RDONLY))
            {
                char tmp[64];
                tmp[63] = 0;
                file.getName(tmp, 63);
                DirEntry entry(tmp);
                entry.size = file.fileSize();
                entry.isFolder = file.isDir();
#ifdef USE_FILE_TIME
                //entry.creationTime = file.getCreateDateTime();
                //entry.writeTime = file.getModifyDateTime();
#endif
                files += entry;
                file.close();
            }
            dir.close();
        }
#else
        File root = SD.open("/");
        File file;
        while (file = root.openNextFile())
        {
#ifdef ESP32
            rtc_wdt_feed();
            //Logger.Log_P(ILogger::LvlDebug, PSTR("Got %s, %ld bytes, %ld time"), file.name(), file.size(), file.getLastWrite());
            DirEntry entry(file.name());
            entry.size = file.size();
#ifdef USE_FILE_TIME
            entry.creationTime = entry.writeTime = file.getLastWrite();
#endif
                files += entry;
#else
            //Logger.Log_P(ILogger::LvlDebug, PSTR("Got %s, %ld bytes, %ld time"), file.fullName(), file.size(), file.getLastWrite());
            if (files.Length() >= 120)
            {
                DirEntry entry("zzz.zzz"); // TODO: PSTR
                entry.size = 0;
                files += entry;
            }
            else
            {
                DirEntry entry(file.fullName());
                entry.size = file.size();
#ifdef USE_FILE_TIME
                entry.creationTime = file.getCreationTime();
                entry.writeTime = file.getLastWrite();
#endif
                files += entry;
            }
#endif
            file.close();
            if (files.Length() >= 121)
                break;
        }
        root.close();
#endif
#endif
        Logger.Log_P(ILogger::LvlDebug, PSTR("Listed %d files from SD"), files.Length());
    }

    FileSystemStats GetFsStats(bool sd)
    {
        FileSystemStats stats;
        if (sd)
        {
#ifdef USE_SD_CARD
#ifdef ESP32
#ifdef SDFAT
            stats.totalSize = SD.clusterCount() * (uint64_t)SD.bytesPerCluster();
            stats.freeSize = SD.freeClusterCount() * (uint64_t)SD.bytesPerCluster();
#else
            stats.freeSize = SD.totalBytes() - SD.usedBytes();
            stats.totalSize = SD.totalBytes();
#endif
#else
            // Seems it crashes on ESP8266
#ifdef AVR
            FSInfo64 info;
            SDFS.info64(info);
            stats.totalSize = info.totalBytes;
            stats.freeSize = info.totalBytes - info.usedBytes;
#else
            stats.totalSize = stats.freeSize = 0;
#endif            
#endif
#endif
        }
        else
        {
#ifdef ESP32
            if (Logger.TryLockFsSemaphore())
            {
#ifdef LFS
                stats.freeSize = LittleFS.totalBytes() - LittleFS.usedBytes();
                stats.totalSize = LittleFS.totalBytes();
#endif
#ifdef USE_SPIFFS
                stats.freeSize = SPIFFS.totalBytes() - SPIFFS.usedBytes();
                stats.totalSize = SPIFFS.totalBytes();
#endif
                Logger.UnlockFsSemaphore();
            }
#else
#ifdef LFS
            FSInfo fs_info;
            LittleFS.info(fs_info);
#endif
#ifdef USE_SPIFFS
            FSInfo fs_info;
            SPIFFS.info(fs_info);
#endif
            stats.freeSize = fs_info.totalBytes - fs_info.usedBytes;
            stats.totalSize = fs_info.totalBytes;
#endif
        }
        return stats;
    }

#ifdef CONTINGOUS_LS
    void HandleLs(bool sd)
    {
        Logger.Log_P(ILogger::LvlDebug, PSTR("Got LS request(Cont) free mem = %ld"), system_get_free_heap_size());
        if (!DoAuth())
            return;

        String buffer;
        buffer.reserve(512);
        StringStream lsFile(buffer);
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        lsFile.printf_P(PSTR("<html> <body> <form method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"> <input class=\"button\" type=\"submit\" value=\"Upload\"></form>"));
        lsFile.printf_P(PSTR("<table><tr><td><b>Name</b></td></td><td><b>Size</b></td>"));
#ifdef USE_FILE_TIME
        lsFile.printf_P(PSTR("<td><b>Created</b></td><td><b>Modified</b></td>"));
#endif
        lsFile.printf_P(PSTR("<td><b>Action</b></td></tr>\n"));
        _server.send_P(200, __text_html__P, PSTR(""));
        _server.sendContent(buffer);
        lsFile.reset();

#ifdef LFS
        Dir dir = LittleFS.openDir("/");
#endif
#ifdef USE_SPIFFS
        Dir dir = SPIFFS.openDir("/");
#endif
        while (dir.next())
        {
            lsFile.printf_P(PSTR("<tr> <td> <a href=\"%s\">%s</a></td><td>"),
                           dir.fileName().c_str()+1, dir.fileName().c_str()+1);
            fileSize(dir.fileSize(), lsFile);
            lsFile.printf_P(PSTR("</td>"));
#ifdef USE_FILE_TIME
            lsFile.printf_P(PSTR("<td><font size=\"-1\">%s</font></td><td><font size=\"-1\">%s</font></td>"),
                            fileTime(entry.creationTime).c_str(), fileTime(entry.writeTime).c_str());
#endif
            lsFile.printf_P(PSTR("<td><a href=\"/tail/%s\">Tail</a>&nbsp<a href=\"/download/%s\">Download</a>&nbsp<a href=\"/del/%s\""),
                                    dir.fileName().c_str()+1, dir.fileName().c_str()+1, dir.fileName().c_str()+1);
            lsFile.printf_P(PSTR(" onclick=\"return confirm('Are you sure to delete %s?')\">DEL</a></td></tr>"), dir.fileName().c_str()+1);
            _server.sendContent(buffer);
            lsFile.reset();

        }
//        Logger.Log_P(ILogger::LvlDebug, PSTR("Getting FS stats"));
        FileSystemStats stats = GetFsStats(sd);
//        Logger.Log_P(ILogger::LvlDebug, PSTR("Got FS stats: %lu, %lu"), (unsigned long)stats.totalSize, (unsigned long)stats.fullSize);
        lsFile.printf_P(PSTR("<tr><td>Total:</td><td>"));
        fileSize(stats.totalSize, lsFile);
        lsFile.printf_P(PSTR("</td><td></td></tr>\n<tr><td>Free:</td><td>"));
        fileSize(stats.freeSize, lsFile);
        lsFile.printf_P(PSTR("</td><td></td></tr>\n</table> <a href=\"/\">Home</a></body>"));
        lsFile.printf_P(PSTR("</html>"));
        _server.sendContent(buffer);
        _server.chunkedResponseFinalize();
        lsFile.reset();
        Logger.Log_P(ILogger::LvlDebug, PSTR("Fully sent LS content, free mem = %ld"), system_get_free_heap_size());
    }

#else
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    void HandleLs(AsyncWebServerRequest *request, bool sd)
    {
        Logger.Log_P(ILogger::LvlDebug, PSTR("Got LS requesd, SD = %d, free mem = %ld"), (int)sd,  esp_get_free_heap_size());
        String folder;
        if (!DoAuth(request))
            return;
        for (int i = 0; i < request->params(); i++)
        {
            auto p = request->getParam(i);
            if (p->name() == "folder")
                folder = p->value();
        }
#else
    void HandleLs(bool sd)
    {
#if defined(ESP32)
        Logger.Log_P(ILogger::LvlDebug, PSTR("Got LS request, SD = %d, free mem = %ld"), (int)sd,  esp_get_free_heap_size());
#else        
        Logger.Log_P(ILogger::LvlDebug, PSTR("Got LS request, SD = %d, free mem = %ld"), (int)sd,  system_get_free_heap_size());
#endif        
        //String folder;
        if (!DoAuth())
            return;
#endif
        Array<DirEntry> files;
        if (sd)
            ListSdFiles(files, ""/*folder.c_str()*/);
        else
            ListSpiffsFiles(files);

        qsort(files.GetData(), files.Length(), sizeof(DirEntry), sort);

        String buffer;
        StringStream lsFile(buffer);
#ifndef ESP32
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
#else        
        rtc_wdt_feed();
#endif
        lsFile.printf_P(PSTR("<html> <body> <form method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"> <input class=\"button\" type=\"submit\" value=\"Upload\"></form>"));
#if defined(USE_SD_CARD) && defined (SDFAT)
        if (sd)
            lsFile.printf_P(PSTR("<form><label for=\"dir\">Create folder:</label><input type=\"text\" id=\"dir\"><input type=\"button\" value=\"Create\" onclick=\"mkdir()\"></form>"));
#endif
        // if (folder != "")
        //     lsFile.printf_P(PSTR("<div> Contents of '%s' </div>"), folder.c_str());
        lsFile.printf_P(PSTR("<table><tr><td><b>Name</b></td></td><td><b>Size</b></td>"));
#ifdef USE_FILE_TIME
        lsFile.printf_P(PSTR("<td><b>Created</b></td><td><b>Modified</b></td>"));
#endif
        lsFile.printf_P(PSTR("<td><b>Action</b></td></tr>\n"));

#ifndef ESP32
        _server.send_P(200, __text_html__P, PSTR(""));
        _server.sendContent(buffer);
        lsFile.reset();
#endif
        //Serial.println("1");

        for (int i = 0; i < files.Length(); i++)
        {
            DirEntry entry = files[i];
            //String fullPath = (folder + entry.fullName);

//            Logger.Log_P(ILogger::LvlDebug, PSTR("Entry #%d/%d[free mem %ld]: %s"), i+1, files.Length(), system_get_free_heap_size(), entry.fullName);

            if (entry.isFolder)
                lsFile.printf_P(PSTR("<tr> <td> <a href=\"/sd/ls?folder=%s/\">%s</a></td><td>%s</td><td>DIR</td>"),
                                entry.fullName, entry.fullName, fileSize(entry.size).c_str());
            else
                lsFile.printf_P(PSTR("<tr> <td> <a href=\"%s\">%s</a></td><td>%s</td>"),
                                entry.fullName, entry.fullName, fileSize(entry.size).c_str());
#ifdef USE_FILE_TIME
            lsFile.printf_P(PSTR("<td><font size=\"-1\">%s</font></td><td><font size=\"-1\">%s</font></td>"),
                            fileTime(entry.creationTime).c_str(), fileTime(entry.writeTime).c_str());
#endif
            if (entry.isFolder)
                lsFile.printf_P(PSTR("<td></td></tr>"));
            else
            {
                if (sd)
                {
                    lsFile.printf_P(PSTR("<td><a href=\"/sd/tail/%s\">Tail</a>&nbsp<a href=\"/sd/download/%s\">Download</a>&nbsp<a href=\"/sd/del/%s\""),
                                    entry.fullName, entry.fullName, entry.fullName);
                }
                else
                    lsFile.printf_P(PSTR("<td><a href=\"/tail/%s\">Tail</a>&nbsp<a href=\"/download/%s\">Download</a>&nbsp<a href=\"/del/%s\""),
                                    entry.fullName, entry.fullName, entry.fullName);
                lsFile.printf_P(PSTR(" onclick=\"return confirm('Are you sure to delete %s?')\">DEL</a></td></tr>"), entry.fullName);
            }
#ifndef ESP32
            _server.sendContent(buffer);
            lsFile.reset();
#else   
            rtc_wdt_feed();            
#endif
            //Serial.println("2*");
        }

        // if (folder != "")
        // {
        //     String parent = folder.substring(0, folder.length() - 1);
        //     int pos = parent.lastIndexOf('/');
        //     if (pos >= 0)
        //         parent = parent.substring(0, pos+1);
        //     else
        //         parent = "";
        //     lsFile.printf_P(PSTR("<tr><td><a href=\"/sd/ls?folder=%s\">[..]</a></td></tr>"), parent.c_str());
        //     if (parent != "")
        //         lsFile.printf_P(PSTR("<tr><td><a href=\"/sd/ls\">[root]</a></td></tr>"));
        // }

//        Logger.Log_P(ILogger::LvlDebug, PSTR("Getting FS stats"));
        FileSystemStats stats = GetFsStats(sd);
//        Logger.Log_P(ILogger::LvlDebug, PSTR("Got FS stats: %lu, %lu"), (unsigned long)stats.totalSize, (unsigned long)stats.fullSize);
        lsFile.printf_P(PSTR("<tr><td>Total:</td><td>%s</td><td></td></tr>\n<tr><td>Free:</td><td>%s</td><td></td></tr>\n</table> <a href=\"/\">Home</a></body>"),
                        fileSize(stats.totalSize).c_str(), fileSize(stats.freeSize).c_str());

#if defined(USE_SD_CARD) && defined (SDFAT)
        if (sd)
        {
            lsFile.printf_P(PSTR("<script>function mkdir() { var folder = (getParameterByName('folder')?getParameterByName('folder'):\"/\"); window.location = \"/sd/mkdir?name=\" + folder + document.getElementById('dir').value+\"&folder=\"+folder;}function getParameterByName(name, url = window.location.href) {name = name.replace(/[\\[\\]]/g, '\\\\$&');var regex = new RegExp('[?&]' + name + '(=([^&#]*)|&|#|$)'), results = regex.exec(url); if (!results) return null; if (!results[2]) return ''; return decodeURIComponent(results[2].replace(/\\+/g, ' '));}</script>"));
        }
#endif
        lsFile.printf_P(PSTR("</html>"));

        //Serial.println("3");

#ifndef ESP32
        _server.sendContent(buffer);
        _server.chunkedResponseFinalize();
        lsFile.reset();
        Logger.Log_P(ILogger::LvlDebug, PSTR("Fully sent LS content, free mem = %ld"), system_get_free_heap_size());
#else
#ifdef NO_ASYNC_WEB_SERVER
        _server.sendContent(buffer);
#else
        request->send(200, __text_html__F, buffer);
#endif
        Logger.Log_P(ILogger::LvlDebug, PSTR("Fully sent LS content, free mem = %ld"), esp_get_free_heap_size());
#endif
    }
#endif        

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    void HandleFileUpload(bool sd, AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
    {
        //DebugDumpRequest(request);

        if (!index)
        {
#if defined(USE_SD_CARD) && defined(SDFAT)
#endif
#ifdef USE_SD_CARD
            if (sd)
            {
                Logger.Log_P(ILogger::LvlDebug, PSTR("ESPHandleFileUpload SD Name: %s"), filename.c_str());
#ifdef SDFAT
                if (request->hasParam("folder"))
                    _fsSdUploadFile = SD.open(request->getParam("folder")->value() + filename, SD_FILE_WRITE);
                else
                    _fsSdUploadFile = SD.open(String(F("/")) + filename, SD_FILE_WRITE);
#else
                if (filename.startsWith("/"))
                    _fsUploadFile = SD.open(filename, SD_FILE_WRITE);
                else
                    _fsUploadFile = SD.open(String(F("/")) + filename, SD_FILE_WRITE);
#endif
            }
            else
#endif
            {
                Logger.Log_P(ILogger::LvlDebug, PSTR("ESPHandleFileUpload Name: %s"), filename.c_str());
#ifdef LFS
                if (filename.startsWith("/"))
                    _fsUploadFile = LittleFS.open(filename, "w");
                else
                    _fsUploadFile = LittleFS.open(String(F("/")) + filename, "w"); // Open the file for writing in SPIFFS (create if it doesn't exist)
#endif
#ifdef USE_SPIFFS
                if (filename.startsWith("/"))
                    _fsUploadFile = SPIFFS.open(filename, "w");
                else
                    _fsUploadFile = SPIFFS.open(String(F("/")) + filename, "w"); // Open the file for writing in SPIFFS (create if it doesn't exist)
#endif
            }
        }
#if defined(USE_SD_CARD) && defined(SDFAT)
        if (sd)
        {
            if (_fsSdUploadFile)
                _fsSdUploadFile.write(data, len); // Write the received bytes to the file
        }
        else
#endif
            if (_fsUploadFile)
                _fsUploadFile.write(data, len); // Write the received bytes to the file
        if (final)
        {
#if defined(USE_SD_CARD) && defined(SDFAT)
            if (sd)
            {
                if (_fsSdUploadFile)
                {
                    _fsSdUploadFile.close();
                    Logger.Log_P(ILogger::LvlDebug, PSTR("ESPHandleFileUpload Size (SDFAT): %ld"), _fsSdUploadFile.size());
                    String folder;
                    if (request->hasParam("folder"))
                        folder = request->getParam("folder")->value();

                    request->redirect(String("/sd/ls?folder=")+folder); // Redirect the client to the success page
                }
                else
                    request->send(500, __text_plain__F, F("500: couldn't create file"));
            }
            else
#endif
            {
                if (_fsUploadFile) // If the file was successfully created
                {
                    _fsUploadFile.close(); // Close the file

                    // reopen to get size
                    // if (filename.startsWith("/"))
                    //     _fsUploadFile = SPIFFS.open(filename);
                    // else
                    //     _fsUploadFile = SPIFFS.open(String(F("/")) + filename); // Open the file for writing in SPIFFS (create if it doesn't exist)
                    // Logger.Log_P(ILogger::LvlDebug, PSTR("ESPHandleFileUpload Size: %ld"), _fsUploadFile.size());
                    // _fsUploadFile.close(); // Close the file again
#ifdef USE_SD_CARD
                    request->redirect(sd ? F("/sd/ls") : F("/spiffs/ls")); // Redirect the client to the success page
#else
                    request->redirect(F("/spiffs/ls")); // Redirect the client to the success page
#endif
                }
                else
                    request->send(500, __text_plain__F, F("500: couldn't create file"));
            }
        }
    }
#else
    void HandleFileUpload(bool sd)
    {
        //Logger.Log_P(ILogger::LvlDebug, PSTR("handleFileUpload SD = %d"), sd);
        if (!DoAuth())
            return;

        HTTPUpload &upload = _server.upload();
        //Logger.Log_P(ILogger::LvlDebug, PSTR("handleFileUpload Status = %d"), upload.status);
        if (upload.status == UPLOAD_FILE_START)
        {
            String filename = upload.filename;
#ifdef USE_SD_CARD
            if (sd)
            {
                Logger.Log_P(ILogger::LvlDebug, PSTR("handleFileUpload Name: %s"), filename.c_str());
                _fsUploadFile = SD.open(filename, SD_FILE_WRITE);
            }
            else
#endif
            {
                if (!filename.startsWith("/"))
                    filename = "/" + filename;
                Logger.Log_P(ILogger::LvlDebug, PSTR("handleFileUpload Name: %s"), filename.c_str());
#ifdef LFS
                _fsUploadFile = LittleFS.open(filename, "w"); // Open the file for writing in SPIFFS (create if it doesn't exist)
#endif
#ifdef USE_SPIFFS
                _fsUploadFile = SPIFFS.open(filename, "w");   // Open the file for writing in SPIFFS (create if it doesn't exist)
#endif
                filename = String();
            }
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
            if (_fsUploadFile)
                _fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
            if (_fsUploadFile)
            {                          // If the file was successfully created
                _fsUploadFile.close(); // Close the file again
                Logger.Log_P(ILogger::LvlDebug, PSTR("handleFileUpload Size: %d"), upload.totalSize);
#ifdef USE_SD_CARD
                _server.sendHeader("Location", sd ? "/sd/ls" : "/spiffs/ls"); // Redirect the client to the success page
#else
                _server.sendHeader("Location", "/spiffs/ls"); // Redirect the client to the success page
#endif
                _server.send(303);
            }
            else
            {
                _server.send(500, __text_plain__F, F("500: couldn't create file"));
            }
        }
    }
#endif

#if defined(USE_SD_CARD) && defined(SDFAT) && defined(ESP32)
    void MkDir(AsyncWebServerRequest *request)
    {
        DebugDumpRequest(request);
        AsyncWebParameter *p = request->getParam("name");
        AsyncWebParameter *p1 = request->getParam("folder");
        if (!p)
        {
            request->send(500, __text_plain__F, F("name missing"));
            return;
        }
        String folder = "/";
        if (p1)
            folder = p1->value();
        auto name = p->value();
        bool ok = SD.mkdir(name);
        if (!ok)
        {
            request->send(500, __text_plain__, String(F("Can't create folder ")) + name);
            return;
        }
        request->redirect(String("/sd/ls?folder=")+folder);
    }
#endif

    void AddRoutes()
    {
#ifdef ESP32
#else
#endif
#ifdef USE_SD_CARD
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        _server.on("/", [this](AsyncWebServerRequest *request) { HandleFileRead("/index.html", request); });
        _server.on("/sd/ls", HTTP_GET,
                   [this](AsyncWebServerRequest *request) { HandleLs(request, true); });
        _server.on(
            "/sd/ls", HTTP_POST,                                                                                                                                                                        // if the client posts to the upload page
            [this](AsyncWebServerRequest *request) { request->send(200); },                                                                                                                             // Send status 200 (OK) to tell the client we are ready to receive
            [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) { HandleFileUpload(true, request, filename, index, data, len, final); } // Receive and save the file
        );
#ifdef SDFAT
        _server.on("/sd/mkdir", HTTP_GET,
                   [this](AsyncWebServerRequest *request) { MkDir(request); });
#endif
#else
        _server.on("/", [this]() { HandleFileRead("/index.html"); });
        _server.on("/sd/ls", HTTP_GET,
                   [this]() { HandleLs(true); });
        _server.on(
            "/sd/ls", HTTP_POST,                 // if the client posts to the upload page
            [this]() { _server.send(200); },     // Send status 200 (OK) to tell the client we are ready to receive
            [this]() { HandleFileUpload(true); } // Receive and save the file
        );
#endif
#endif
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        //        _server.on("/", [this](AsyncWebServerRequest * request) { HandleFileRead("/index_nosd.html", request); });
        _server.on("/spiffs/ls", HTTP_GET,
                   [this](AsyncWebServerRequest *request) { HandleLs(request, false); });
        _server.on("/logout", [this](AsyncWebServerRequest *request) {
            request->send(401, __text_plain__F, F("You are logged out"));
        });
        _server.on(
            "/spiffs/ls", HTTP_POST,                                                                                                                                                                     // if the client posts to the upload page
            [this](AsyncWebServerRequest *request) { request->send(200); },                                                                                                                              // Send status 200 (OK) to tell the client we are ready to receive
            [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) { HandleFileUpload(false, request, filename, index, data, len, final); } // Receive and save the file
        );
// #ifdef USE_SD_CARD
// #ifndef SDFAT
//         _server.serveStatic("/", SD, "/").setDefaultFile("index.html").setAuthentication(_username, _password);
//         ;
// #endif
// #endif
#ifdef LFS
        _server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");
//        _server.serveStatic("/", LittleFS, "/").setDefaultFile("index_nosd.html").setAuthentication(_username, _password);
#endif
#ifdef USE_SPIFFS
        _server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
//        _server.serveStatic("/", SPIFFS, "/").setDefaultFile("index_nosd.html");
#endif
        _server.onNotFound([this](AsyncWebServerRequest *request) {
            if (!HandleFileRead(request->url(), request))
                request->send(404, __text_plain__F, F("404: Not Found")); // otherwise, respond with a 404 (Not Found) error
        });
#else
        _server.on(__slash_favicon_ico__F, [this]() { HandleFileRead(__slash_favicon_ico__F, false); });
        _server.on(F("/spiffs/ls"), HTTP_GET,
                   [this]() { HandleLs(false); });
        _server.on(F("/logout"), [this]() {
            _server.send(401, __text_plain__F, F("You are logged out"));
        });
        _server.on(
            F("/spiffs/ls"), HTTP_POST,              // if the client posts to the upload page
            [this]() { _server.send(200); },      // Send status 200 (OK) to tell the client we are ready to receive
            [this]() { HandleFileUpload(false); } // Receive and save the file
        );
        _server.onNotFound([this]() {
            HandleFileRead(_server.uri());
            // if (!HandleFileRead(_server.uri()))
            //     _server.send(404, __text_plain__F, F("404: Not Found")); // otherwise, respond with a 404 (Not Found) error
        });
        _server.on("/", [this]() { HandleFileRead(F("/index_nosd.html")); });
#endif
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // Can't format here due to watchdog !
//         _server.on("/format", HTTP_GET, [this](AsyncWebServerRequest *request) {
//             if (!DoAuth(request)) return;
//         Logger.Log_P(ILogger::LvlInfo, PSTR("Formatting Ffile system"));
// #ifdef LFS
//             LittleFS.format();
//             request->send(200, __text_plain__F, "LittleFS Formatted");
// #endif
// #ifdef USE_SPIFFS
//             SPIFFS.format();
//             request->send(200, __text_plain__F, "SPIFFS Formatted");
// #endif
//         Logger.Log_P(ILogger::LvlInfo, PSTR("Format complete"));
//     });
#else
        _server.on(F("/format"), HTTP_GET, [this]() {
            if (!DoAuth()) return;
        Logger.Log_P(ILogger::LvlInfo, PSTR("Formatting file system"));
#ifdef LFS
            LittleFS.format();
            _server.send(200, __text_plain__F, F("LittleFS Formatted"));
#endif
#ifdef USE_SPIFFS
            SPIFFS.format();
            _server.send(200, __text_plain__F, F("SPIFFS Formatted"));
#endif
        Logger.Log_P(ILogger::LvlInfo, PSTR("Format complete"));
    });
#endif

    }
};
