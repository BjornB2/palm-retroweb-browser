/*
 * A small HTTP/HTML companion browser for RetroModem on Palm OS 4.
 *
 * Networking approach adapted from PalmGET by Fred (MIT, 2021).
 * All new code is MIT licensed; see LICENSE.
 */

#include <PalmOS.h>
#include <Field.h>
#include <StdIOPalm.h>
#include <Unix/sys_socket.h>
#include "resources.h"
#include "gif.h"
#include "document.h"

Err errno;

#ifdef RETROWEB_BUILD
#define APP_NAME             "RetroWEB Browser"
#define APP_VERSION          "0.1.22"
#define USER_AGENT           "RetroWEBBrowser/0.1.22 (PalmOS 4)"
#define PREF_CREATOR         'RtWB'
#else
#define APP_NAME             "RetroModem Browser"
#define APP_VERSION          "0.1.22"
#define USER_AGENT           "RetroModemBrowser/0.1.22 (PalmOS 4)"
#define PREF_CREATOR         'RwBr'
#endif
#define DEFAULT_HOME_URL     "http://retroweb.bjrno.com/?img=inline"
#define HTTP_PORT            80
#define ADDRESS_MAX          255
#define HISTORY_MAX          8
#define RECEIVE_CHUNK        1024
#define PAGE_MAX             60000
#define BOOKMARK_MAX         12
#define BOOKMARK_TITLE_MAX   39
#define PREF_ID              1
#define PREF_VERSION         2
#define DeferredHomeEvent    (firstUserEvent + 1)

typedef struct {
    Char title[BOOKMARK_TITLE_MAX + 1];
    Char url[ADDRESS_MAX + 1];
} BookmarkType;

typedef struct {
    UInt16 version;
    UInt16 bookmarkCount;
    BookmarkType bookmarks[BOOKMARK_MAX];
    UInt16 historyCount;
    Char history[HISTORY_MAX][ADDRESS_MAX + 1];
    UInt8 imagesEnabled;
    Char homeUrl[ADDRESS_MAX + 1];
} SavedPreferencesType;

static Char gCurrentUrl[ADDRESS_MAX + 1];
static Char gHomeUrl[ADDRESS_MAX + 1];
static Char gHistory[HISTORY_MAX][ADDRESS_MAX + 1];
static UInt16 gHistoryCount = 0;
static UInt16 gHistoryPosition = 0;
static Document *gDocument = NULL;
static BookmarkType gBookmarks[BOOKMARK_MAX];
static UInt16 gBookmarkCount = 0;
static Boolean gPlacesShowsHistory = false;
static Char *gPlaceLabels[BOOKMARK_MAX];
static Int16 gEditingControl = -1;
static Boolean gImagesEnabled = true;
static Char gLastFetchUrl[512];
static UInt32 gLastResponseBytes = 0;
static UInt32 gLastExpectedBytes = 0;
static UInt32 gHttpExpectedBytes = 0;
static Boolean gHttpResponseLimited = false;
static Boolean gLastResponseLimited = false;
static Boolean gProgressiveReceive = false;
static Boolean gNetworkBusy = false;
static Boolean gAbortRequested = false;
static Boolean gLastRequestAborted = false;
static Boolean gStopRequested = false;
static UInt16 gImageAttemptCount = 0;
static UInt16 gImageLoadedCount = 0;
static UInt16 gImageFailureCount = 0;
static Char gLastImageError[64];
static Boolean gPendingNavigation = false;
static Char gPendingNavigationUrl[ADDRESS_MAX + 1];
static UInt16 gPendingMenuItem = 0;
static Boolean gDisconnectRequested = false;
static Char gStatusScratch[40];
/* Large temporary buffers must not live on the tiny Palm OS 68K stack. */
static Char gWorkUrl[1024];
static Char gPostBody[1024];
static Char gPostAction[ADDRESS_MAX + 1];
static Char gVisibleUrl[ADDRESS_MAX + 1];
static Char gHttpPath[1024];
static Char gHttpRequest[1400];
static Char gHttpChunk[RECEIVE_CHUNK];
static Char gHttpRedirect[1024];

static void SetStatus(const Char *text);
static void SetAddress(const Char *url);
static void SetPageText(const Char *text);
static void PushHistory(const Char *url);
static Boolean Navigate(const Char *url, Boolean addHistory);
static Boolean MainFormHandleEvent(EventType *eventP);
static Boolean PlacesFormHandleEvent(EventType *eventP);
static Boolean InputFormHandleEvent(EventType *eventP);
static Boolean PreferencesFormHandleEvent(EventType *eventP);
static Boolean AddressFormHandleEvent(EventType *eventP);
static Boolean SplitHttpUrl(const Char *url, Char *host, UInt16 hostCapacity,
                            Char *path, UInt16 pathCapacity);

static Boolean StartsWith(const Char *text, const Char *prefix)
{
    return StrNCompare(text, prefix, StrLen(prefix)) == 0;
}

static void SafeCopy(Char *dest, const Char *src, UInt16 capacity)
{
    UInt16 length;
    if (capacity == 0) return;
    length = StrLen(src);
    if (length >= capacity) length = capacity - 1;
    MemMove(dest, src, length);
    dest[length] = '\0';
}

static FieldType *GetField(UInt16 id)
{
    FormType *formP;
    UInt16 index;
    formP = FrmGetActiveForm();
    if (!formP) return NULL;
    index = FrmGetObjectIndex(formP, id);
    if (index == frmInvalidObjectId) return NULL;
    return (FieldType *)FrmGetObjectPtr(formP, index);
}

static Boolean GetPageBounds(RectangleType *bounds)
{
    FormType *formP;
    UInt16 index;
    formP = FrmGetActiveForm();
    if (!formP || FrmGetFormId(formP) != MainForm) return false;
    index = FrmGetObjectIndex(formP, PageGadget);
    if (index == frmInvalidObjectId) return false;
    FrmGetObjectBounds(formP, index, bounds);
    return true;
}

static void DrawDocument(void)
{
    RectangleType bounds;
    FormType *formP;
    UInt16 index;
    ScrollBarType *scrollP;
    Int32 maximum;
    if (gDocument && GetPageBounds(&bounds)) {
        DocumentDraw(gDocument, &bounds);
        formP = FrmGetActiveForm();
        index = FrmGetObjectIndex(formP, PageScrollBar);
        if (index != frmInvalidObjectId) {
            scrollP = (ScrollBarType *)FrmGetObjectPtr(formP, index);
            maximum = gDocument->contentHeight > bounds.extent.y ?
                      gDocument->contentHeight - bounds.extent.y : 0;
            SclSetScrollBar(scrollP, gDocument->scrollY, 0, (Int16)maximum,
                            bounds.extent.y > 1 ? bounds.extent.y - 1 : 1);
        }
    }
}

/* Palm OS turns taps on a standard scrollbar track into page jumps before the
 * form handler sees them.  Intercept the pen directly so the full track maps
 * to the full document and dragging gives pixel-level positioning. */
static Boolean ScrollFromPen(Coord screenX, Coord screenY, Boolean trackPen)
{
    RectangleType bounds;
    Int32 maximum;
    Int32 target;
    Boolean penDown;
    Coord penX;
    Coord penY;
    Coord lastY;

    if (!gDocument || screenX < 148 || !GetPageBounds(&bounds) ||
        screenY < bounds.topLeft.y ||
        screenY >= bounds.topLeft.y + bounds.extent.y) return false;

    penX = screenX;
    penY = screenY;
    lastY = -1;
    penDown = true;
    do {
        if (penY < bounds.topLeft.y) penY = bounds.topLeft.y;
        if (penY >= bounds.topLeft.y + bounds.extent.y)
            penY = bounds.topLeft.y + bounds.extent.y - 1;
        if (penY != lastY) {
            maximum = gDocument->contentHeight > bounds.extent.y ?
                      gDocument->contentHeight - bounds.extent.y : 0;
            target = bounds.extent.y > 1 ?
                     ((Int32)(penY - bounds.topLeft.y) * maximum) /
                     (bounds.extent.y - 1) : 0;
            gDocument->scrollY = (Coord)target;
            DrawDocument();
            lastY = penY;
        }
        if (!trackPen) break;
        EvtResetAutoOffTimer();
        EvtGetPen(&penX, &penY, &penDown);
    } while (penDown);
    return true;
}

static void QueuePendingMenuEvent(void)
{
    EventType event;
    if (!gPendingMenuItem) return;
    MemSet(&event, sizeof(event), 0);
    event.eType = menuEvent;
    event.data.menu.itemID = gPendingMenuItem;
    gPendingMenuItem = 0;
    EvtAddEventToQueue(&event);
}

static void SelectMenuWhileLoading(UInt16 itemID)
{
    if (itemID == MenuDisconnect) {
        gDisconnectRequested = true;
        gPendingNavigation = false;
        gPendingMenuItem = 0;
        gAbortRequested = true;
        SetStatus("Cancelling - closing connection...");
        return;
    }
    gPendingNavigation = false;
    gPendingMenuItem = itemID;
    gAbortRequested = true;
    SetStatus("Stopping load for menu command...");
}

static void PumpLoadingEvents(void)
{
    EventType event;
    UInt16 menuError;
    RectangleType bounds;
    for (;;) {
        EvtGetEvent(&event, 0);
        if (event.eType == nilEvent) break;
        if (event.eType == appStopEvent) {
            gAbortRequested = true;
            gStopRequested = true;
            continue;
        }
        if (event.eType == menuEvent) {
            SelectMenuWhileLoading(event.data.menu.itemID);
            continue;
        }
        if (event.eType == keyDownEvent && GetPageBounds(&bounds)) {
            if (event.data.keyDown.chr == pageUpChr) {
                DocumentScroll(gDocument, -bounds.extent.y + 12, bounds.extent.y);
                DrawDocument();
            } else if (event.data.keyDown.chr == pageDownChr) {
                DocumentScroll(gDocument, bounds.extent.y - 12, bounds.extent.y);
                DrawDocument();
            } else {
                if (SysHandleEvent(&event)) continue;
                if (MenuHandleEvent(NULL, &event, &menuError)) continue;
            }
        } else if ((event.eType == penDownEvent || event.eType == penMoveEvent) &&
                   ScrollFromPen(event.screenX, event.screenY, false)) {
        } else if (event.eType == sclRepeatEvent &&
                   event.data.sclRepeat.scrollBarID == PageScrollBar) {
            gDocument->scrollY = event.data.sclRepeat.newValue;
            DrawDocument();
        } else if (event.eType == penDownEvent && GetPageBounds(&bounds) &&
                   RctPtInRectangle(event.screenX, event.screenY, &bounds)) {
            const Char *link;
            link = DocumentLinkAt(gDocument, event.screenX, event.screenY, &bounds);
            if (link) {
                DocumentHighlightLinkAt(gDocument, event.screenX, event.screenY, &bounds);
                SafeCopy(gPendingNavigationUrl, link, sizeof(gPendingNavigationUrl));
                gPendingNavigation = true;
                gAbortRequested = true;
                SetStatus("Opening link - stopping current load...");
            }
        } else {
            if (SysHandleEvent(&event)) continue;
            if (MenuHandleEvent(NULL, &event, &menuError)) continue;
        }
    }
}

static Boolean LoadingEventPump(void *context)
{
    PumpLoadingEvents();
    return !gAbortRequested;
}

static void ReplaceFieldText(FieldType *fieldP, const Char *text)
{
    MemHandle oldH;
    MemHandle newH;
    Char *locked;
    UInt32 length;

    if (!fieldP) return;
    oldH = FldGetTextHandle(fieldP);
    FldSetTextHandle(fieldP, NULL);
    if (oldH) MemHandleFree(oldH);

    length = StrLen(text);
    newH = MemHandleNew(length + 1);
    if (!newH) return;
    locked = (Char *)MemHandleLock(newH);
    MemMove(locked, text, length + 1);
    MemHandleUnlock(newH);
    FldSetTextHandle(fieldP, newH);
    FldSetScrollPosition(fieldP, 0);
    FldDrawField(fieldP);
}

static void SetStatus(const Char *text)
{
    FormType *formP;
    UInt16 index;
    RectangleType bounds;
    formP = FrmGetActiveForm();
    if (!formP) return;
    index = FrmGetObjectIndex(formP, StatusLabel);
    if (index == frmInvalidObjectId) return;
    FrmGetObjectBounds(formP, index, &bounds);
    {
        RGBColorType black;
        RGBColorType white;
        IndexedColorType oldFore;
        IndexedColorType oldBack;
        IndexedColorType oldText;
        black.index = 0; black.r = 0; black.g = 0; black.b = 0;
        white.index = 0; white.r = 255; white.g = 255; white.b = 255;
        oldFore = WinSetForeColor(WinRGBToIndex(&black));
        oldBack = WinSetBackColor(WinRGBToIndex(&black));
        oldText = WinSetTextColor(WinRGBToIndex(&white));
        WinFillRectangle(&bounds, 0);
        WinDrawTruncChars(text, StrLen(text), bounds.topLeft.x + 3,
                         bounds.topLeft.y + 2, bounds.extent.x - 6);
        WinSetTextColor(oldText);
        WinSetBackColor(oldBack);
        WinSetForeColor(oldFore);
    }
}

static void SetAddress(const Char *url)
{
    SafeCopy(gCurrentUrl, url, sizeof(gCurrentUrl));
}

static void SetPageText(const Char *text)
{
    if (!gDocument) return;
    DocumentParse(gDocument, text, gCurrentUrl, NULL, NULL, NULL, NULL);
    DrawDocument();
}


static Boolean SplitHttpUrl(const Char *url, Char *host, UInt16 hostCapacity,
                            Char *path, UInt16 pathCapacity)
{
    const Char *start;
    const Char *slash;
    UInt16 hostLength;
    if (!StartsWith(url, "http://")) return false;
    start = url + 7;
    slash = StrChr(start, '/');
    if (!slash) {
        SafeCopy(host, start, hostCapacity);
        SafeCopy(path, "/", pathCapacity);
        return host[0] != '\0';
    }
    hostLength = (UInt16)(slash - start);
    if (hostLength == 0 || hostLength >= hostCapacity) return false;
    MemMove(host, start, hostLength);
    host[hostLength] = '\0';
    SafeCopy(path, slash, pathCapacity);
    return true;
}

static Boolean MakeFetchUrl(const Char *original, Char *result, UInt16 capacity)
{
#ifdef RETROWEB_BUILD
    static const Char hex[] = "0123456789ABCDEF";
    const Char *prefix;
    UInt16 used;
    UInt8 c;
    if (StartsWith(original, "http://retroweb.bjrno.com/") ||
        StrCompare(original, "http://retroweb.bjrno.com") == 0) {
        if (StrLen(original) + 1 > capacity) return false;
        SafeCopy(result, original, capacity);
        return true;
    }
    prefix = "http://retroweb.bjrno.com/proxy.php?url=";
    if (StrLen(prefix) + 12 >= capacity) return false;
    SafeCopy(result, prefix, capacity);
    used = StrLen(result);
    while (*original) {
        c = (UInt8)*original++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            if (used + 1 >= capacity) return false;
            result[used++] = (Char)c;
        } else {
            if (used + 3 >= capacity) return false;
            result[used++] = '%';
            result[used++] = hex[c >> 4];
            result[used++] = hex[c & 15];
        }
    }
    if (used + 12 >= capacity) return false;
    StrCopy(result + used, "&img=inline");
    return true;
#else
    if (StrLen(original) + 1 > capacity) return false;
    SafeCopy(result, original, capacity);
    return true;
#endif
}

static Err OpenNetwork(void)
{
    UInt16 ifErr;
    Err err;
    UInt8 allInterfacesUp;
    if (AppNetRefnum != sysInvalidRefNum && AppNetRefnum != 0) {
        AppNetTimeout = (Int32)SysTicksPerSecond() * 30;
        return errNone;
    }
    if (AppNetRefnum == sysInvalidRefNum || AppNetRefnum == 0) {
        err = SysLibFind("Net.lib", &AppNetRefnum);
        if (err) return err;
        ifErr = 0;
        err = NetLibOpen(AppNetRefnum, &ifErr);
        if (err == netErrAlreadyOpen) err = errNone;
        if (err || ifErr) {
            if (!err) err = (Err)ifErr;
            AppNetRefnum = sysInvalidRefNum;
            return err;
        }
    }
    AppNetTimeout = (Int32)SysTicksPerSecond() * 30;
    allInterfacesUp = false;
    ifErr = 0;
    err = NetLibConnectionRefresh(AppNetRefnum, true, &allInterfacesUp, &ifErr);
    if (err) return err;
    if (ifErr) return (Err)ifErr;
    if (!allInterfacesUp) return netErrInterfaceDown;
    return errNone;
}

static void CloseNetwork(Boolean immediate)
{
    if (AppNetRefnum != sysInvalidRefNum && AppNetRefnum != 0) {
        NetLibClose(AppNetRefnum, immediate ? 1 : 0);
        AppNetRefnum = sysInvalidRefNum;
    }
}

static Int16 FindHeaderEnd(const Char *data, UInt32 length)
{
    UInt32 i;
    for (i = 0; i + 3 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') return (Int16)(i + 4);
    }
    return -1;
}

static UInt32 FindContentLength(const Char *headers, UInt16 headerLength)
{
    const Char *line;
    const Char *end;
    line = headers;
    while (line < headers + headerLength) {
        end = StrStr(line, "\r\n");
        if (!end || end > headers + headerLength) break;
        if (StrNCaselessCompare(line, "Content-Length:", 15) == 0) {
            line += 15;
            while (*line == ' ' || *line == '\t') line++;
            return (UInt32)StrAToI(line);
        }
        line = end + 2;
    }
    return 0;
}

static Boolean HttpRequestInternal(const Char *url, const Char *method, const Char *postBody,
                                   Char **bodyOut, UInt32 *bodyLengthOut,
                                   Char *errorText, UInt16 errorCapacity,
                                   UInt8 redirectDepth)
{
    Char host[128];
    Char lengthText[16];
    Char *response;
    Char *grown;
    UInt32 used;
    UInt32 capacity;
    Int16 headerEnd;
    Int16 received;
    int socketDesc;
    struct sockaddr_in server;
    struct hostent *hp;
    Err err;
    Boolean isRedirect;
    Char *line;
    Char *lineEnd;
    UInt16 redirectLength;
    UInt8 receiveErrors;
    UInt32 progressDrawn;
    UInt32 lastDataTick;
    Int32 normalTimeout;
    UInt16 nonBlocking;
    Coord savedScroll;
    RectangleType progressBounds;

    *bodyOut = NULL;
    *bodyLengthOut = 0;
    gHttpExpectedBytes = 0;
    gHttpResponseLimited = false;
    headerEnd = -1;
    progressDrawn = 0;
    if (!SplitHttpUrl(url, host, sizeof(host), gHttpPath, sizeof(gHttpPath))) {
        SafeCopy(errorText, "Only HTTP URLs are supported.", errorCapacity);
        return false;
    }
    SetStatus("Opening network connection...");
    err = OpenNetwork();
    if (err) {
        SafeCopy(errorText, "Could not open the Palm network connection.", errorCapacity);
        return false;
    }

    SetStatus("Preparing HTTP connection...");
    socketDesc = socket(AF_INET, SOCK_STREAM, 0);
    if (socketDesc < 0) {
        SafeCopy(errorText, "Could not create network socket.", errorCapacity);
        return false;
    }
    SetStatus("Resolving server name...");
    normalTimeout = AppNetTimeout;
    AppNetTimeout = (Int32)SysTicksPerSecond();
    lastDataTick = TimGetTicks();
    hp = NULL;
    do {
        hp = gethostbyname(host);
        if (hp) break;
        PumpLoadingEvents();
        if (gAbortRequested) {
            AppNetTimeout = normalTimeout;
            close(socketDesc);
            SafeCopy(errorText, "Request cancelled.", errorCapacity);
            return false;
        }
    } while (TimGetTicks() - lastDataTick < (UInt32)SysTicksPerSecond() * 30);
    AppNetTimeout = normalTimeout;
    if (!hp) {
        close(socketDesc);
        SafeCopy(errorText, "Host name could not be resolved.", errorCapacity);
        return false;
    }
    MemSet(&server, sizeof(server), 0);
    bcopy(hp->h_addr, (Char *)&server.sin_addr, hp->h_length);
    server.sin_family = AF_INET;
    server.sin_port = htons(HTTP_PORT);
    SetStatus("Connecting to web server...");
    if (connect(socketDesc, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(socketDesc);
        SafeCopy(errorText, "Could not connect to web server.", errorCapacity);
        return false;
    }
    if (postBody) {
        StrIToA(lengthText, StrLen(postBody));
        StrPrintF(gHttpRequest,
                  "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nAccept: text/html,image/gif\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %s\r\nConnection: close\r\n\r\n",
                  method, gHttpPath, host, lengthText);
    } else {
        StrPrintF(gHttpRequest,
                  "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nAccept: text/html,image/gif\r\nConnection: close\r\n\r\n",
                  method, gHttpPath, host);
    }
    SetStatus("Sending page request...");
    if (send(socketDesc, gHttpRequest, StrLen(gHttpRequest), 0) < 0) {
        close(socketDesc);
        SafeCopy(errorText, "Could not send HTTP request.", errorCapacity);
        return false;
    }
    if (postBody && send(socketDesc, (Char *)postBody, StrLen(postBody), 0) < 0) {
        close(socketDesc);
        SafeCopy(errorText, "Could not send form data.", errorCapacity);
        return false;
    }

    capacity = 4096;
    used = 0;
    response = (Char *)MemPtrNew(capacity);
    if (!response) {
        close(socketDesc);
        SafeCopy(errorText, "Not enough memory for response.", errorCapacity);
        return false;
    }
    SetStatus("Waiting for page data...");
    nonBlocking = 1;
    if (setsockopt(socketDesc, SOL_SOCKET, netSocketOptSockNonBlocking,
                   &nonBlocking, sizeof(nonBlocking)) < 0) {
        close(socketDesc);
        MemPtrFree(response);
        SafeCopy(errorText, "Could not enable responsive network mode.", errorCapacity);
        return false;
    }
    receiveErrors = 0;
    lastDataTick = TimGetTicks();
    normalTimeout = AppNetTimeout;
    AppNetTimeout = (Int32)SysTicksPerSecond();
    for (;;) {
        PumpLoadingEvents();
        if (gAbortRequested) {
            close(socketDesc);
            MemPtrFree(response);
            AppNetTimeout = normalTimeout;
            SafeCopy(errorText, "Request cancelled.", errorCapacity);
            return false;
        }
        received = recv(socketDesc, gHttpChunk, sizeof(gHttpChunk), 0);
        if (received == 0) break;
        if (received < 0) {
            receiveErrors++;
            PumpLoadingEvents();
            if (gAbortRequested) {
                close(socketDesc);
                MemPtrFree(response);
                AppNetTimeout = normalTimeout;
                SafeCopy(errorText, "Request cancelled.", errorCapacity);
                return false;
            }
            if (TimGetTicks() - lastDataTick < (UInt32)SysTicksPerSecond() * 30) {
                SysTaskDelay(1);
                continue;
            }
            break;
        }
        receiveErrors = 0;
        lastDataTick = TimGetTicks();
        if (used + (UInt32)received + 1 > PAGE_MAX) {
            gHttpResponseLimited = true;
            break;
        }
        if (used + (UInt32)received + 1 > capacity) {
            UInt32 newCapacity;
            newCapacity = capacity * 2;
            if (newCapacity > PAGE_MAX) newCapacity = PAGE_MAX;
            grown = (Char *)MemPtrNew(newCapacity);
            if (!grown) break;
            MemMove(grown, response, used);
            MemPtrFree(response);
            response = grown;
            capacity = newCapacity;
        }
        MemMove(response + used, gHttpChunk, received);
        used += received;
        response[used] = '\0';
        if (headerEnd < 0) headerEnd = FindHeaderEnd(response, used);
        if (headerEnd >= 0 && !gHttpExpectedBytes)
            gHttpExpectedBytes = FindContentLength(response, (UInt16)headerEnd);
        if (gProgressiveReceive) {
            if (headerEnd >= 0 && used - headerEnd >= progressDrawn + 1024) {
                progressDrawn = used - headerEnd;
                if (gHttpExpectedBytes)
                    StrPrintF(gStatusScratch, "Receiving page: %lu / %lu KB",
                              (progressDrawn + 1023) / 1024,
                              (gHttpExpectedBytes + 1023) / 1024);
                else
                    StrPrintF(gStatusScratch, "Receiving page: %lu KB",
                              (progressDrawn + 1023) / 1024);
                SetStatus(gStatusScratch);
                savedScroll = gDocument->scrollY;
                DocumentParse(gDocument, response + headerEnd, url, NULL, NULL,
                              LoadingEventPump, NULL);
                gDocument->scrollY = savedScroll;
                if (GetPageBounds(&progressBounds))
                    DocumentScroll(gDocument, 0, progressBounds.extent.y);
                DrawDocument();
            }
        }
        PumpLoadingEvents();
    }
    close(socketDesc);
    AppNetTimeout = normalTimeout;
    response[used] = '\0';
    if (headerEnd < 0) headerEnd = FindHeaderEnd(response, used);
    if (headerEnd < 0) {
        MemPtrFree(response);
        SafeCopy(errorText, "Invalid HTTP response.", errorCapacity);
        return false;
    }
    if (!gHttpExpectedBytes)
        gHttpExpectedBytes = FindContentLength(response, (UInt16)headerEnd);
    isRedirect = StrNCompare(response, "HTTP/1.", 7) == 0 &&
                 (StrStr(response, " 301 ") || StrStr(response, " 302 ") ||
                  StrStr(response, " 303 ") || StrStr(response, " 307 ") ||
                  StrStr(response, " 308 "));
    if (isRedirect) {
        line = response;
        gHttpRedirect[0] = '\0';
        while (line < response + headerEnd) {
            lineEnd = StrStr(line, "\r\n");
            if (!lineEnd) break;
            if (StrNCaselessCompare(line, "Location:", 9) == 0) {
                line += 9;
                while (*line == ' ' || *line == '\t') line++;
                redirectLength = (UInt16)(lineEnd - line);
                if (redirectLength >= sizeof(gHttpRedirect)) redirectLength = sizeof(gHttpRedirect) - 1;
                MemMove(gHttpRedirect, line, redirectLength);
                gHttpRedirect[redirectLength] = '\0';
                break;
            }
            line = lineEnd + 2;
        }
        if (gHttpRedirect[0]) {
            if (gHttpRedirect[0] == '/') {
                UInt16 relativeLength;
                relativeLength = StrLen(gHttpRedirect);
                if (StrLen(host) + relativeLength + 8 < sizeof(gHttpRedirect)) {
                    MemMove(gHttpRedirect + 7 + StrLen(host), gHttpRedirect,
                            relativeLength + 1);
                    MemMove(gHttpRedirect + 7, host, StrLen(host));
                    MemMove(gHttpRedirect, "http://", 7);
                }
            }
            MemPtrFree(response);
            if (redirectDepth >= 5) {
                SafeCopy(errorText, "Too many HTTP redirects.", errorCapacity);
                return false;
            }
            return HttpRequestInternal(gHttpRedirect, "GET", NULL, bodyOut, bodyLengthOut,
                                       errorText, errorCapacity, redirectDepth + 1);
        }
    }
    *bodyLengthOut = used - headerEnd;
    if (!gHttpResponseLimited && gHttpExpectedBytes && *bodyLengthOut < gHttpExpectedBytes) {
        MemPtrFree(response);
        SafeCopy(errorText, "The response ended before the complete page arrived.", errorCapacity);
        return false;
    }
    *bodyOut = (Char *)MemPtrNew(*bodyLengthOut + 1);
    if (!*bodyOut) {
        MemPtrFree(response);
        SafeCopy(errorText, "Not enough memory for page.", errorCapacity);
        return false;
    }
    MemMove(*bodyOut, response + headerEnd, *bodyLengthOut);
    (*bodyOut)[*bodyLengthOut] = '\0';
    MemPtrFree(response);
    return true;
}

static Boolean HttpRequest(const Char *url, const Char *method, const Char *postBody,
                           Char **bodyOut, UInt32 *bodyLengthOut,
                           Char *errorText, UInt16 errorCapacity)
{
    Boolean result;
    gNetworkBusy = true;
    gAbortRequested = false;
    gLastRequestAborted = false;
    gDisconnectRequested = false;
    result = HttpRequestInternal(url, method, postBody, bodyOut, bodyLengthOut,
                                 errorText, errorCapacity, 0);
    if (gAbortRequested) {
        gLastRequestAborted = true;
        if (gDisconnectRequested || !gPendingNavigation) CloseNetwork(true);
    }
    gNetworkBusy = false;
    if (gStopRequested) {
        EventType stopEvent;
        MemSet(&stopEvent, sizeof(stopEvent), 0);
        stopEvent.eType = appStopEvent;
        EvtAddEventToQueue(&stopEvent);
        gStopRequested = false;
    }
    return result;
}

static Boolean HttpGet(const Char *url, Char **bodyOut, UInt32 *bodyLengthOut,
                       Char *errorText, UInt16 errorCapacity)
{
    return HttpRequest(url, "GET", NULL, bodyOut, bodyLengthOut,
                       errorText, errorCapacity);
}

static BitmapType *LoadGifForDocument(const Char *url, void *context)
{
    Char *body;
    UInt32 bodyLength;
    Char errorText[64];
    UInt16 width;
    UInt16 height;
    Err error;
    BitmapType *bitmap;
    Int32 normalTimeout;
    if (!gImagesEnabled || gLastRequestAborted) return NULL;
    gImageAttemptCount++;
    body = NULL;
    StrPrintF(gStatusScratch, "Loading image %u...", gImageAttemptCount);
    SetStatus(gStatusScratch);
    normalTimeout = AppNetTimeout;
    AppNetTimeout = (Int32)SysTicksPerSecond() * 8;
    if (!HttpGet(url, &body, &bodyLength, errorText, sizeof(errorText))) {
        AppNetTimeout = normalTimeout;
        gImageFailureCount++;
        SafeCopy(gLastImageError, errorText, sizeof(gLastImageError));
        return NULL;
    }
    AppNetTimeout = normalTimeout;
    if (bodyLength < 6 ||
        (MemCmp(body, "GIF87a", 6) != 0 && MemCmp(body, "GIF89a", 6) != 0)) {
        gImageFailureCount++;
        SafeCopy(gLastImageError, "Image is not GIF data.", sizeof(gLastImageError));
        MemPtrFree(body);
        return NULL;
    }
    bitmap = GifDecodeFirstFrame((UInt8 *)body, bodyLength, &width, &height, &error,
                                 LoadingEventPump, NULL);
    MemPtrFree(body);
    if (!bitmap) {
        gImageFailureCount++;
        SafeCopy(gLastImageError, "GIF could not be decoded.", sizeof(gLastImageError));
    } else gImageLoadedCount++;
    return bitmap;
}

static Boolean ReplaceWithImageDocument(const Char *html, const Char *baseUrl)
{
    Document *candidate;
    Document *previous;
    RectangleType bounds;
    Coord savedScroll;
    candidate = (Document *)MemPtrNew(sizeof(Document));
    if (!candidate) return false;
    DocumentInit(candidate);
    if (!DocumentParse(candidate, html, baseUrl, LoadGifForDocument, NULL,
                       LoadingEventPump, NULL)) {
        DocumentFree(candidate);
        MemPtrFree(candidate);
        return false;
    }
    savedScroll = gDocument->scrollY;
    previous = gDocument;
    gDocument = candidate;
    gDocument->scrollY = savedScroll;
    if (GetPageBounds(&bounds)) DocumentScroll(gDocument, 0, bounds.extent.y);
    DocumentFree(previous);
    MemPtrFree(previous);
    return true;
}

static void VisibleUrlFromLink(const Char *link, Char *output, UInt16 capacity)
{
    SafeCopy(output, link, capacity);
}

static Boolean IsTagName(const Char *name, const Char *expected)
{
    return StrCaselessCompare(name, expected) == 0;
}

static void AppendChar(Char *out, UInt32 *used, UInt32 capacity, Char c)
{
    if (*used + 1 < capacity) out[(*used)++] = c;
}

static void AppendNewline(Char *out, UInt32 *used, UInt32 capacity)
{
    if (*used > 0 && out[*used - 1] != '\n') AppendChar(out, used, capacity, '\n');
}

static Char *HtmlToText(const Char *html)
{
    UInt32 inputLength;
    UInt32 i;
    UInt32 used;
    UInt32 capacity;
    Char *out;
    Char tag[20];
    UInt16 tagUsed;
    Boolean inTag;
    Boolean closing;

    inputLength = StrLen(html);
    capacity = inputLength + 1;
    if (capacity > PAGE_MAX) capacity = PAGE_MAX;
    out = (Char *)MemPtrNew(capacity);
    if (!out) return NULL;
    used = 0;
    inTag = false;
    closing = false;
    tagUsed = 0;

    for (i = 0; i < inputLength && used + 1 < capacity; ++i) {
        Char c;
        c = html[i];
        if (!inTag && c == '<') {
            inTag = true;
            closing = false;
            tagUsed = 0;
        } else if (inTag) {
            if (tagUsed == 0 && c == '/') closing = true;
            else if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                tag[tagUsed] = '\0';
                if (c == '>') {
                    if (IsTagName(tag, "br") || IsTagName(tag, "p") ||
                        IsTagName(tag, "li") || IsTagName(tag, "h1") ||
                        IsTagName(tag, "h2") || IsTagName(tag, "h3") ||
                        IsTagName(tag, "hr") ||
                        (closing && (IsTagName(tag, "ul") || IsTagName(tag, "ol"))))
                        AppendNewline(out, &used, capacity);
                    inTag = false;
                }
            } else if (tagUsed + 1 < sizeof(tag)) {
                tag[tagUsed++] = c;
            }
        } else if (c == '&') {
            if (StrNCompare(html + i, "&amp;", 5) == 0) { AppendChar(out, &used, capacity, '&'); i += 4; }
            else if (StrNCompare(html + i, "&lt;", 4) == 0) { AppendChar(out, &used, capacity, '<'); i += 3; }
            else if (StrNCompare(html + i, "&gt;", 4) == 0) { AppendChar(out, &used, capacity, '>'); i += 3; }
            else if (StrNCompare(html + i, "&quot;", 6) == 0) { AppendChar(out, &used, capacity, '"'); i += 5; }
            else if (StrNCompare(html + i, "&nbsp;", 6) == 0) { AppendChar(out, &used, capacity, ' '); i += 5; }
            else AppendChar(out, &used, capacity, c);
        } else if (c == '\r') {
            /* Ignore CR; Palm fields use LF. */
        } else {
            AppendChar(out, &used, capacity, c);
        }
    }
    out[used] = '\0';
    return out;
}

static void PushHistory(const Char *url)
{
    UInt16 i;
    if (gHistoryPosition && StrCompare(gHistory[gHistoryPosition - 1], url) == 0) return;
    if (gHistoryPosition < gHistoryCount) gHistoryCount = gHistoryPosition;
    if (gHistoryCount == HISTORY_MAX) {
        for (i = 1; i < HISTORY_MAX; ++i)
            SafeCopy(gHistory[i - 1], gHistory[i], ADDRESS_MAX + 1);
        gHistoryCount--;
        if (gHistoryPosition) gHistoryPosition--;
    }
    SafeCopy(gHistory[gHistoryCount++], url, ADDRESS_MAX + 1);
    gHistoryPosition = gHistoryCount;
}

static void LoadPreferences(void)
{
    SavedPreferencesType *saved;
    UInt16 size;
    Int16 version;
    gImagesEnabled = true;
    SafeCopy(gHomeUrl, DEFAULT_HOME_URL, sizeof(gHomeUrl));
    size = sizeof(SavedPreferencesType);
    saved = (SavedPreferencesType *)MemPtrNew(size);
    if (!saved) return;
    MemSet(saved, size, 0);
    version = PrefGetAppPreferences(PREF_CREATOR, PREF_ID, saved, &size, true);
    if (version == PREF_VERSION && saved->version == PREF_VERSION) {
        gBookmarkCount = saved->bookmarkCount;
        if (gBookmarkCount > BOOKMARK_MAX) gBookmarkCount = BOOKMARK_MAX;
        MemMove(gBookmarks, saved->bookmarks, sizeof(gBookmarks));
        gHistoryCount = saved->historyCount;
        if (gHistoryCount > HISTORY_MAX) gHistoryCount = HISTORY_MAX;
        MemMove(gHistory, saved->history, sizeof(gHistory));
        gHistoryPosition = gHistoryCount;
        gImagesEnabled = saved->imagesEnabled != 0;
        if (saved->homeUrl[0])
            SafeCopy(gHomeUrl, saved->homeUrl, sizeof(gHomeUrl));
    }
    MemPtrFree(saved);
}

static void SavePreferences(void)
{
    SavedPreferencesType *saved;
    saved = (SavedPreferencesType *)MemPtrNew(sizeof(SavedPreferencesType));
    if (!saved) return;
    MemSet(saved, sizeof(SavedPreferencesType), 0);
    saved->version = PREF_VERSION;
    saved->bookmarkCount = gBookmarkCount;
    MemMove(saved->bookmarks, gBookmarks, sizeof(gBookmarks));
    saved->historyCount = gHistoryCount;
    MemMove(saved->history, gHistory, sizeof(gHistory));
    saved->imagesEnabled = gImagesEnabled ? 1 : 0;
    SafeCopy(saved->homeUrl, gHomeUrl, sizeof(saved->homeUrl));
    PrefSetAppPreferences(PREF_CREATOR, PREF_ID, PREF_VERSION, saved,
                          sizeof(SavedPreferencesType), true);
    MemPtrFree(saved);
}

static void AddCurrentBookmark(void)
{
    UInt16 i;
    BookmarkType *bookmark;
    if (!gCurrentUrl[0]) return;
    for (i = 0; i < gBookmarkCount; ++i) {
        if (StrCompare(gBookmarks[i].url, gCurrentUrl) == 0) {
            FrmCustomAlert(MessageAlert, "This page is already bookmarked.", "", "");
            return;
        }
    }
    if (gBookmarkCount >= BOOKMARK_MAX) {
        FrmCustomAlert(MessageAlert, "The bookmark list is full. Delete one first.", "", "");
        return;
    }
    bookmark = &gBookmarks[gBookmarkCount++];
    SafeCopy(bookmark->url, gCurrentUrl, sizeof(bookmark->url));
    if (gDocument && gDocument->title[0])
        SafeCopy(bookmark->title, gDocument->title, sizeof(bookmark->title));
    else SafeCopy(bookmark->title, gCurrentUrl, sizeof(bookmark->title));
    SavePreferences();
    FrmCustomAlert(MessageAlert, "Current page added to Bookmarks.", "", "");
}

static void OpenPlaces(Boolean history)
{
    gPlacesShowsHistory = history;
    FrmPopupForm(PlacesForm);
}

static UInt16 PlaceCount(void)
{
    return gPlacesShowsHistory ? gHistoryCount : gBookmarkCount;
}

static const Char *PlaceUrl(UInt16 index)
{
    if (gPlacesShowsHistory) {
        /* Show newest first. */
        if (index >= gHistoryCount) return NULL;
        return gHistory[gHistoryCount - 1 - index];
    }
    if (index >= gBookmarkCount) return NULL;
    return gBookmarks[index].url;
}

static void PopulatePlacesList(void)
{
    FormType *formP;
    ListType *listP;
    UInt16 count;
    UInt16 i;
    const Char *label;
    formP = FrmGetActiveForm();
    listP = (ListType *)FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, PlacesList));
    count = PlaceCount();
    for (i = 0; i < count; ++i) {
        if (gPlacesShowsHistory) label = PlaceUrl(i);
        else label = gBookmarks[i].title;
        gPlaceLabels[i] = (Char *)label;
    }
    LstSetListChoices(listP, gPlaceLabels, count);
    LstSetHeight(listP, count > 7 ? 7 : (count ? count : 1));
    if (count) LstSetSelection(listP, 0);
}

static void DeleteSelectedPlace(void)
{
    FormType *formP;
    ListType *listP;
    Int16 selected;
    UInt16 index;
    UInt16 i;
    formP = FrmGetActiveForm();
    listP = (ListType *)FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, PlacesList));
    selected = LstGetSelection(listP);
    if (selected == noListSelection) return;
    if (gPlacesShowsHistory) {
        index = gHistoryCount - 1 - (UInt16)selected;
        for (i = index + 1; i < gHistoryCount; ++i)
            SafeCopy(gHistory[i - 1], gHistory[i], ADDRESS_MAX + 1);
        if (gHistoryCount) gHistoryCount--;
        gHistoryPosition = gHistoryCount;
    } else {
        index = (UInt16)selected;
        for (i = index + 1; i < gBookmarkCount; ++i)
            MemMove(&gBookmarks[i - 1], &gBookmarks[i], sizeof(BookmarkType));
        if (gBookmarkCount) gBookmarkCount--;
    }
    SavePreferences();
    PopulatePlacesList();
    LstDrawList(listP);
}

static void OpenSelectedPlace(void)
{
    FormType *formP;
    ListType *listP;
    Int16 selected;
    Char url[ADDRESS_MAX + 1];
    const Char *source;
    formP = FrmGetActiveForm();
    listP = (ListType *)FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, PlacesList));
    selected = LstGetSelection(listP);
    if (selected == noListSelection) return;
    source = PlaceUrl((UInt16)selected);
    if (!source) return;
    SafeCopy(url, source, sizeof(url));
    FrmReturnToForm(MainForm);
    Navigate(url, true);
}

static Boolean Navigate(const Char *url, Boolean addHistory)
{
    Char normalized[ADDRESS_MAX + 1];
    Char errorText[96];
    Char *html;
    UInt32 htmlLength;

    if (!url || !url[0]) return false;
    if (StartsWith(url, "http://")
#ifdef RETROWEB_BUILD
        || StartsWith(url, "https://")
#endif
       ) SafeCopy(normalized, url, sizeof(normalized));
    else {
        if (StrLen(url) + 8 > sizeof(normalized)) return false;
        StrCopy(normalized, "http://");
        StrCat(normalized, url);
    }
    SetStatus("Address");
    SetAddress(normalized);
    if (addHistory) PushHistory(normalized);
    if (!MakeFetchUrl(normalized, gWorkUrl, sizeof(gWorkUrl))) {
        SetPageText("Address is too long.");
        return false;
    }
    SafeCopy(gLastFetchUrl, gWorkUrl, sizeof(gLastFetchUrl));
    SetStatus("Loading");
    EvtResetAutoOffTimer();
    html = NULL;
    gImageAttemptCount = 0;
    gImageLoadedCount = 0;
    gImageFailureCount = 0;
    gLastImageError[0] = '\0';
    gProgressiveReceive = true;
    if (!HttpGet(gWorkUrl, &html, &htmlLength, errorText, sizeof(errorText))) {
        gProgressiveReceive = false;
        if (gLastRequestAborted) {
            if (gPendingNavigation) {
                SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
                gPendingNavigation = false;
                gLastRequestAborted = false;
                return Navigate(gVisibleUrl, true);
            }
            SetStatus("Cancelled - disconnected");
            DrawDocument();
            QueuePendingMenuEvent();
            return false;
        }
        SetPageText(errorText);
        SetStatus("Error");
        return false;
    }
    gProgressiveReceive = false;
    gLastResponseBytes = htmlLength;
    gLastExpectedBytes = gHttpExpectedBytes;
    gLastResponseLimited = gHttpResponseLimited;
    SetStatus("Laying out page text...");
    if (!DocumentParse(gDocument, html, gWorkUrl, NULL, NULL,
                       LoadingEventPump, NULL)) {
        MemPtrFree(html);
        if (!gPendingNavigation) CloseNetwork(true);
        if (gPendingNavigation) {
            SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
            gPendingNavigation = false;
            gAbortRequested = false;
            return Navigate(gVisibleUrl, true);
        }
        SetStatus("Cancelled - disconnected");
        QueuePendingMenuEvent();
        return false;
    }
    DrawDocument();
    if (gImagesEnabled) {
        SetStatus("Loading inline images...");
        ReplaceWithImageDocument(html, gWorkUrl);
    }
    MemPtrFree(html);
    DrawDocument();
    if (gLastRequestAborted || gAbortRequested) {
        if (!gPendingNavigation) CloseNetwork(true);
        if (gPendingNavigation) {
            SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
            gPendingNavigation = false;
            gLastRequestAborted = false;
            gAbortRequested = false;
            return Navigate(gVisibleUrl, true);
        }
        SetStatus("Cancelled - disconnected");
        QueuePendingMenuEvent();
        return false;
    }
    if (!gImagesEnabled) SetStatus("Page loaded - images are off");
    else if (gImageAttemptCount)
        StrPrintF(gStatusScratch, "Page loaded - %u/%u images",
                  gImageLoadedCount, gImageAttemptCount), SetStatus(gStatusScratch);
    else SetStatus(gLastResponseLimited ? "Page loaded - 60 KB limit reached" : "Page loaded - no images found");
    return true;
}

static Boolean NavigatePost(const Char *url, const Char *postBody)
{
    Char normalized[ADDRESS_MAX + 1];
    Char errorText[96];
    Char *html;
    UInt32 htmlLength;
    if (!url || !url[0]) return false;
    if (StartsWith(url, "http://")) SafeCopy(normalized, url, sizeof(normalized));
    else return false;
    SetAddress(normalized);
    PushHistory(normalized);
    SetStatus("Sending");
    SafeCopy(gLastFetchUrl, normalized, sizeof(gLastFetchUrl));
    html = NULL;
    gImageAttemptCount = 0;
    gImageLoadedCount = 0;
    gImageFailureCount = 0;
    gLastImageError[0] = '\0';
    gProgressiveReceive = true;
    if (!HttpRequest(normalized, "POST", postBody, &html, &htmlLength,
                     errorText, sizeof(errorText))) {
        gProgressiveReceive = false;
        if (gLastRequestAborted) {
            if (gPendingNavigation) {
                SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
                gPendingNavigation = false;
                gLastRequestAborted = false;
                return Navigate(gVisibleUrl, true);
            }
            SetStatus("Cancelled - disconnected");
            DrawDocument();
            QueuePendingMenuEvent();
            return false;
        }
        SetPageText(errorText);
        SetStatus("Error");
        return false;
    }
    gProgressiveReceive = false;
    gLastResponseBytes = htmlLength;
    gLastExpectedBytes = gHttpExpectedBytes;
    gLastResponseLimited = gHttpResponseLimited;
    SetStatus("Laying out page text...");
    if (!DocumentParse(gDocument, html, normalized, NULL, NULL,
                       LoadingEventPump, NULL)) {
        MemPtrFree(html);
        if (!gPendingNavigation) CloseNetwork(true);
        if (gPendingNavigation) {
            SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
            gPendingNavigation = false;
            gAbortRequested = false;
            return Navigate(gVisibleUrl, true);
        }
        SetStatus("Cancelled - disconnected");
        QueuePendingMenuEvent();
        return false;
    }
    DrawDocument();
    if (gImagesEnabled) {
        SetStatus("Loading inline images...");
        ReplaceWithImageDocument(html, normalized);
    }
    MemPtrFree(html);
    DrawDocument();
    if (gLastRequestAborted || gAbortRequested) {
        if (!gPendingNavigation) CloseNetwork(true);
        if (gPendingNavigation) {
            SafeCopy(gVisibleUrl, gPendingNavigationUrl, sizeof(gVisibleUrl));
            gPendingNavigation = false;
            gLastRequestAborted = false;
            gAbortRequested = false;
            return Navigate(gVisibleUrl, true);
        }
        SetStatus("Cancelled - disconnected");
        QueuePendingMenuEvent();
        return false;
    }
    if (!gImagesEnabled) SetStatus("Page loaded - images are off");
    else if (gImageAttemptCount)
        StrPrintF(gStatusScratch, "Page loaded - %u/%u images",
                  gImageLoadedCount, gImageAttemptCount), SetStatus(gStatusScratch);
    else SetStatus(gLastResponseLimited ? "Page loaded - 60 KB limit reached" : "Page loaded - no images found");
    return true;
}

static void GoFromAddressField(void)
{
    FieldType *fieldP;
    Char *text;
    fieldP = GetField(AddressField);
    text = fieldP ? FldGetTextPtr(fieldP) : NULL;
    if (text) Navigate(text, true);
}

static void GoBack(void)
{
    if (gHistoryPosition < 2) return;
    gHistoryPosition--;
    Navigate(gHistory[gHistoryPosition - 1], false);
}

static void GoForward(void)
{
    if (gHistoryPosition >= gHistoryCount) return;
    Navigate(gHistory[gHistoryPosition], false);
    gHistoryPosition++;
}

static Boolean HandleMenu(UInt16 itemID)
{
    Char info[480];
    Char sizeText[16];
    Err networkError;
    switch (itemID) {
    case MenuAbout:
        FrmAlert(AboutAlert);
        return true;
    case MenuAddBookmark:
        AddCurrentBookmark();
        return true;
    case MenuBookmarks:
        OpenPlaces(false);
        return true;
    case MenuHistory:
        OpenPlaces(true);
        return true;
    case MenuPreferences:
        FrmPopupForm(PreferencesForm);
        return true;
    case MenuPageInfo:
        StrIToA(sizeText, gLastResponseBytes);
        StrCopy(info, gDocument && gDocument->title[0] ? gDocument->title : "Untitled page");
        StrCat(info, "\n\nAddress:\n");
        StrNCat(info, gCurrentUrl, 120);
        StrCat(info, "\n\nHTML bytes: ");
        StrCat(info, sizeText);
        StrCat(info, " / ");
        StrIToA(sizeText, gLastExpectedBytes);
        StrCat(info, sizeText);
        StrCat(info, "\nDocument height: ");
        StrIToA(sizeText, gDocument ? gDocument->contentHeight : 0);
        StrCat(info, sizeText);
        StrCat(info, "\nImages loaded: ");
        StrIToA(sizeText, gImageLoadedCount);
        StrCat(info, sizeText);
        StrCat(info, " / ");
        StrIToA(sizeText, gImageAttemptCount);
        StrCat(info, sizeText);
        if (gLastImageError[0]) {
            StrCat(info, "\nLast image error: ");
            StrNCat(info, gLastImageError, 63);
        }
        FrmCustomAlert(MessageAlert, info, "", "");
        return true;
    case MenuOpenAddress:
        FrmPopupForm(AddressForm);
        return true;
    case MenuHome:
        Navigate(gHomeUrl, true);
        return true;
    case MenuReload:
        Navigate(gCurrentUrl, false);
        return true;
    case MenuBack:
        GoBack();
        return true;
    case MenuForward:
        GoForward();
        return true;
    case MenuConnect:
        SetStatus("Connecting");
        networkError = OpenNetwork();
        if (networkError) {
            SetStatus("Connection failed");
            FrmCustomAlert(MessageAlert, "Could not open the Palm network connection.", "", "");
        } else SetStatus("Connected");
        return true;
    case MenuDisconnect:
        if (gNetworkBusy) {
            gDisconnectRequested = true;
            gPendingNavigation = false;
            gAbortRequested = true;
            SetStatus("Cancelling - closing connection...");
        } else {
            gDisconnectRequested = false;
            gPendingNavigation = false;
            gAbortRequested = false;
            CloseNetwork(true);
            SetStatus("Disconnected");
        }
        return true;
    case MenuImagesInline:
        gImagesEnabled = true;
        SavePreferences();
        Navigate(gCurrentUrl, false);
        return true;
    case MenuImagesLinks:
        gImagesEnabled = false;
        SavePreferences();
        Navigate(gCurrentUrl, false);
        return true;
    default:
        return false;
    }
}

static Boolean PreferencesFormHandleEvent(EventType *eventP)
{
    FormType *formP;
    ControlType *checkboxP;
    FieldType *homeFieldP;
    Char *homeValue;
    switch (eventP->eType) {
    case frmOpenEvent:
        formP = FrmGetActiveForm();
        FrmDrawForm(formP);
        checkboxP = (ControlType *)FrmGetObjectPtr(formP,
                    FrmGetObjectIndex(formP, ImagesCheckbox));
        CtlSetValue(checkboxP, gImagesEnabled);
        homeFieldP = (FieldType *)FrmGetObjectPtr(formP,
                     FrmGetObjectIndex(formP, HomeUrlField));
        ReplaceFieldText(homeFieldP, gHomeUrl);
        return true;
    case ctlSelectEvent:
        if (eventP->data.ctlSelect.controlID == PreferencesOKButton) {
            formP = FrmGetActiveForm();
            checkboxP = (ControlType *)FrmGetObjectPtr(formP,
                        FrmGetObjectIndex(formP, ImagesCheckbox));
            gImagesEnabled = CtlGetValue(checkboxP) != 0;
            homeFieldP = (FieldType *)FrmGetObjectPtr(formP,
                         FrmGetObjectIndex(formP, HomeUrlField));
            homeValue = homeFieldP ? FldGetTextPtr(homeFieldP) : NULL;
            if (homeValue && homeValue[0])
                SafeCopy(gHomeUrl, homeValue, sizeof(gHomeUrl));
            else
                SafeCopy(gHomeUrl, DEFAULT_HOME_URL, sizeof(gHomeUrl));
            SavePreferences();
            FrmReturnToForm(MainForm);
            return true;
        }
        if (eventP->data.ctlSelect.controlID == PreferencesCancelButton) {
            FrmReturnToForm(MainForm);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

static Boolean PlacesFormHandleEvent(EventType *eventP)
{
    FormType *formP;
    switch (eventP->eType) {
    case frmOpenEvent:
        formP = FrmGetActiveForm();
        FrmSetTitle(formP, gPlacesShowsHistory ? "History" : "Bookmarks");
        PopulatePlacesList();
        FrmDrawForm(formP);
        return true;
    case ctlSelectEvent:
        switch (eventP->data.ctlSelect.controlID) {
        case PlacesOpenButton: OpenSelectedPlace(); return true;
        case PlacesDeleteButton: DeleteSelectedPlace(); return true;
        case PlacesCancelButton: FrmReturnToForm(MainForm); return true;
        }
        break;
    case lstSelectEvent:
        OpenSelectedPlace();
        return true;
    default:
        break;
    }
    return false;
}

static Boolean MainFormHandleEvent(EventType *eventP)
{
    FormType *formP;
    RectangleType bounds;
    const Char *link;
    Int16 controlIndex;
    switch (eventP->eType) {
    case frmOpenEvent:
        formP = FrmGetActiveForm();
        FrmDrawForm(formP);
        SetAddress(gHomeUrl);
        SetPageText("Opening home page...");
        {
            EventType deferredEvent;
            MemSet(&deferredEvent, sizeof(deferredEvent), 0);
            deferredEvent.eType = DeferredHomeEvent;
            EvtAddEventToQueue(&deferredEvent);
        }
        return true;
    case DeferredHomeEvent:
        Navigate(gHomeUrl, true);
        return true;
    case ctlSelectEvent:
        switch (eventP->data.ctlSelect.controlID) {
        case GoButton: GoFromAddressField(); return true;
        case HomeButton: Navigate(gHomeUrl, true); return true;
        case ReloadButton: Navigate(gCurrentUrl, false); return true;
        case BackButton: GoBack(); return true;
        case ForwardButton: GoForward(); return true;
        }
        break;
    case sclRepeatEvent:
        if (eventP->data.sclRepeat.scrollBarID == PageScrollBar && GetPageBounds(&bounds)) {
            gDocument->scrollY = eventP->data.sclRepeat.newValue;
            DrawDocument();
            return true;
        }
        break;
    case keyDownEvent:
        if (eventP->data.keyDown.chr == pageUpChr) {
            if (GetPageBounds(&bounds)) {
                DocumentScroll(gDocument, -bounds.extent.y + 12, bounds.extent.y);
                DrawDocument();
            }
            return true;
        }
        if (eventP->data.keyDown.chr == pageDownChr) {
            if (GetPageBounds(&bounds)) {
                DocumentScroll(gDocument, bounds.extent.y - 12, bounds.extent.y);
                DrawDocument();
            }
            return true;
        }
        break;
    case penDownEvent:
        if (GetPageBounds(&bounds) && RctPtInRectangle(eventP->screenX, eventP->screenY, &bounds)) {
            controlIndex = DocumentControlAt(gDocument, eventP->screenX, eventP->screenY, &bounds);
            if (controlIndex >= 0) {
                if (DocumentGetControlType(gDocument, controlIndex) == 1) {
                    gEditingControl = controlIndex;
                    FrmPopupForm(InputForm);
                } else if (DocumentGetControlType(gDocument, controlIndex) == 2) {
                    SetStatus("Submitting form...");
                    if (DocumentBuildSubmitUrl(gDocument, controlIndex, gWorkUrl, sizeof(gWorkUrl))) {
                        VisibleUrlFromLink(gWorkUrl, gVisibleUrl, sizeof(gVisibleUrl));
                        Navigate(gVisibleUrl, true);
                    } else if (DocumentBuildPost(gDocument, controlIndex,
                                                 gPostAction, sizeof(gPostAction),
                                                 gPostBody, sizeof(gPostBody))) {
                        VisibleUrlFromLink(gPostAction, gVisibleUrl, sizeof(gVisibleUrl));
                        NavigatePost(gVisibleUrl, gPostBody);
                    } else FrmCustomAlert(MessageAlert, "Could not submit this form.", "", "");
                }
                return true;
            }
            link = DocumentLinkAt(gDocument, eventP->screenX, eventP->screenY, &bounds);
            if (link) {
                DocumentHighlightLinkAt(gDocument, eventP->screenX, eventP->screenY, &bounds);
                SetStatus("Opening link...");
                EvtResetAutoOffTimer();
                SysTaskDelay(SysTicksPerSecond() / 20 + 1);
                VisibleUrlFromLink(link, gVisibleUrl, sizeof(gVisibleUrl));
                Navigate(gVisibleUrl, true);
            }
            return true;
        }
        break;
    case menuEvent:
        return HandleMenu(eventP->data.menu.itemID);
    default:
        break;
    }
    return false;
}

static Boolean AddressFormHandleEvent(EventType *eventP)
{
    FormType *formP;
    FieldType *fieldP;
    Char *value;
    switch (eventP->eType) {
    case frmOpenEvent:
        formP = FrmGetActiveForm();
        FrmDrawForm(formP);
        fieldP = (FieldType *)FrmGetObjectPtr(formP,
                 FrmGetObjectIndex(formP, AddressEntryField));
        ReplaceFieldText(fieldP, gCurrentUrl);
        FrmSetFocus(formP, FrmGetObjectIndex(formP, AddressEntryField));
        return true;
    case ctlSelectEvent:
        if (eventP->data.ctlSelect.controlID == AddressOKButton) {
            fieldP = GetField(AddressEntryField);
            value = fieldP ? FldGetTextPtr(fieldP) : NULL;
            if (value && value[0]) SafeCopy(gVisibleUrl, value, sizeof(gVisibleUrl));
            else gVisibleUrl[0] = '\0';
            FrmReturnToForm(MainForm);
            if (gVisibleUrl[0]) Navigate(gVisibleUrl, true);
            return true;
        }
        if (eventP->data.ctlSelect.controlID == AddressCancelButton) {
            FrmReturnToForm(MainForm);
            return true;
        }
        break;
    case keyDownEvent:
        if (eventP->data.keyDown.chr == linefeedChr) {
            fieldP = GetField(AddressEntryField);
            value = fieldP ? FldGetTextPtr(fieldP) : NULL;
            if (value && value[0]) SafeCopy(gVisibleUrl, value, sizeof(gVisibleUrl));
            else gVisibleUrl[0] = '\0';
            FrmReturnToForm(MainForm);
            if (gVisibleUrl[0]) Navigate(gVisibleUrl, true);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

static Boolean InputFormHandleEvent(EventType *eventP)
{
    FormType *formP;
    FieldType *fieldP;
    Char *value;
    switch (eventP->eType) {
    case frmOpenEvent:
        formP = FrmGetActiveForm();
        FrmDrawForm(formP);
        fieldP = (FieldType *)FrmGetObjectPtr(formP, FrmGetObjectIndex(formP, InputField));
        value = (Char *)DocumentGetControlValue(gDocument, (UInt16)gEditingControl);
        ReplaceFieldText(fieldP, value ? value : "");
        FrmSetFocus(formP, FrmGetObjectIndex(formP, InputField));
        return true;
    case ctlSelectEvent:
        if (eventP->data.ctlSelect.controlID == InputOKButton) {
            fieldP = GetField(InputField);
            value = fieldP ? FldGetTextPtr(fieldP) : NULL;
            if (value && gEditingControl >= 0)
                DocumentSetControlValue(gDocument, (UInt16)gEditingControl, value);
            FrmReturnToForm(MainForm);
            DrawDocument();
            return true;
        }
        if (eventP->data.ctlSelect.controlID == InputCancelButton) {
            FrmReturnToForm(MainForm);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

static Boolean AppHandleEvent(EventType *eventP)
{
    UInt16 formID;
    FormType *formP;
    if (eventP->eType != frmLoadEvent) return false;
    formID = eventP->data.frmLoad.formID;
    formP = FrmInitForm(formID);
    FrmSetActiveForm(formP);
    if (formID == MainForm) FrmSetEventHandler(formP, MainFormHandleEvent);
    else if (formID == PlacesForm) FrmSetEventHandler(formP, PlacesFormHandleEvent);
    else if (formID == InputForm) FrmSetEventHandler(formP, InputFormHandleEvent);
    else if (formID == PreferencesForm) FrmSetEventHandler(formP, PreferencesFormHandleEvent);
    else if (formID == AddressForm) FrmSetEventHandler(formP, AddressFormHandleEvent);
    return true;
}

/* Versioned database names let Palm OS install an update even when an older
 * code database is still open.  Once the new build launches, remove only old
 * application databases with our own creator ID.  Preferences live in the
 * Saved Preferences database and are not touched. */
static void RemoveOlderApplicationDatabases(void)
{
    DmSearchStateType search;
    UInt16 currentCard;
    UInt16 card;
    UInt16 cards[8];
    UInt16 count;
    UInt16 i;
    LocalID currentID;
    LocalID databaseID;
    LocalID databases[8];
    Boolean newSearch;

    currentCard = 0;
    currentID = 0;
    SysCurAppDatabase(&currentCard, &currentID);
    MemSet(&search, sizeof(search), 0);
    count = 0;
    newSearch = true;
    while (count < 8 &&
           DmGetNextDatabaseByTypeCreator(newSearch, &search, 'appl',
                                          PREF_CREATOR, false, &card,
                                          &databaseID) == errNone) {
        newSearch = false;
        if (databaseID != currentID || card != currentCard) {
            cards[count] = card;
            databases[count] = databaseID;
            count++;
        }
    }
    for (i = 0; i < count; ++i) DmDeleteDatabase(cards[i], databases[i]);
}

static Err AppStart(void)
{
    UInt32 romVersion;
    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &romVersion);
    if (romVersion < sysMakeROMVersion(4, 0, 0, sysROMStageRelease, 0)) {
        FrmAlert(AboutAlert);
        return sysErrRomIncompatible;
    }
    RemoveOlderApplicationDatabases();
    gCurrentUrl[0] = '\0';
    gDocument = (Document *)MemPtrNew(sizeof(Document));
    if (!gDocument) return memErrNotEnoughSpace;
    DocumentInit(gDocument);
    LoadPreferences();
    return errNone;
}

static void AppStop(void)
{
    CloseNetwork(false);
    SavePreferences();
    if (gDocument) {
        DocumentFree(gDocument);
        MemPtrFree(gDocument);
        gDocument = NULL;
    }
    FrmCloseAllForms();
}

static void AppEventLoop(void)
{
    EventType event;
    UInt16 error;
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (event.eType == penDownEvent &&
            ScrollFromPen(event.screenX, event.screenY, true)) continue;
        if (!SysHandleEvent(&event) && !MenuHandleEvent(NULL, &event, &error) &&
            !AppHandleEvent(&event)) FrmDispatchEvent(&event);
    } while (event.eType != appStopEvent);
}

UInt32 PilotMain(UInt16 command, MemPtr commandPBP, UInt16 launchFlags)
{
    Err err;
    if (command != sysAppLaunchCmdNormalLaunch) return errNone;
    err = AppStart();
    if (err) return err;
    FrmGotoForm(MainForm);
    AppEventLoop();
    AppStop();
    return errNone;
}

