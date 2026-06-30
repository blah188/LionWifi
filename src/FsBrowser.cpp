#include "DirEntry.h"

// Single definition site for the shared PROGMEM MIME strings declared extern in
// FsBrowser.h (this .cpp is the one translation unit that owns them).
extern const char __text_plain__P[] PROGMEM = "text/plain";
extern const char __text_html__P[] PROGMEM = "text/html";
extern const char __slash_favicon_ico__P[] PROGMEM = "/favicon.ico";

// qsort comparator: folders last, then by extension, then by name.
int DirEntrySort(const void *cmp1, const void *cmp2)
{
    DirEntry *a = (DirEntry *)cmp1;
    DirEntry *b = (DirEntry *)cmp2;
    if (a->isFolder && !b->isFolder)
        return 1;
    if (!a->isFolder && b->isFolder)
        return -1;

    int r1 = strcmp(a->Ext(), b->Ext());
    if (r1 != 0)
        return r1;
    return strcmp(a->fullName, b->fullName);
}
