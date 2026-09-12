#pragma once

// DirEntry — one fixed-capacity directory entry used by FsBrowser's listings.
// Plain POD (no heap members) so it is trivially copyable and safe to sort with
// qsort / store in a memcpy-growing Array.
#include <Arduino.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>

struct DirEntry
{
    // Max stored name length, excluding the NUL. Longer names are truncated in
    // listings — and a truncated name then breaks its tail/download/delete links.
    // Override with -D LIONWIFI_NAME_MAX=N (costs N+1 bytes per listed entry; SD
    // listings hold up to ~120 entries).
    //
    // The default follows the filesystem, because on SPIFFS the extra bytes can
    // never be used: SPIFFS caps object names at 31 characters, so a 64-byte buffer
    // is half padding on every entry — and SPIFFS is what the small ESP8266 nodes
    // run, exactly where a listing of 50 files costs real heap. LittleFS and FAT/SD
    // do allow long names, so they keep the generous default; an SD build keeps it
    // too even alongside SPIFFS, since the same struct lists both.
#ifndef LIONWIFI_NAME_MAX
#if defined(USE_SPIFFS) && !defined(USE_SD_CARD)
#define LIONWIFI_NAME_MAX 31
#else
#define LIONWIFI_NAME_MAX 63
#endif
#endif
    const static int MaxSize = LIONWIFI_NAME_MAX;
    DirEntry(const char* name = NULL)
    {
        if (name)
        {
            if (name[0] == '/')
                strncpy(fullName, name+1, MaxSize);
            else
                strncpy(fullName, name, MaxSize);
        }
        else
            fullName[0] = 0;
        fullName[MaxSize] = 0;
        char *p = strchr(fullName, '.');
        if (p)
            dotPos = p - fullName;
        else
            dotPos = -1;
    }
    const char *Ext()
    {
        if (dotPos < 0)
            return fullName + MaxSize; // points at the guaranteed NUL → empty string
        return fullName + dotPos + 1;
    }
    char fullName[MaxSize+1];
    size_t size = 0;
#ifdef USE_FILE_TIME
    time_t creationTime = 0, writeTime = 0;
#endif
    int dotPos = -1;
    bool isFolder = false;
};
