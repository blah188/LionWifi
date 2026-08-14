#pragma once

// =============================================================================
// FsBrowser — minimal web filesystem browser for LionWifi (SPIFFS / LittleFS /
// optional SD). Registers routes on the consumer's web server (ESP8266WebServer,
// or ESP32 WebServer / ESPAsyncWebServer): directory listing with upload, file
// tail/download/delete, log views and a format route. HTTP Basic auth optional.
//
// Select a filesystem with -D USE_SPIFFS or -D LFS; enable SD with -D USE_SD_CARD
// (and -D SDFAT for SdFat). Uses the global `Logger` (LionLogger) for the FS
// semaphore and diagnostics. Header-only except for a few symbols in
// FsBrowser.cpp (the comparator, the shared PROGMEM MIME strings).
// =============================================================================

#ifdef ESP32
#include "soc/rtc_wdt.h"
#ifdef NO_ASYNC_WEB_SERVER
#include <WebServer.h>
#else
#include <ESPAsyncWebServer.h>
#endif
#else // ESP8266
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#endif

// ---- Filesystem selection: define EXACTLY ONE of USE_SPIFFS / LFS -----------
// LIONWIFI_FS is the chosen FS object; use it everywhere instead of branching on
// LFS/USE_SPIFFS. LIONWIFI_FS_NAME is its short label for logs.
#if defined(USE_SPIFFS) && defined(LFS)
#error "LionWifi: define only ONE filesystem — USE_SPIFFS or LFS, not both."
#endif
#if !defined(USE_SPIFFS) && !defined(LFS)
#error "LionWifi: no filesystem selected — define -D USE_SPIFFS or -D LFS (LittleFS)."
#endif

#ifdef LFS
#include <LittleFS.h>
#define LIONWIFI_FS LittleFS
#define LIONWIFI_FS_NAME "LFS"
#else // USE_SPIFFS
#ifdef ESP32
#include <SPIFFS.h>
#else
#include <FS.h>
#endif
#define LIONWIFI_FS SPIFFS
#define LIONWIFI_FS_NAME "SPIFFS"
#endif

#ifdef USE_SD_CARD
#ifdef SDFAT
// SdFat streaming on the web uses the async server's chunked response (FsFile is
// not an fs::FS). The sync WebServer / ESP8266 paths use the core `File` type and
// can't hold an FsFile — so SDFAT is only supported on the ESP32 async server.
#if !defined(ESP32) || defined(NO_ASYNC_WEB_SERVER)
#error "LionWifi: SDFAT is supported only on the ESP32 async web server (the default). For ESP8266 or -D NO_ASYNC_WEB_SERVER, use the Arduino SD library (USE_SD_CARD without SDFAT)."
#endif
#include <SdFat.h>
extern SdFat SD;
#define SD_FILE_READ O_RDONLY
#define SD_FILE_WRITE (O_RDWR | O_CREAT | O_AT_END)
// FsFile is the default SdFat (SdFs) file type — handles FAT16/FAT32 AND exFAT.
// It's a concrete class (unlike the `File` alias), so it never clashes with the
// core's fs::File. Build SdFat in its default mode (SDFAT_FILE_TYPE=3).
#define MySdFile FsFile
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
#if !defined(ESP32) || defined(NO_ASYNC_WEB_SERVER)
#include <ServerStream.h> // sync-server chunked streaming (not available for async)
#endif
#if defined(USE_SD_CARD) && defined(SDFAT) && defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
#include <memory> // shared_ptr keeps the SdFat file alive across async chunked sends
#endif

#include "DirEntry.h"

extern const char __text_plain__P[];
#define __text_plain__F FPSTR(__text_plain__P)
extern const char __text_html__P[];
#define __text_html__F FPSTR(__text_html__P)
extern const char __slash_favicon_ico__P[];
#define __slash_favicon_ico__F FPSTR(__slash_favicon_ico__P)

// ---- File-listing look & feel ----------------------------------------------
// The directory page (/spiffs/ls, /sd/ls) ships a small built-in stylesheet
// (light + dark theme). It costs ~2.0 KB of flash (the CSS string is ~2012 B).
// It is fully configurable WITHOUT editing this library — pick the level of
// control you need:
//
//  1. TWEAK the palette / add rules while KEEPING the built-in sheet. The default
//     is exposed as the macro DEFAULT_FS_BROWSER_CSS, so prepend your overrides
//     and append it back (adjacent string literals concatenate):
//         #define FS_BROWSER_CSS ":root{--fs-accent:#0aa}" DEFAULT_FS_BROWSER_CSS
//     Every color is a :root custom property, so a one-line var override re-themes
//     everything. Vars: --fs-bg --fs-fg --fs-card --fs-line --fs-muted
//           --fs-accent --fs-danger   (each has a dark-theme value in @media).
//
//  2. REPLACE the whole stylesheet:  -D FS_BROWSER_CSS='"...your css..."'
//     (a string literal). Easiest from a force-included config header:
//         #define FS_BROWSER_CSS ".fs-table{font-size:16px;width:100%}"
//     Flash cost then = the length of YOUR string instead of ~2 KB. Keep the
//     class names below so the markup still targets your rules.
//
//  3. TURN IT OFF:  -D NO_FS_BROWSER_CSS
//     Emits no <style> at all — the page falls back to the browser's default
//     table (plain, but fully functional). Saves the full ~2.0 KB of flash.
//     Combine with option 2 in your own page wrapper if you serve CSS elsewhere.
//
// Stable class names the markup uses (target these from options 1/2):
//   .fs-wrap      page container (centered, max-width)
//   .fs-upload    the upload <form> row
//   .fs-btn       the Upload submit button
//   .fs-table     the listing <table>            (thead / tbody / tfoot)
//   th/td.name    filename column   (td.name holds .fs-link for files)
//   th/td.size    size column       (shows "DIR" for folders)
//   th/td.time    created/modified columns       (only with USE_FILE_TIME)
//   th/td.actions action column
//   tr.dir        a folder row
//   .fs-link      the filename link (download)
//   .act          an action link (Tail / Download)
//   .act-del      the Delete action link (danger color)
//   .fs-summary   the Total/Free footer rows
//   .fs-home      the "Home" link under the table
#ifndef NO_FS_BROWSER_CSS
// The built-in sheet is exposed as DEFAULT_FS_BROWSER_CSS so you can KEEP it and
// just prepend overrides:  -D FS_BROWSER_CSS='":root{--fs-accent:#0aa}" DEFAULT_FS_BROWSER_CSS'
#define DEFAULT_FS_BROWSER_CSS \
":root{--fs-bg:#f3f4f6;--fs-fg:#1f2937;--fs-card:#fff;--fs-line:#e5e7eb;--fs-muted:#6b7280;--fs-accent:#6366f1;--fs-danger:#dc2626;}" \
"*{box-sizing:border-box;}" \
"body{margin:0;padding:24px;font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--fs-bg);color:var(--fs-fg);font-size:14px;line-height:1.45;}" \
".fs-wrap{max-width:900px;margin:0 auto;}" \
".fs-upload{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:0 0 16px;}" \
".fs-btn{background:var(--fs-accent);color:#fff;border:0;border-radius:8px;padding:8px 16px;font:inherit;cursor:pointer;}" \
".fs-btn:hover{filter:brightness(1.08);}" \
".fs-table{width:100%;border-collapse:collapse;background:var(--fs-card);border:1px solid var(--fs-line);border-radius:12px;overflow:hidden;}" \
".fs-table th,.fs-table td{padding:8px 12px;text-align:left;border-bottom:1px solid var(--fs-line);}" \
".fs-table th{font-size:12px;text-transform:uppercase;letter-spacing:.04em;color:var(--fs-muted);background:rgba(0,0,0,.02);}" \
".fs-table tbody tr:hover{background:rgba(99,102,241,.06);}" \
".fs-table .size{white-space:nowrap;color:var(--fs-muted);}" \
".fs-table .time{white-space:nowrap;color:var(--fs-muted);font-size:12px;}" \
".fs-table .actions{white-space:nowrap;}" \
".fs-link{color:var(--fs-fg);text-decoration:none;font-weight:500;}" \
".fs-link:hover{color:var(--fs-accent);text-decoration:underline;}" \
"tr.dir .name{color:var(--fs-muted);font-weight:600;}" \
".act{color:var(--fs-accent);text-decoration:none;margin-right:10px;font-size:13px;}" \
".act:hover{text-decoration:underline;}" \
".act-del{color:var(--fs-danger);}" \
".fs-summary td{color:var(--fs-muted);font-weight:600;border-bottom:0;}" \
".fs-home{display:inline-block;margin-top:16px;color:var(--fs-accent);text-decoration:none;}" \
".fs-home:hover{text-decoration:underline;}" \
"@media(prefers-color-scheme:dark){" \
":root{--fs-bg:#0f172a;--fs-fg:#e5e7eb;--fs-card:#1e293b;--fs-line:#334155;--fs-muted:#94a3b8;--fs-accent:#a78bfa;--fs-danger:#f87171;}" \
".fs-table th{background:rgba(255,255,255,.03);}" \
".fs-table tbody tr:hover{background:rgba(167,139,250,.10);}}"
#ifndef FS_BROWSER_CSS
#define FS_BROWSER_CSS DEFAULT_FS_BROWSER_CSS
#endif
#endif

struct FileSystemStats
{
    uint64_t totalSize, freeSize;
};

int DirEntrySort(const void *cmp1, const void *cmp2);

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
    const char *_username = nullptr, *_password = nullptr;
#if defined(USE_SD_CARD) && defined(SDFAT)
    FsFile _fsSdUploadFile;
#endif
    File _fsUploadFile;
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // Async uploads: the onUpload callback may not send the HTTP response (only
    // the onRequest callback, which runs last, may). So onUpload records the
    // outcome here and the route's onRequest handler turns it into a redirect/500.
    bool _uploadOk = false;
#endif

public:
    // `server` is the consumer's web server (type varies by platform/build).
    // Diagnostics and the FS semaphore go through the global LionLogger `Logger`.
    // Pass username/password for HTTP Basic auth, or NULL/NULL to disable it.
#ifdef ESP32
#ifdef NO_ASYNC_WEB_SERVER
    FsBrowser(WebServer &server, const char *username, const char *password) : _server(server)
#else
    FsBrowser(AsyncWebServer &server, const char *username, const char *password) : _server(server)
#endif
#else // ESP8266
    FsBrowser(ESP8266WebServer &server, const char *username, const char *password) : _server(server)
#endif
    {
        if (username && password)
        {
            _username = username;
            _password = password;
            _doAuth = true;
        }
    }

    // Holds a server reference, an open upload File and registered routes —
    // single-instance, non-copyable (a copy would alias all of that).
    FsBrowser(const FsBrowser &) = delete;
    FsBrowser &operator=(const FsBrowser &) = delete;

    // Returns a flash-resident MIME type for the file extension (no String built
    // here; the caller materializes one only if its API needs it).
    static const __FlashStringHelper *GetContentType(const String &filename)
    {
        if (filename.endsWith(".html") || filename.endsWith(".htm"))
            return __text_html__F;
        else if (filename.endsWith(".css"))
            return F("text/css");
        else if (filename.endsWith(".js"))
            return F("application/javascript");
        else if (filename.endsWith(".ico"))
            return F("image/x-icon");
        else if (filename.endsWith(".png"))
            return F("image/png");
        else if (filename.endsWith(".jpg"))
            return F("image/jpeg");
        else if (filename.endsWith(".gz"))
            return F("application/x-gzip");
        return __text_plain__F;
    }

    // Percent-encode a filename for use inside an href, so spaces / UTF-8 bytes /
    // reserved chars ('#', '?', '&', …) yield a valid URL. '/' and unreserved
    // chars pass through. (The cores expose urlDecode() but no urlEncode().)
    // Decoding back to the real name happens server-side: the sync WebServer
    // leaves uri() percent-encoded so we urlDecode() it in onNotFound; the async
    // server already decodes request->url().
    static String UrlEncode(const char *s)
    {
        static const char hex[] = "0123456789ABCDEF";
        String out;
        for (; *s; s++)
        {
            unsigned char c = (unsigned char)*s;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
                out += (char)c;
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0F];
            }
        }
        return out;
    }

    static String FileSize(uint64_t size)
    {
        if (size < 100000)
            return String((uint32_t)size);
        if (size < 100 * 1024 * 1024l)
            return String((uint32_t)(size / 1024l)) + F("k");
        return String((uint32_t)(size / 1024l / 1024l)) + F("m");
    }

    static void FileSize(uint64_t size, Stream &out)
    {
        if (size < 100000)
            out.print((uint32_t)size);
        else if (size < 100 * 1024 * 1024l)
            out.printf_P(PSTR("%ldk"), (uint32_t)(size / 1024l));
        else
            out.printf_P(PSTR("%ldm"), (uint32_t)(size / 1024l / 1024l));
    }

    static String FileTime(time_t unix)
    {
#ifdef USE_FILE_TIME
        static char buffer[64];
        buffer[63] = 0;
        struct tm *timeinfo = localtime(&unix);
        if (!timeinfo)
            return String(F("Unknown"));
        snprintf_P(buffer, 63, PSTR("%02d/%02d/%04d<br>%02d:%02d:%02d"), timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        return String(buffer);
#else
        return String(F("Unknown"));
#endif
    }

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    bool DoAuth(AsyncWebServerRequest *request)
    {
#ifdef NO_AUTH
        return true; // Authentication disabled at build time — plain access.
#endif
        if (_doAuth && !request->authenticate(_username, _password))
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
        if (_doAuth && !_server.authenticate(_username, _password))
        {
            _server.requestAuthentication();
            Logger.Log_P(ILogger::LvlWarning, PSTR("Need auth for %d.%d.%d.%d: %s"), _server.client().remoteIP()[0], _server.client().remoteIP()[1], _server.client().remoteIP()[2], _server.client().remoteIP()[3], _server.uri().c_str());
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
            request->send(429, __text_plain__F, F("Server Busy"));
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
        if (!sd)
            exists = LIONWIFI_FS.exists(name);
        if (!exists)
        {
            Logger.UnlockFsSemaphore();
            Logger.Log_P(ILogger::LvlWarning, PSTR("sendFileResponse: %s file %s not found"), (sd ? F("SD") : F("SP")), name);
            request->send(404, __text_plain__F, String(F("Can't find ")) + (sd ? F("SD") : F("SP")) + F(" file ") + name);
            return false;
        }
        if (tail) // Only tail — read the last TailSize bytes into buf
        {
            char *buf = new char[TailSize + 1];
            if (!buf)
            {
                Logger.UnlockFsSemaphore();
                request->send(500, __text_plain__F, F("500: out of memory"));
                return false;
            }
            size_t read = 0;
#ifdef USE_SD_CARD
            if (sd)
            {
                String sdPath = name;
                if (sdPath.length() > 0 && sdPath[0] != '/')
                    sdPath = "/" + sdPath;
                MySdFile f = SD.open(sdPath.c_str(), SD_FILE_READ);
                if (f.size() > TailSize)
                    f.seek(f.size() - TailSize);
                read = f.readBytes(buf, TailSize);
                f.close();
            }
            else
#endif
            {
                File f = LIONWIFI_FS.open(name, "r");
                uint32_t tailPos = f.size() > TailSize ? f.size() - TailSize : 0;
                if (tailPos)
                    f.seek(tailPos);
                read = f.readBytes(buf, TailSize);
                Logger.Log_P(ILogger::LvlDebug, PSTR("sendFileResponse: sending %u tail bytes (from pos %lu) of file %s"), (unsigned)read, (unsigned long)tailPos, name);
                f.close();
            }
            buf[read] = 0;
            request->send(200, __text_plain__F, buf);
            delete[] buf;
            Logger.UnlockFsSemaphore();
            return true;
        }

#ifdef USE_SD_CARD
        if (sd)
        {
            String sdPath = name;
            if (sdPath.length() > 0 && sdPath[0] != '/')
                sdPath = "/" + sdPath;
#ifdef SDFAT
            // SdFat's FsFile is not an fs::FS, so beginResponse(FS,...) can't serve
            // it. Stream via a chunked response; the file lives in a shared_ptr
            // captured by the filler, so it is closed when the stream finishes OR
            // the client aborts (the response and its captured copy are destroyed).
            auto f = std::make_shared<FsFile>();
            if (!f->open(sdPath.c_str(), O_RDONLY))
            {
                Logger.UnlockFsSemaphore();
                request->send(500, __text_plain__F, F("500: SD open failed"));
                return false;
            }
            AsyncWebServerResponse *response = request->beginChunkedResponse(GetContentType(name),
                [f](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
                {
                    (void)index;
                    int n = f->read(buffer, maxLen);
                    return n > 0 ? (size_t)n : 0;
                });
            if (forceDownload)
                response->addHeader(F("Content-Disposition"), String(F("attachment; filename=")) + (name[0] == '/' ? name + 1 : name));
            request->send(response);
#else
            AsyncWebServerResponse *response = request->beginResponse(SD, sdPath.c_str(), GetContentType(name), forceDownload);
            request->send(response);
#endif
        }
        else
        {
#endif
            AsyncWebServerResponse *response = request->beginResponse(LIONWIFI_FS, name, GetContentType(name), forceDownload);
            request->send(response);
#ifdef USE_SD_CARD
        }
#endif
        Logger.UnlockFsSemaphore();

        Logger.Log_P(ILogger::LvlDebug, PSTR("sendFileResponse: %s(%s)"), name, sd ? "Sd" : "SP");
        return true;
    }
#else // sync web server (ESP8266, or ESP32 + NO_ASYNC_WEB_SERVER)
    bool SendFileResponse(const char *name, bool sd, bool tail, bool forceDownload = false)
    {
        if (!Logger.TryLockFsSemaphore()) // no-op (true) on ESP8266; real lock on ESP32+WebServer
        {
            _server.send(429, __text_plain__F, F("Server Busy"));
            return false;
        }
        File dataFile;

#ifdef USE_SD_CARD
        if (sd)
        {
            // Normalize a leading '/' — the SD VFS needs it, and /sd/tail strips
            // the slash (substring(9)) while /sd/download keeps it.
            String sdPath = (name[0] == '/') ? String(name) : (String('/') + name);
            dataFile = SD.open(sdPath.c_str(), SD_FILE_READ);
        }
        else
#endif
            dataFile = LIONWIFI_FS.open(name, "r");

        if (!dataFile)
        {
            Logger.UnlockFsSemaphore();
            Logger.Log_P(ILogger::LvlWarning, PSTR("sendFileResponse: %s file %s not found"), (sd ? F("SD") : F("SP")), name);
            _server.send(404, __text_plain__F, String(F("Can't find ")) + (sd ? F("SD") : F("SP")) + F(" file ") + name);
            return false;
        }
        if (forceDownload)
        {
            _server.sendHeader(F("Content-Disposition"), String(F("attachment; filename=")) + (name[0] == '/'?name+1:name));
            _server.sendHeader(F("Content-Transfer-Encoding"), F("binary"));
            _server.sendHeader(F("Expires"), F("0"));
            _server.sendHeader(F("Cache-Control"), F("must-revalidate, post-check=0, pre-check=0"));
            _server.sendHeader(F("Pragma"), F("public"));
        }
        if (tail) // Send only the last TailSize bytes with a CORRECT Content-Length.
        {         // streamFile() advertises the full file size while sending only
                  // the tail → body/length mismatch and a hung client on big logs.
            uint32_t fsize = dataFile.size();
            uint32_t pos = fsize > TailSize ? fsize - TailSize : 0;
            if (pos)
                dataFile.seek(pos);
            // Стримим хвост маленькими чанками с ПРАВИЛЬНЫМ Content-Length — без
            // большого буфера. Раньше читали весь хвост (до 8 КБ) в char[] и отдавали
            // send(200,type,buf), где buf копировался ещё и в String: на фрагментированной
            // куче ESP8266 аллокация падала → тело пустое (Content-Length 0), хотя лог
            // печатал read=TailSize. (streamFile() тоже нельзя — он ставит полный размер файла.)
            size_t tailLen = fsize - pos;
            _server.setContentLength(tailLen);
            _server.send(200, GetContentType(name), emptyString);
            uint8_t chunk[512];
            size_t sent = 0;
            while (sent < tailLen)
            {
                size_t want = tailLen - sent;
                if (want > sizeof(chunk))
                    want = sizeof(chunk);
                int n = dataFile.read(chunk, want);
                if (n <= 0)
                    break;
                _server.sendContent((const char *)chunk, (size_t)n);
                sent += n;
            }
            dataFile.close();
            Logger.UnlockFsSemaphore();
            Logger.Log_P(ILogger::LvlDebug, PSTR("Sent tail %s(%s), %u bytes"), name, sd ? "Sd" : "SP", (unsigned)sent);
            return true;
        }
        size_t sent = _server.streamFile(dataFile, GetContentType(name));
        dataFile.close();
        Logger.UnlockFsSemaphore();
#ifndef LOG_FAVICON
        if (strcasecmp_P(name, __slash_favicon_ico__P) != 0)
#endif
            Logger.Log_P(ILogger::LvlDebug, PSTR("Sent %s(%s), %u bytes"), name, sd ? "Sd" : "SP", (unsigned)sent);
        return true;
    }
#endif

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    bool HandleFileRead(String path, AsyncWebServerRequest *request, bool auth = true) // send the right file to the client (if it exists)
    {
        if (auth && !DoAuth(request))
            return false;
#else // sync web server (ESP8266, or ESP32 + NO_ASYNC_WEB_SERVER)
    bool HandleFileRead(const String &path, bool auth = true) // send the right file to the client (if it exists)
    {
        if (auth && !DoAuth())
            return false;
#endif

        if (path.startsWith("/del"))
        {
            String name = path.substring(4);
            Logger.Log_P(ILogger::LvlDebug, PSTR("about to delete %s"), name.c_str());

            bool del = false;
            if (Logger.TryLockFsSemaphore())
            {
                if (LIONWIFI_FS.exists(name))
                {
                    LIONWIFI_FS.remove(name);
                    del = true;
                }
                Logger.UnlockFsSemaphore();
            }
            if (!del)
                return false;
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
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER) // async: `request` exists
            request->redirect(F("/sd/ls"));
#else // sync (ESP8266 or ESP32 + NO_ASYNC_WEB_SERVER): no `request`
            _server.sendHeader(F("Location"), F("/sd/ls")); // Redirect the client to the success page
            _server.send(303);
#endif
            return true;
        }
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        if (path.startsWith("/sd/tail"))
            return SendFileResponse(path.substring(9).c_str(), request, true, true);
        if (path.startsWith("/sd/download"))
            return SendFileResponse(path.substring(12).c_str(), request, true, false, true); // full file, as attachment
        if (path.startsWith("/sd"))
            return SendFileResponse(path.substring(3).c_str(), request, true, false);
#else
        if (path.startsWith("/sd/tail"))
            return SendFileResponse(path.substring(9).c_str(), true, true);
        if (path.startsWith("/sd/download"))
            return SendFileResponse(path.substring(12).c_str(), true, false, true); // full file, as attachment
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
#ifdef ESP32
        File dir = LIONWIFI_FS.open("/");
        File file;
        while (file = dir.openNextFile())
#else // ESP8266 uses the Dir iterator API
        Dir dir = LIONWIFI_FS.openDir("/");
        while (dir.next())
#endif
        {
#ifdef ESP32
            rtc_wdt_feed();
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
        Logger.Log_P(ILogger::LvlDebug, PSTR("Listed %d files from %s"), files.Length(), LIONWIFI_FS_NAME);
        Logger.UnlockFsSemaphore();
    }

    void ListSdFiles(Array<DirEntry> &files)
    {
#ifdef USE_SD_CARD
#ifdef SDFAT
        Logger.Log_P(ILogger::LvlDebug, PSTR("Using SDFAT"));
        FsFile dir, file;
        if (dir.open("/"))
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
            DirEntry entry(file.name());
            entry.size = file.size();
#ifdef USE_FILE_TIME
            entry.creationTime = entry.writeTime = file.getLastWrite();
#endif
                files += entry;
#else
            // Cap the SD listing at ~120 entries (memory/watchdog guard): once
            // reached, append a final sentinel row and stop enumerating below.
            if (files.Length() >= 120)
            {
                DirEntry entry("zzz.zzz");
                entry.size = 0;
                files += entry;
            }
            else
            {
                DirEntry entry(file.fullName());
                entry.size = file.size();
#ifdef USE_FILE_TIME
                // ESP8266 fs::File has no getCreationTime(); use last-write for both.
                entry.creationTime = entry.writeTime = file.getLastWrite();
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
                stats.freeSize = LIONWIFI_FS.totalBytes() - LIONWIFI_FS.usedBytes();
                stats.totalSize = LIONWIFI_FS.totalBytes();
                Logger.UnlockFsSemaphore();
            }
#else // ESP8266
            FSInfo fs_info;
            LIONWIFI_FS.info(fs_info);
            stats.freeSize = fs_info.totalBytes - fs_info.usedBytes;
            stats.totalSize = fs_info.totalBytes;
#endif
        }
        return stats;
    }

    // ---- /spiffs/ls rendering, shared by the sync ServerStream path and the async
    //      chunked generator. Row printf keeps the format string in flash on ESP8266
    //      (printf_P); on ESP32 PROGMEM is flat and Print has no printf_P. ---------
#ifdef ESP32
#define LS_ROW_PRINTF(o, fmt, ...) (o).printf(fmt, ##__VA_ARGS__)
#else
#define LS_ROW_PRINTF(o, fmt, ...) (o).printf_P(PSTR(fmt), ##__VA_ARGS__)
#endif

    void renderLsHead(Print &out)
    {
        out.print(F("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Files</title>"));
#ifndef NO_FS_BROWSER_CSS
        out.print(F("<style>" FS_BROWSER_CSS "</style>"));
#endif
        out.print(F("</head><body><div class=\"fs-wrap\">"
                    "<form class=\"fs-upload\" method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"><input class=\"fs-btn\" type=\"submit\" value=\"Upload\"></form>"
                    "<table class=\"fs-table\"><thead><tr><th class=\"name\">Name</th><th class=\"size\">Size</th>"));
#ifdef USE_FILE_TIME
        out.print(F("<th class=\"time\">Created</th><th class=\"time\">Modified</th>"));
#endif
        out.print(F("<th class=\"actions\">Action</th></tr></thead><tbody>\n"));
    }

    void renderLsRow(Print &out, DirEntry &entry, bool sd)
    {
        // href targets are percent-encoded; visible text / confirm() prompt stay raw.
        String enc = UrlEncode(entry.fullName);
        if (entry.isFolder) // subfolders listed but not navigable (flat listing)
            LS_ROW_PRINTF(out, "<tr class=\"dir\"><td class=\"name\">%s</td><td class=\"size\">DIR</td>", entry.fullName);
        else
            LS_ROW_PRINTF(out, "<tr><td class=\"name\"><a class=\"fs-link\" href=\"%s\">%s</a></td><td class=\"size\">%s</td>",
                          enc.c_str(), entry.fullName, FileSize(entry.size).c_str());
#ifdef USE_FILE_TIME
        LS_ROW_PRINTF(out, "<td class=\"time\">%s</td><td class=\"time\">%s</td>",
                      FileTime(entry.creationTime).c_str(), FileTime(entry.writeTime).c_str());
#endif
        if (entry.isFolder)
            out.print(F("<td class=\"actions\"></td></tr>\n"));
        else
        {
            if (sd)
                LS_ROW_PRINTF(out, "<td class=\"actions\"><a class=\"act\" href=\"/sd/tail/%s\">Tail</a><a class=\"act\" href=\"/sd/download/%s\">Download</a><a class=\"act act-del\" href=\"/sd/del/%s\"",
                              enc.c_str(), enc.c_str(), enc.c_str());
            else
                LS_ROW_PRINTF(out, "<td class=\"actions\"><a class=\"act\" href=\"/tail/%s\">Tail</a><a class=\"act\" href=\"/download/%s\">Download</a><a class=\"act act-del\" href=\"/del/%s\"",
                              enc.c_str(), enc.c_str(), enc.c_str());
            LS_ROW_PRINTF(out, " onclick=\"return confirm('Are you sure to delete %s?')\">Delete</a></td></tr>\n", entry.fullName);
        }
    }

    void renderLsFoot(Print &out, const FileSystemStats &stats)
    {
        out.print(F("</tbody><tfoot>"));
#ifdef USE_FILE_TIME
        LS_ROW_PRINTF(out, "<tr class=\"fs-summary\"><td class=\"name\">Total</td><td class=\"size\">%s</td><td colspan=\"3\"></td></tr>"
                      "<tr class=\"fs-summary\"><td class=\"name\">Free</td><td class=\"size\">%s</td><td colspan=\"3\"></td></tr>",
                      FileSize(stats.totalSize).c_str(), FileSize(stats.freeSize).c_str());
#else
        LS_ROW_PRINTF(out, "<tr class=\"fs-summary\"><td class=\"name\">Total</td><td class=\"size\">%s</td><td></td></tr>"
                      "<tr class=\"fs-summary\"><td class=\"name\">Free</td><td class=\"size\">%s</td><td></td></tr>",
                      FileSize(stats.totalSize).c_str(), FileSize(stats.freeSize).c_str());
#endif
        out.print(F("</tfoot></table><a class=\"fs-home\" href=\"/\">Home</a></div></body></html>"));
    }

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // Streaming state for the async chunked LS response — lives in a shared_ptr
    // captured by the filler; freed when the stream completes OR the client aborts.
    struct LsState
    {
        Array<DirEntry> files;
        FileSystemStats stats;
        bool sd = false;
        int phase = 0; // 0 head, 1 rows, 2 foot, 3 done
        int row = 0;
        String pending;   // current not-yet-fully-sent fragment
        size_t pos = 0;   // bytes of `pending` already copied out
    };

    void HandleLs(AsyncWebServerRequest *request, bool sd)
    {
        Logger.Log_P(ILogger::LvlDebug, PSTR("Got LS request, SD = %d, free mem = %ld"), (int)sd,  esp_get_free_heap_size());
        if (!DoAuth(request))
            return;
#else // sync web server (ESP8266, or ESP32 + NO_ASYNC_WEB_SERVER)
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
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        // Async: TRUE chunked stream. The page is generated on demand inside the
        // filler, so only one fragment (a row / the head) is buffered at a time —
        // peak memory is independent of file count. (Buffering the whole page in a
        // String/AsyncResponseStream needs one large contiguous block that OOM-panics
        // on a fragmented heap once the listing grows.) State (sorted list + render
        // position) lives in a shared_ptr captured by the filler; it is freed when the
        // stream finishes OR the client aborts — same pattern as SendFileResponse.
        auto st = std::make_shared<LsState>();
        st->sd = sd;
        if (sd)
            ListSdFiles(st->files);
        else
            ListSpiffsFiles(st->files);
        qsort(st->files.GetData(), st->files.Length(), sizeof(DirEntry), DirEntrySort);
        st->stats = GetFsStats(sd);
        Logger.Log_P(ILogger::LvlDebug, PSTR("LS: streaming %d files"), st->files.Length());

        FsBrowser *self = this;
        request->send(request->beginChunkedResponse("text/html; charset=utf-8",
            [self, st](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
            {
                (void)index;
                size_t written = 0;
                while (written < maxLen)
                {
                    if (st->pos >= st->pending.length()) // current fragment drained → make the next
                    {
                        st->pending = String();
                        st->pos = 0;
                        StringStream ss(st->pending);
                        if (st->phase == 0) { self->renderLsHead(ss); st->phase = 1; }
                        else if (st->phase == 1)
                        {
                            if (st->row < st->files.Length()) { self->renderLsRow(ss, st->files[st->row], st->sd); st->row++; }
                            else st->phase = 2;
                        }
                        else if (st->phase == 2) { self->renderLsFoot(ss, st->stats); st->phase = 3; }
                        else break; // phase 3: whole page emitted
                        rtc_wdt_feed();
                        if (st->pending.length() == 0) continue;
                    }
                    size_t avail = st->pending.length() - st->pos;
                    size_t n = avail < (maxLen - written) ? avail : (maxLen - written);
                    memcpy(buffer + written, st->pending.c_str() + st->pos, n);
                    written += n;
                    st->pos += n;
                }
                return written; // 0 → done, ends the stream
            }));
#else
        Array<DirEntry> files;
        if (sd)
            ListSdFiles(files); // SD listing is flat (root only)
        else
            ListSpiffsFiles(files);
        qsort(files.GetData(), files.Length(), sizeof(DirEntry), DirEntrySort);

        // Sync chunked-response start: CONTENT_LENGTH_UNKNOWN → "Transfer-Encoding:
        // chunked"; send() with an empty String writes only headers. ServerStream then
        // streams the chunks; terminated at the end. (send(), NOT send_P("").)
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(200, __text_html__F, "");
        ServerStream out(_server);
        renderLsHead(out);
        for (int i = 0; i < files.Length(); i++)
            renderLsRow(out, files[i], sd);
        renderLsFoot(out, GetFsStats(sd));
        out.flush(); // emit the final partial chunk
#ifdef ESP32
        _server.sendContent(""); // ESP32 WebServer: terminate the chunked response
        Logger.Log_P(ILogger::LvlDebug, PSTR("Fully sent LS content, free mem = %ld"), esp_get_free_heap_size());
#else
        _server.chunkedResponseFinalize(); // ESP8266: terminate the chunked response
        Logger.Log_P(ILogger::LvlDebug, PSTR("Fully sent LS content, free mem = %ld"), system_get_free_heap_size());
#endif
#endif
    }

#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    void HandleFileUpload(bool sd, AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
    {
        if (!index) // first chunk: open the destination file
        {
            _uploadOk = false;
#ifdef USE_SD_CARD
            if (sd)
            {
                Logger.Log_P(ILogger::LvlInfo, PSTR("Upload start (SD): %s"), filename.c_str());
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
                Logger.Log_P(ILogger::LvlInfo, PSTR("Upload start: %s"), filename.c_str());
                if (filename.startsWith("/"))
                    _fsUploadFile = LIONWIFI_FS.open(filename, "w");
                else
                    _fsUploadFile = LIONWIFI_FS.open(String(F("/")) + filename, "w"); // create if it doesn't exist
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
            {
                size_t w = _fsUploadFile.write(data, len); // Write the received bytes to the file
                if (w != len) // FS full / write error — abort; reported as failure at final
                {
                    Logger.Log_P(ILogger::LvlError, PSTR("Upload: short write %u/%u, aborting"), (unsigned)w, (unsigned)len);
                    _fsUploadFile.close();
                }
            }
        if (final) // last chunk: close + record outcome. The HTTP response is sent
        {          // by the route's onRequest handler (an onUpload callback must not).
            size_t total = index + len;
#if defined(USE_SD_CARD) && defined(SDFAT)
            if (sd)
            {
                _uploadOk = (bool)_fsSdUploadFile;
                if (_fsSdUploadFile)
                    _fsSdUploadFile.close();
            }
            else
#endif
            {
                _uploadOk = (bool)_fsUploadFile;
                if (_fsUploadFile)
                    _fsUploadFile.close();
            }
            Logger.Log_P(_uploadOk ? ILogger::LvlInfo : ILogger::LvlError,
                         PSTR("Upload end: %s, %u bytes (%s)"), filename.c_str(), (unsigned)total, _uploadOk ? PSTR("OK") : PSTR("FAILED"));
        }
    }
#else // sync web server (ESP8266, or ESP32 + NO_ASYNC_WEB_SERVER)
    void HandleFileUpload(bool sd)
    {
        if (!DoAuth())
            return;

        HTTPUpload &upload = _server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
            String filename = upload.filename;
            if (!filename.startsWith("/")) // both SD (ESP32 VFS) and the internal FS need a leading '/'
                filename = "/" + filename;
            Logger.Log_P(ILogger::LvlInfo, PSTR("Upload start: %s"), filename.c_str());
#ifdef USE_SD_CARD
            if (sd)
                _fsUploadFile = SD.open(filename, SD_FILE_WRITE);
            else
#endif
                _fsUploadFile = LIONWIFI_FS.open(filename, "w"); // create if it doesn't exist
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
            if (_fsUploadFile)
            {
                size_t w = _fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
                if (w != upload.currentSize) // FS full / write error — abort so UPLOAD_FILE_END reports failure
                {
                    Logger.Log_P(ILogger::LvlError, PSTR("handleFileUpload: short write %u/%u, aborting"), (unsigned)w, (unsigned)upload.currentSize);
                    _fsUploadFile.close();
                }
            }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
            if (_fsUploadFile)
            {                          // If the file was successfully created
                _fsUploadFile.close(); // Close the file again
                Logger.Log_P(ILogger::LvlInfo, PSTR("Upload end: %u bytes (OK)"), (unsigned)upload.totalSize);
#ifdef USE_SD_CARD
                _server.sendHeader(F("Location"), sd ? F("/sd/ls") : F("/spiffs/ls")); // Redirect the client to the success page
#else
                _server.sendHeader(F("Location"), F("/spiffs/ls")); // Redirect the client to the success page
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

    void AddRoutes()
    {
#ifdef USE_SD_CARD
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
        _server.on("/", [this](AsyncWebServerRequest *request) { HandleFileRead("/index.html", request); });
        _server.on("/sd/ls", HTTP_GET,
                   [this](AsyncWebServerRequest *request) { HandleLs(request, true); });
        _server.on(
            "/sd/ls", HTTP_POST,
            // onRequest runs after the upload: turn the recorded outcome into a response.
            [this](AsyncWebServerRequest *request) { if (_uploadOk) request->redirect(F("/sd/ls")); else request->send(500, __text_plain__F, F("500: upload failed")); },
            [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) { HandleFileUpload(true, request, filename, index, data, len, final); } // Receive and save the file
        );
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
#ifndef USE_SD_CARD
        // SPIFFS/LittleFS-only build: serve the no-SD landing page at root.
        // (With USE_SD_CARD the root is registered above → /index.html.) Without
        // this, the ESP32-async build leaves "/" unhandled → onNotFound →
        // HandleFileRead("/") appends index.html → 404. Mirrors the sync branch.
        _server.on("/", [this](AsyncWebServerRequest *request) { HandleFileRead("/index_nosd.html", request); });
#endif
        _server.on("/spiffs/ls", HTTP_GET,
                   [this](AsyncWebServerRequest *request) { HandleLs(request, false); });
        _server.on("/logout", [this](AsyncWebServerRequest *request) {
            request->send(401, __text_plain__F, F("You are logged out"));
        });
        _server.on(
            "/spiffs/ls", HTTP_POST,
            // onRequest runs after the upload: turn the recorded outcome into a response.
            [this](AsyncWebServerRequest *request) { if (_uploadOk) request->redirect(F("/spiffs/ls")); else request->send(500, __text_plain__F, F("500: upload failed")); },
            [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) { HandleFileUpload(false, request, filename, index, data, len, final); } // Receive and save the file
        );
        _server.serveStatic("/favicon.ico", LIONWIFI_FS, "/favicon.ico");
        _server.onNotFound([this](AsyncWebServerRequest *request) {
            if (!HandleFileRead(request->url(), request))
                request->send(404, __text_plain__F, F("404: Not Found")); // otherwise, respond with a 404 (Not Found) error
        });
#else // sync web server (ESP8266, or ESP32 + NO_ASYNC_WEB_SERVER)
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
            // uri() is percent-encoded on the sync server — decode it so names
            // with spaces / UTF-8 match the real file (the async path decodes itself).
            HandleFileRead(_server.urlDecode(_server.uri()));
        });
        _server.on("/", [this]() { HandleFileRead(F("/index_nosd.html")); });
#endif
#if defined(ESP32) && !defined(NO_ASYNC_WEB_SERVER)
    // NOTE: no "/format" route on the ESP32 async path — formatting the FS
    // synchronously inside an async callback trips the task watchdog. Format
    // out-of-band (e.g. over serial / OTA) on ESP32 async builds.
#else
        _server.on(F("/format"), HTTP_GET, [this]() {
            if (!DoAuth()) return;
        Logger.Log_P(ILogger::LvlInfo, PSTR("Formatting file system"));
            LIONWIFI_FS.format();
            _server.send(200, __text_plain__F, F(LIONWIFI_FS_NAME " formatted"));
        Logger.Log_P(ILogger::LvlInfo, PSTR("Format complete"));
    });
#endif

    }
};
