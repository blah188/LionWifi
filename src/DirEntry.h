#pragma once
#include <Arduino.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>

struct DirEntry
{
#ifdef SDFAT    
    const static int MaxSize = 63;
#else
    const static int MaxSize = 31;
#endif
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
            return fullName + MaxSize;
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
