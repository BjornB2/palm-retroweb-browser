#ifndef RETROWEB_DOCUMENT_H
#define RETROWEB_DOCUMENT_H

#include <PalmOS.h>

#define DOC_MAX_ITEMS 384
#define DOC_POOL_SIZE 16000
#define DOC_MAX_CONTROLS 16
#define DOC_MAX_FORMS 4

typedef BitmapType *(*DocumentImageLoader)(const Char *url, void *context);
typedef Boolean (*DocumentEventPump)(void *context);

typedef enum {
    docItemText = 1,
    docItemImage = 2,
    docItemRule = 3,
    docItemInput = 4,
    docItemSubmit = 5
} DocumentItemKind;

typedef struct {
    UInt8 kind;
    UInt8 font;
    UInt16 textOffset;
    UInt16 linkOffset;
    UInt16 controlIndex;
    Coord x;
    Coord y;
    Coord width;
    Coord height;
    BitmapType *bitmap;
} DocumentItem;

typedef struct {
    Char action[256];
    Char method[5];
} DocumentForm;

typedef struct {
    Char name[48];
    Char value[128];
    UInt8 type;
    UInt8 formIndex;
} DocumentControl;

typedef struct {
    DocumentItem items[DOC_MAX_ITEMS];
    UInt16 itemCount;
    Char pool[DOC_POOL_SIZE];
    UInt16 poolUsed;
    Coord contentHeight;
    Coord scrollY;
    Char title[64];
    Char baseUrl[256];
    UInt16 imageCount;
    DocumentForm forms[DOC_MAX_FORMS];
    UInt16 formCount;
    DocumentControl controls[DOC_MAX_CONTROLS];
    UInt16 controlCount;
} Document;

void DocumentInit(Document *document);
void DocumentFree(Document *document);
Boolean DocumentParse(Document *document, const Char *html, const Char *baseUrl,
                      DocumentImageLoader loader, void *loaderContext,
                      DocumentEventPump eventPump, void *eventContext);
void DocumentDraw(Document *document, const RectangleType *bounds);
const Char *DocumentLinkAt(Document *document, Coord screenX, Coord screenY,
                           const RectangleType *bounds);
Boolean DocumentHighlightLinkAt(Document *document, Coord screenX, Coord screenY,
                                const RectangleType *bounds);
void DocumentScroll(Document *document, Int16 delta, Coord viewportHeight);
Int16 DocumentControlAt(Document *document, Coord screenX, Coord screenY,
                        const RectangleType *bounds);
Boolean DocumentSetControlValue(Document *document, UInt16 index, const Char *value);
const Char *DocumentGetControlValue(Document *document, UInt16 index);
UInt8 DocumentGetControlType(Document *document, UInt16 index);
Boolean DocumentBuildSubmitUrl(Document *document, UInt16 submitIndex,
                               Char *output, UInt16 capacity);
Boolean DocumentBuildPost(Document *document, UInt16 submitIndex,
                          Char *action, UInt16 actionCapacity,
                          Char *body, UInt16 bodyCapacity);

#endif

