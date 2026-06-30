#include "DirEntry.h"

#ifdef ESP32
#include <AsyncTCP.h>
//#include <ESPAsyncWebSrv.h>
#include <ESPAsyncWebServer.h>
#else
#include <ESP8266WebServer.h>
#endif

extern const char __text_plain__P[] PROGMEM = "text/plain";
extern const char __text_html__P[] PROGMEM = "text/html";
extern const char __slash_favicon_ico__P[] PROGMEM = "/favicon.ico";

int sort(const void *cmp1, const void *cmp2)
{
    // Need to cast the void * to int *
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

#ifdef ESP32
void DebugDumpRequest(AsyncWebServerRequest *request)
{
    Serial.println(request->version());       // uint8_t: 0 = HTTP/1.0, 1 = HTTP/1.1
    Serial.println(request->method());        // enum:    HTTP_GET, HTTP_POST, HTTP_DELETE, HTTP_PUT, HTTP_PATCH, HTTP_HEAD, HTTP_OPTIONS
    Serial.println(request->url());           // String:  URL of the request (not including host, port or GET parameters)
    Serial.println(request->host());          // String:  The requested host (can be used for virtual hosting)
    Serial.println(request->contentType());   // String:  ContentType of the request (not avaiable in Handler::canHandle)
    Serial.println(request->contentLength()); // size_t:  ContentLength of the request (not avaiable in Handler::canHandle)
    Serial.println(request->multipart());     // bool:    True if the request has content type "multipart"

    //List all collected headers
    int headers = request->headers();
    int i;
    for (i = 0; i < headers; i++)
    {
        AsyncWebHeader *h = request->getHeader(i);
        Serial.printf("HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
    }
    // //get specific header by name
    // if (request->hasHeader("MyHeader"))
    // {
    //     AsyncWebHeader *h = request->getHeader("MyHeader");
    //     Serial.printf("MyHeader: %s\n", h->value().c_str());
    // }
    // //List all collected headers (Compatibility)
    // int headers = request->headers();
    // int i;
    // for (i = 0; i < headers; i++)
    // {
    //     Serial.printf("HEADER[%s]: %s\n", request->headerName(i).c_str(), request->header(i).c_str());
    // }
    // //get specific header by name (Compatibility)
    // if (request->hasHeader("MyHeader"))
    // {
    //     Serial.printf("MyHeader: %s\n", request->header("MyHeader").c_str());
    // }

    //List all parameters
    int params = request->params();
    for (int i = 0; i < params; i++)
    {
        AsyncWebParameter *p = request->getParam(i);
        if (p->isFile())
        { //p->isPost() is also true
            Serial.printf("FILE[%s]: %s, size: %u\n", p->name().c_str(), p->value().c_str(), p->size());
        }
        else if (p->isPost())
        {
            Serial.printf("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
        }
        else
        {
            Serial.printf("GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
        }
    }

    AsyncWebParameter *p = NULL;

    //Check if GET parameter exists
    if (request->hasParam("download"))
        p = request->getParam("download");

    //Check if POST (but not File) parameter exists
    if (request->hasParam("download", true))
        p = request->getParam("download", true);

    //Check if FILE was uploaded
    if (request->hasParam("download", true, true))
        p = request->getParam("download", true, true);

    if (p)
        Serial.printf("DNLD: %s %s\n", p->name().c_str(), p->value().c_str());

    //List all parameters (Compatibility)
    int args = request->args();
    for (int i = 0; i < args; i++)
    {
        Serial.printf("ARG[%s]: %s\n", request->argName(i).c_str(), request->arg(i).c_str());
    }

    // //Check if parameter exists (Compatibility)
    // if (request->hasArg("download"))
    //     String arg = request->arg("download");
}
#endif