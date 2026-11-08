#include "document.h"

#define DOC_LEFT 2
#define DOC_RIGHT 144
#define DOC_LINE_GAP 1

/* Shared parser scratch space. Palm OS 68K stacks are too small for these
 * buffers to be automatic variables in nested parser/image calls. */
static Char gDocTag[600];
static Char gDocAttribute[512];
static Char gDocUrlA[512];
static Char gDocUrlB[512];
static Char gDocUrlC[512];
static Char gDocAlt[96];

typedef struct {
    Document *document;
    Coord x;
    Coord y;
    Coord lineHeight;
    FontID font;
    UInt16 linkOffset;
    Boolean preformatted;
    Boolean pendingSpace;
    UInt8 listDepth;
    Int16 formIndex;
    DocumentImageLoader loader;
    void *loaderContext;
    DocumentEventPump eventPump;
    void *eventContext;
    Boolean cancelled;
} LayoutState;

static Boolean StartsWithCaseless(const Char *text, const Char *prefix)
{
    return StrNCaselessCompare(text, prefix, StrLen(prefix)) == 0;
}

static const Char *FindLastChar(const Char *text, Char wanted)
{
    const Char *found;
    found = NULL;
    while (*text) {
        if (*text == wanted) found = text;
        text++;
    }
    return found;
}

static UInt16 PoolAdd(Document *document, const Char *text, UInt16 length)
{
    UInt16 offset;
    if (!text || !length) return 0;
    if ((UInt32)document->poolUsed + length + 1 > DOC_POOL_SIZE) return 0;
    offset = document->poolUsed;
    MemMove(document->pool + offset, text, length);
    document->pool[offset + length] = '\0';
    document->poolUsed += length + 1;
    return offset;
}

static void NewLine(LayoutState *state, Boolean paragraph)
{
    Coord minimum;
    minimum = FntLineHeight();
    if (state->lineHeight < minimum) state->lineHeight = minimum;
    state->x = DOC_LEFT + state->listDepth * 6;
    state->y += state->lineHeight + (paragraph ? 3 : DOC_LINE_GAP);
    state->lineHeight = FntLineHeight();
    state->pendingSpace = false;
}

static DocumentItem *NewItem(LayoutState *state, UInt8 kind)
{
    DocumentItem *item;
    if (state->document->itemCount >= DOC_MAX_ITEMS) return NULL;
    item = &state->document->items[state->document->itemCount++];
    MemSet(item, sizeof(DocumentItem), 0);
    item->kind = kind;
    return item;
}

static void AddWord(LayoutState *state, const Char *word, UInt16 length)
{
    Char buffer[96];
    UInt16 outputLength;
    UInt16 offset;
    Coord width;
    Coord available;
    Boolean fit;
    DocumentItem *item;
    FontID oldFont;

    if (!length) return;
    if (length > 90) length = 90;
    outputLength = 0;
    if (state->pendingSpace && state->x > DOC_LEFT + state->listDepth * 6)
        buffer[outputLength++] = ' ';
    MemMove(buffer + outputLength, word, length);
    outputLength += length;
    buffer[outputLength] = '\0';

    oldFont = FntSetFont(state->font);
    width = FntCharsWidth(buffer, outputLength);
    if (state->x + width > DOC_RIGHT && state->x > DOC_LEFT + state->listDepth * 6) {
        NewLine(state, false);
        outputLength = length;
        MemMove(buffer, word, length);
        buffer[length] = '\0';
        width = FntCharsWidth(buffer, outputLength);
    }
    if (width > DOC_RIGHT - state->x) {
        available = DOC_RIGHT - state->x;
        fit = false;
        FntCharsInWidth(buffer, &available, &outputLength, &fit);
        if (!outputLength) outputLength = 1;
        buffer[outputLength] = '\0';
        width = FntCharsWidth(buffer, outputLength);
    }
    offset = PoolAdd(state->document, buffer, outputLength);
    if (offset || (outputLength && buffer[0] == '\0')) {
        item = NewItem(state, docItemText);
        if (item) {
            item->font = state->font;
            item->textOffset = offset;
            item->linkOffset = state->linkOffset;
            item->x = state->x;
            item->y = state->y;
            item->width = width;
            item->height = FntLineHeight();
            state->x += width;
            if (state->lineHeight < item->height) state->lineHeight = item->height;
        }
    }
    FntSetFont(oldFont);
    state->pendingSpace = true;
}

static UInt16 DecodeEntity(const Char *input, UInt16 remaining, Char *output)
{
    UInt16 i;
    UInt32 value;
    if (remaining >= 5 && StrNCompare(input, "&amp;", 5) == 0) { *output = '&'; return 5; }
    if (remaining >= 4 && StrNCompare(input, "&lt;", 4) == 0) { *output = '<'; return 4; }
    if (remaining >= 4 && StrNCompare(input, "&gt;", 4) == 0) { *output = '>'; return 4; }
    if (remaining >= 6 && StrNCompare(input, "&quot;", 6) == 0) { *output = '"'; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&nbsp;", 6) == 0) { *output = ' '; return 6; }
    if (remaining >= 8 && StrNCompare(input, "&Atilde;", 8) == 0) { *output = (Char)0xC3; return 8; }
    if (remaining >= 7 && StrNCompare(input, "&acirc;", 7) == 0) { *output = (Char)0xE2; return 7; }
    if (remaining >= 6 && StrNCompare(input, "&para;", 6) == 0) { *output = (Char)0xB6; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&auml;", 6) == 0) { *output = (Char)0xE4; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&ouml;", 6) == 0) { *output = (Char)0xF6; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&uuml;", 6) == 0) { *output = (Char)0xFC; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&Auml;", 6) == 0) { *output = (Char)0xC4; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&Ouml;", 6) == 0) { *output = (Char)0xD6; return 6; }
    if (remaining >= 6 && StrNCompare(input, "&Uuml;", 6) == 0) { *output = (Char)0xDC; return 6; }
    if (remaining >= 8 && StrNCompare(input, "&eacute;", 8) == 0) { *output = (Char)0xE9; return 8; }
    if (remaining >= 8 && StrNCompare(input, "&egrave;", 8) == 0) { *output = (Char)0xE8; return 8; }
    if (remaining >= 6 && StrNCompare(input, "&copy;", 6) == 0) { *output = (Char)0xA9; return 6; }
    if (remaining >= 7 && StrNCompare(input, "&laquo;", 7) == 0) { *output = (Char)0xAB; return 7; }
    if (remaining >= 7 && StrNCompare(input, "&raquo;", 7) == 0) { *output = (Char)0xBB; return 7; }
    if (remaining >= 5 && StrNCompare(input, "&uml;", 5) == 0) { *output = (Char)0xA8; return 5; }
    if (remaining >= 8 && StrNCompare(input, "&curren;", 8) == 0) { *output = (Char)0xA4; return 8; }
    if (remaining >= 8 && StrNCompare(input, "&frac14;", 8) == 0) { *output = (Char)0xBC; return 8; }
    if (remaining >= 4 && input[1] == '#') {
        value = 0;
        i = 2;
        while (i < remaining && input[i] >= '0' && input[i] <= '9') {
            value = value * 10 + input[i] - '0';
            i++;
        }
        if (i < remaining && input[i] == ';' && i > 2) {
            *output = value <= 255 ? (Char)value : '?';
            return i + 1;
        }
    }
    *output = '&';
    return 1;
}

static Char PalmCharForUnicode(UInt32 code)
{
    if (code <= 255) return (Char)code;
    if (code == 0x2018 || code == 0x2019) return '\'';
    if (code == 0x201C || code == 0x201D) return '"';
    if (code == 0x2013 || code == 0x2014) return '-';
    if (code == 0x2022) return '*';
    if (code == 0x2026) return (Char)0x85;
    return '?';
}

static UInt16 DecodeUtf8(const Char *input, UInt16 remaining, Char *output)
{
    UInt8 a;
    UInt8 b;
    UInt8 c;
    UInt32 code;
    a = (UInt8)input[0];
    if (a < 0x80) { *output = (Char)a; return 1; }
    if (a >= 0xC2 && a <= 0xDF && remaining >= 2) {
        b = (UInt8)input[1];
        if ((b & 0xC0) == 0x80) {
            code = ((UInt32)(a & 0x1F) << 6) | (b & 0x3F);
            *output = PalmCharForUnicode(code);
            return 2;
        }
    }
    if (a >= 0xE0 && a <= 0xEF && remaining >= 3) {
        b = (UInt8)input[1];
        c = (UInt8)input[2];
        if ((b & 0xC0) == 0x80 && (c & 0xC0) == 0x80) {
            code = ((UInt32)(a & 0x0F) << 12) |
                   ((UInt32)(b & 0x3F) << 6) | (c & 0x3F);
            *output = PalmCharForUnicode(code);
            return 3;
        }
    }
    /* RetroWEB's Palm profile emits Windows-1252. Preserve a lone high byte
     * when it is not a valid UTF-8 sequence (for example 0xF6 for ö). */
    *output = (Char)a;
    return 1;
}

static UInt16 DecodeHtmlChar(const Char *input, UInt16 remaining, Char *output)
{
    UInt16 firstUsed;
    UInt16 secondUsed;
    UInt16 thirdUsed;
    Char first;
    Char second;
    Char third;
    UInt8 a;
    UInt8 b;
    UInt8 c;
    UInt32 code;
    if (!remaining) { *output = '\0'; return 0; }
    if (input[0] != '&' && (UInt8)input[0] < 0x80) {
        *output = input[0];
        return 1;
    }
    if (input[0] != '&') return DecodeUtf8(input, remaining, output);
    firstUsed = DecodeEntity(input, remaining, &first);
    a = (UInt8)first;
    if (firstUsed < remaining && input[firstUsed] == '&' && a >= 0xC2 && a <= 0xDF) {
        secondUsed = DecodeEntity(input + firstUsed, remaining - firstUsed, &second);
        b = (UInt8)second;
        if ((b & 0xC0) == 0x80) {
            code = ((UInt32)(a & 0x1F) << 6) | (b & 0x3F);
            *output = PalmCharForUnicode(code);
            return firstUsed + secondUsed;
        }
    }
    if (firstUsed < remaining && input[firstUsed] == '&' && a >= 0xE0 && a <= 0xEF) {
        secondUsed = DecodeEntity(input + firstUsed, remaining - firstUsed, &second);
        if (firstUsed + secondUsed < remaining && input[firstUsed + secondUsed] == '&') {
            thirdUsed = DecodeEntity(input + firstUsed + secondUsed,
                                     remaining - firstUsed - secondUsed, &third);
            b = (UInt8)second;
            c = (UInt8)third;
            if ((b & 0xC0) == 0x80 && (c & 0xC0) == 0x80) {
                code = ((UInt32)(a & 0x0F) << 12) |
                       ((UInt32)(b & 0x3F) << 6) | (c & 0x3F);
                *output = PalmCharForUnicode(code);
                return firstUsed + secondUsed + thirdUsed;
            }
        }
    }
    *output = first;
    return firstUsed;
}

static void AddText(LayoutState *state, const Char *text, UInt16 length)
{
    Char word[96];
    UInt16 wordLength;
    UInt16 i;
    UInt16 consumed;
    Char decoded;
    wordLength = 0;
    for (i = 0; i < length; ) {
        Char c;
        consumed = DecodeHtmlChar(text + i, length - i, &decoded);
        c = decoded;
        i += consumed;
        if ((i & 63) == 0 && state->eventPump &&
            !state->eventPump(state->eventContext)) {
            state->cancelled = true;
            return;
        }
        if (c == '\r') continue;
        if (c == '\n' && state->preformatted) {
            if (wordLength) { AddWord(state, word, wordLength); wordLength = 0; }
            NewLine(state, false);
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (wordLength) { AddWord(state, word, wordLength); wordLength = 0; }
            state->pendingSpace = true;
        } else {
            if (wordLength == sizeof(word) - 1) {
                AddWord(state, word, wordLength);
                wordLength = 0;
            }
            word[wordLength++] = c;
        }
    }
    if (wordLength) AddWord(state, word, wordLength);
}

static void AppendTitleText(Document *document, UInt16 *used,
                            const Char *text, UInt16 length)
{
    UInt16 i;
    UInt16 consumed;
    Char decoded;
    for (i = 0; i < length && *used + 1 < sizeof(document->title); ) {
        consumed = DecodeHtmlChar(text + i, length - i, &decoded);
        document->title[(*used)++] = decoded;
        i += consumed;
    }
    document->title[*used] = '\0';
}

static Boolean IsNameChar(Char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static Boolean GetAttribute(const Char *tag, const Char *name,
                            Char *value, UInt16 capacity)
{
    const Char *p;
    UInt16 nameLength;
    p = tag;
    nameLength = StrLen(name);
    while (*p) {
        while (*p && !IsNameChar(*p)) p++;
        if (!*p) break;
        if (StrNCaselessCompare(p, name, nameLength) == 0 &&
            !IsNameChar(p[nameLength])) {
            p += nameLength;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            {
                Char quote;
                UInt16 used;
                quote = (*p == '"' || *p == '\'') ? *p++ : 0;
                used = 0;
                while (*p && ((quote && *p != quote) || (!quote && *p != ' ' && *p != '\t'))) {
                    if (used + 1 < capacity) value[used++] = *p;
                    p++;
                }
                value[used] = '\0';
                return used != 0;
            }
        }
        while (*p && IsNameChar(*p)) p++;
    }
    return false;
}

static void ResolveUrl(const Char *base, const Char *reference,
                       Char *output, UInt16 capacity)
{
    const Char *source;
    UInt16 sourceUsed;
    const Char *path;
    const Char *slash;
    UInt16 length;
    sourceUsed = 0;
    while (*reference && sourceUsed + 1 < sizeof(gDocUrlC)) {
        if (StrNCompare(reference, "&amp;", 5) == 0) {
            gDocUrlC[sourceUsed++] = '&';
            reference += 5;
        } else gDocUrlC[sourceUsed++] = *reference++;
    }
    gDocUrlC[sourceUsed] = '\0';
    source = gDocUrlC;
    if (source[0] == '/' && source[1] == '/') {
        if (StartsWithCaseless(base, "https://")) StrNCopy(output, "https:", capacity - 1);
        else StrNCopy(output, "http:", capacity - 1);
        output[capacity - 1] = '\0';
        if (StrLen(output) + StrLen(source) + 1 <= capacity) StrCat(output, source);
        else output[0] = '\0';
        return;
    }
    if (StartsWithCaseless(source, "http://") || StartsWithCaseless(source, "https://")) {
        StrNCopy(output, source, capacity - 1);
        output[capacity - 1] = '\0';
        return;
    }
    if (source[0] == '/') {
        path = StrStr(base, "://");
        if (!path) { StrNCopy(output, reference, capacity - 1); output[capacity - 1] = '\0'; return; }
        path += 3;
        slash = StrChr(path, '/');
        length = slash ? (UInt16)(slash - base) : StrLen(base);
        if (length + StrLen(source) + 1 > capacity) { output[0] = '\0'; return; }
        MemMove(output, base, length);
        output[length] = '\0';
        StrCat(output, source);
        return;
    }
    slash = FindLastChar(base, '/');
    if (!slash || slash < base + 7) {
        StrNCopy(output, source, capacity - 1);
        output[capacity - 1] = '\0';
        return;
    }
    length = (UInt16)(slash - base + 1);
    if (length + StrLen(source) + 1 > capacity) { output[0] = '\0'; return; }
    MemMove(output, base, length);
    output[length] = '\0';
    StrCat(output, source);
}

Boolean DocumentImageUrlAt(const Char *html, const Char *baseUrl, UInt16 index,
                           Char *output, UInt16 capacity)
{
    const Char *p;
    const Char *tagEnd;
    const Char *name;
    UInt16 tagLength;
    UInt16 found;
    if (!html || !baseUrl || !output || !capacity) return false;
    output[0] = '\0';
    p = html;
    found = 0;
    while ((p = StrChr(p, '<')) != NULL) {
        tagEnd = StrChr(p, '>');
        if (!tagEnd) break;
        tagLength = (UInt16)(tagEnd - p - 1);
        if (tagLength >= sizeof(gDocTag)) tagLength = sizeof(gDocTag) - 1;
        MemMove(gDocTag, p + 1, tagLength);
        gDocTag[tagLength] = '\0';
        name = gDocTag;
        while (*name == ' ' || *name == '\t') name++;
        if (*name != '/' && StrNCaselessCompare(name, "img", 3) == 0 &&
            !IsNameChar(name[3]) &&
            GetAttribute(gDocTag, "src", gDocUrlA, sizeof(gDocUrlA))) {
            if (found == index) {
                ResolveUrl(baseUrl, gDocUrlA, gDocUrlB, sizeof(gDocUrlB));
                if (!gDocUrlB[0] || StrLen(gDocUrlB) + 1 > capacity) return false;
                StrCopy(output, gDocUrlB);
                return true;
            }
            found++;
        }
        p = tagEnd + 1;
    }
    return false;
}

static void AddImage(LayoutState *state, const Char *tag)
{
    BitmapType *bitmap;
    UInt16 width;
    UInt16 height;
    UInt16 rowBytes;
    UInt16 imageIndex;
    DocumentItem *item;
    if (!GetAttribute(tag, "src", gDocUrlA, sizeof(gDocUrlA))) return;
    ResolveUrl(state->document->baseUrl, gDocUrlA, gDocUrlB, sizeof(gDocUrlB));
    if (!gDocUrlB[0]) return;
    if (state->eventPump && !state->eventPump(state->eventContext)) {
        state->cancelled = true;
        return;
    }
    bitmap = NULL;
    width = 0;
    height = 0;
    imageIndex = 0xffff;
    if (state->loader)
        bitmap = state->loader(gDocUrlB, state->loaderContext,
                               &width, &height, &imageIndex);
    if (state->eventPump && !state->eventPump(state->eventContext)) {
        if (bitmap) BmpDelete(bitmap);
        state->cancelled = true;
        return;
    }
    if (!bitmap && (!width || !height || imageIndex == 0xffff)) {
        if (GetAttribute(tag, "alt", gDocAlt, sizeof(gDocAlt)))
            AddText(state, gDocAlt, StrLen(gDocAlt));
        else AddText(state, "[image]", 7);
        return;
    }
    state->document->imageCount++;
    if (bitmap) BmpGetDimensions(bitmap, &width, &height, &rowBytes);
    if (state->x > DOC_LEFT + state->listDepth * 6) NewLine(state, false);
    item = NewItem(state, docItemImage);
    if (!item) { BmpDelete(bitmap); return; }
    item->bitmap = bitmap;
    item->imageIndex = imageIndex;
    item->x = DOC_LEFT;
    item->y = state->y;
    item->width = width;
    item->height = height;
    if (item->width > DOC_RIGHT - DOC_LEFT) item->width = DOC_RIGHT - DOC_LEFT;
    state->lineHeight = height;
    NewLine(state, false);
}

static void AddRule(LayoutState *state)
{
    DocumentItem *item;
    if (state->x > DOC_LEFT + state->listDepth * 6) NewLine(state, false);
    item = NewItem(state, docItemRule);
    if (!item) return;
    item->x = DOC_LEFT;
    item->y = state->y + 2;
    item->width = DOC_RIGHT - DOC_LEFT;
    item->height = 5;
    state->lineHeight = 5;
    NewLine(state, false);
}

static void AddInput(LayoutState *state, const Char *tag)
{
    Char type[24];
    Char name[48];
    Char value[128];
    DocumentControl *control;
    DocumentItem *item;
    UInt16 index;
    Coord height;
    Coord width;
    if (state->formIndex < 0 || state->document->controlCount >= DOC_MAX_CONTROLS) return;
    type[0] = '\0';
    name[0] = '\0';
    value[0] = '\0';
    GetAttribute(tag, "type", type, sizeof(type));
    GetAttribute(tag, "name", name, sizeof(name));
    GetAttribute(tag, "value", value, sizeof(value));
    index = state->document->controlCount++;
    control = &state->document->controls[index];
    MemSet(control, sizeof(DocumentControl), 0);
    StrNCopy(control->name, name, sizeof(control->name) - 1);
    StrNCopy(control->value, value, sizeof(control->value) - 1);
    control->formIndex = (UInt8)state->formIndex;
    if (StrCaselessCompare(type, "hidden") == 0) { control->type = 0; return; }
    control->type = (StrCaselessCompare(type, "submit") == 0) ? 2 : 1;
    if (state->x > DOC_LEFT + state->listDepth * 6) NewLine(state, false);
    item = NewItem(state, control->type == 2 ? docItemSubmit : docItemInput);
    if (!item) return;
    item->controlIndex = index;
    item->x = DOC_LEFT + state->listDepth * 6;
    item->y = state->y;
    height = FntLineHeight() + 6;
    width = control->type == 2 ? FntCharsWidth(control->value, StrLen(control->value)) + 12 : 146;
    if (width < 30) width = 30;
    if (width > 150) width = 150;
    item->width = width;
    item->height = height;
    state->lineHeight = height;
    NewLine(state, false);
}

static void StartForm(LayoutState *state, const Char *tag)
{
    DocumentForm *form;
    Char method[12];
    if (state->document->formCount >= DOC_MAX_FORMS) return;
    gDocUrlA[0] = '\0';
    method[0] = '\0';
    GetAttribute(tag, "action", gDocUrlA, sizeof(gDocUrlA));
    GetAttribute(tag, "method", method, sizeof(method));
    if (gDocUrlA[0])
        ResolveUrl(state->document->baseUrl, gDocUrlA, gDocUrlB, sizeof(gDocUrlB));
    else StrNCopy(gDocUrlB, state->document->baseUrl, sizeof(gDocUrlB) - 1);
    gDocUrlB[sizeof(gDocUrlB) - 1] = '\0';
    state->formIndex = state->document->formCount++;
    form = &state->document->forms[state->formIndex];
    MemSet(form, sizeof(DocumentForm), 0);
    StrNCopy(form->action, gDocUrlB, sizeof(form->action) - 1);
    StrCopy(form->method, StrCaselessCompare(method, "post") == 0 ? "post" : "get");
}

void DocumentInit(Document *document)
{
    MemSet(document, sizeof(Document), 0);
    document->pool[0] = '\0';
    document->poolUsed = 1;
    document->contentHeight = 1;
}

void DocumentFree(Document *document)
{
    UInt16 i;
    for (i = 0; i < document->itemCount; ++i) {
        if (document->items[i].kind == docItemImage && document->items[i].bitmap)
            BmpDelete(document->items[i].bitmap);
    }
    DocumentInit(document);
}

Boolean DocumentParse(Document *document, const Char *html, const Char *baseUrl,
                      DocumentImageLoader loader, void *loaderContext,
                      DocumentEventPump eventPump, void *eventContext)
{
    LayoutState state;
    const Char *p;
    const Char *tagEnd;
    const Char *textEnd;
    Char name[24];
    UInt16 tagLength;
    UInt16 nameLength;
    Boolean closing;
    Boolean inTitle;
    UInt16 titleUsed;

    DocumentFree(document);
    StrNCopy(document->baseUrl, baseUrl, sizeof(document->baseUrl) - 1);
    document->baseUrl[sizeof(document->baseUrl) - 1] = '\0';
    MemSet(&state, sizeof(state), 0);
    state.document = document;
    state.x = DOC_LEFT;
    state.y = 1;
    state.font = stdFont;
    state.lineHeight = FntLineHeight();
    state.loader = loader;
    state.loaderContext = loaderContext;
    state.eventPump = eventPump;
    state.eventContext = eventContext;
    state.formIndex = -1;
    p = html;
    inTitle = false;
    titleUsed = 0;

    while (*p && document->itemCount < DOC_MAX_ITEMS) {
        if (state.eventPump && !state.eventPump(state.eventContext)) {
            state.cancelled = true;
            break;
        }
        if (*p != '<') {
            textEnd = StrChr(p, '<');
            if (!textEnd) textEnd = p + StrLen(p);
            if (inTitle) {
                AppendTitleText(document, &titleUsed, p, (UInt16)(textEnd - p));
            } else AddText(&state, p, (UInt16)(textEnd - p));
            if (state.cancelled) break;
            p = textEnd;
            continue;
        }
        tagEnd = StrChr(p, '>');
        if (!tagEnd) break;
        tagLength = (UInt16)(tagEnd - p - 1);
        if (tagLength >= sizeof(gDocTag)) tagLength = sizeof(gDocTag) - 1;
        MemMove(gDocTag, p + 1, tagLength);
        gDocTag[tagLength] = '\0';
        p = tagEnd + 1;

        {
            const Char *cursor;
            cursor = gDocTag;
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            closing = *cursor == '/';
            if (closing) cursor++;
            nameLength = 0;
            while (IsNameChar(*cursor) && nameLength + 1 < sizeof(name))
                name[nameLength++] = *cursor++;
            name[nameLength] = '\0';
        }
        if (!nameLength || name[0] == '!' || name[0] == '?') continue;
        if (StrCaselessCompare(name, "title") == 0) { inTitle = !closing; continue; }
        if (inTitle) continue;
        if (!closing && StrCaselessCompare(name, "a") == 0) {
            if (GetAttribute(gDocTag, "href", gDocAttribute, sizeof(gDocAttribute))) {
                ResolveUrl(document->baseUrl, gDocAttribute, gDocUrlA, sizeof(gDocUrlA));
                state.linkOffset = PoolAdd(document, gDocUrlA, StrLen(gDocUrlA));
            }
        } else if (closing && StrCaselessCompare(name, "a") == 0) state.linkOffset = 0;
        else if (!closing && StrCaselessCompare(name, "img") == 0) AddImage(&state, gDocTag);
        else if (!closing && StrCaselessCompare(name, "form") == 0) StartForm(&state, gDocTag);
        else if (closing && StrCaselessCompare(name, "form") == 0) state.formIndex = -1;
        else if (!closing && StrCaselessCompare(name, "input") == 0) AddInput(&state, gDocTag);
        else if (!closing && StrCaselessCompare(name, "br") == 0) NewLine(&state, false);
        else if (!closing && StrCaselessCompare(name, "hr") == 0) AddRule(&state);
        else if (StrCaselessCompare(name, "p") == 0) NewLine(&state, true);
        else if (StrCaselessCompare(name, "li") == 0 && !closing) {
            NewLine(&state, false);
            AddText(&state, "* ", 2);
        } else if ((StrCaselessCompare(name, "ul") == 0 || StrCaselessCompare(name, "ol") == 0)) {
            if (!closing && state.listDepth < 4) state.listDepth++;
            else if (closing && state.listDepth) state.listDepth--;
            NewLine(&state, false);
        } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
            NewLine(&state, true);
            state.font = closing ? stdFont : (name[1] <= '2' ? largeBoldFont : boldFont);
        } else if (StrCaselessCompare(name, "b") == 0 || StrCaselessCompare(name, "strong") == 0) {
            state.font = closing ? stdFont : boldFont;
        } else if (StrCaselessCompare(name, "pre") == 0) {
            state.preformatted = !closing;
            NewLine(&state, false);
        } else if (StrCaselessCompare(name, "blockquote") == 0) {
            NewLine(&state, false);
            if (!closing && state.listDepth < 4) state.listDepth++;
            else if (closing && state.listDepth) state.listDepth--;
        }
        if (state.cancelled) break;
    }
    document->contentHeight = state.y + state.lineHeight + 2;
    if (!document->title[0]) StrCopy(document->title, "Untitled page");
    return !state.cancelled;
}

void DocumentDraw(Document *document, const RectangleType *bounds)
{
    UInt16 i;
    DocumentItem *item;
    Coord screenY;
    RectangleType oldClip;
    FontID oldFont;
    FontID currentFont;
    IndexedColorType oldText;
    IndexedColorType linkText;
    IndexedColorType normalText;
    IndexedColorType currentText;
    RGBColorType blue;
    WinGetClip(&oldClip);
    WinSetClip(bounds);
    WinEraseRectangle(bounds, 0);
    oldFont = FntSetFont(stdFont);
    oldText = WinSetTextColor(UIColorGetTableEntryIndex(UIFieldText));
    currentFont = stdFont;
    blue.index = 0;
    blue.r = 0;
    blue.g = 0;
    blue.b = 200;
    linkText = WinRGBToIndex(&blue);
    normalText = UIColorGetTableEntryIndex(UIFieldText);
    currentText = normalText;
    for (i = 0; i < document->itemCount; ++i) {
        item = &document->items[i];
        screenY = bounds->topLeft.y + item->y - document->scrollY;
        if (screenY + item->height < bounds->topLeft.y) continue;
        if (screenY > bounds->topLeft.y + bounds->extent.y) break;
        if (item->kind == docItemText) {
            IndexedColorType wantedText;
            if (item->font != currentFont) {
                FntSetFont(item->font);
                currentFont = item->font;
            }
            wantedText = item->linkOffset ? linkText : normalText;
            if (wantedText != currentText) {
                WinSetTextColor(wantedText);
                currentText = wantedText;
            }
            WinDrawChars(document->pool + item->textOffset,
                         StrLen(document->pool + item->textOffset),
                         bounds->topLeft.x + item->x, screenY);
            if (item->linkOffset)
                WinDrawLine(bounds->topLeft.x + item->x, screenY + item->height - 1,
                            bounds->topLeft.x + item->x + item->width, screenY + item->height - 1);
        } else if (item->kind == docItemImage && item->bitmap) {
            WinDrawBitmap(item->bitmap, bounds->topLeft.x + item->x, screenY);
        } else if (item->kind == docItemRule) {
            WinDrawLine(bounds->topLeft.x + item->x, screenY + 2,
                        bounds->topLeft.x + item->x + item->width, screenY + 2);
        } else if (item->kind == docItemInput || item->kind == docItemSubmit) {
            DocumentControl *control;
            RectangleType controlBounds;
            control = &document->controls[item->controlIndex];
            controlBounds.topLeft.x = bounds->topLeft.x + item->x;
            controlBounds.topLeft.y = screenY;
            controlBounds.extent.x = item->width;
            controlBounds.extent.y = item->height;
            if (item->kind == docItemSubmit) WinDrawRectangle(&controlBounds, 3);
            else WinDrawRectangleFrame(simpleFrame, &controlBounds);
            WinDrawTruncChars(control->value, StrLen(control->value),
                              controlBounds.topLeft.x + 3, controlBounds.topLeft.y + 3,
                              controlBounds.extent.x - 6);
        }
    }
    WinSetTextColor(oldText);
    FntSetFont(oldFont);
    WinSetClip(&oldClip);
}

Int16 DocumentControlAt(Document *document, Coord screenX, Coord screenY,
                        const RectangleType *bounds)
{
    UInt16 i;
    DocumentItem *item;
    Coord documentX;
    Coord documentY;
    documentX = screenX - bounds->topLeft.x;
    documentY = screenY - bounds->topLeft.y + document->scrollY;
    for (i = 0; i < document->itemCount; ++i) {
        item = &document->items[i];
        if ((item->kind == docItemInput || item->kind == docItemSubmit) &&
            documentX >= item->x && documentX < item->x + item->width &&
            documentY >= item->y && documentY < item->y + item->height)
            return item->controlIndex;
    }
    return -1;
}

Boolean DocumentSetControlValue(Document *document, UInt16 index, const Char *value)
{
    if (index >= document->controlCount || document->controls[index].type != 1) return false;
    StrNCopy(document->controls[index].value, value, sizeof(document->controls[index].value) - 1);
    document->controls[index].value[sizeof(document->controls[index].value) - 1] = '\0';
    return true;
}

const Char *DocumentGetControlValue(Document *document, UInt16 index)
{
    if (index >= document->controlCount) return NULL;
    return document->controls[index].value;
}

UInt8 DocumentGetControlType(Document *document, UInt16 index)
{
    if (index >= document->controlCount) return 0;
    return document->controls[index].type;
}

static Boolean AppendEncoded(Char *output, UInt16 capacity, UInt16 *used, const Char *text)
{
    static const Char hex[] = "0123456789ABCDEF";
    UInt8 c;
    while (*text) {
        c = (UInt8)*text++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            if (*used + 1 >= capacity) return false;
            output[(*used)++] = c;
        } else if (c == ' ') {
            if (*used + 1 >= capacity) return false;
            output[(*used)++] = '+';
        } else {
            if (*used + 3 >= capacity) return false;
            output[(*used)++] = '%';
            output[(*used)++] = hex[c >> 4];
            output[(*used)++] = hex[c & 15];
        }
    }
    output[*used] = '\0';
    return true;
}

Boolean DocumentBuildSubmitUrl(Document *document, UInt16 submitIndex,
                               Char *output, UInt16 capacity)
{
    DocumentControl *submit;
    DocumentForm *form;
    UInt16 used;
    UInt16 i;
    Boolean first;
    if (submitIndex >= document->controlCount) return false;
    submit = &document->controls[submitIndex];
    if (submit->formIndex >= document->formCount) return false;
    form = &document->forms[submit->formIndex];
    if (StrCompare(form->method, "post") == 0) return false;
    if (StrLen(form->action) + 2 > capacity) return false;
    StrCopy(output, form->action);
    used = StrLen(output);
    output[used++] = StrChr(output, '?') ? '&' : '?';
    output[used] = '\0';
    first = true;
    for (i = 0; i < document->controlCount; ++i) {
        DocumentControl *control;
        control = &document->controls[i];
        if (control->formIndex != submit->formIndex || !control->name[0]) continue;
        if (control->type == 2 && i != submitIndex) continue;
        if (!first) {
            if (used + 1 >= capacity) return false;
            output[used++] = '&';
        }
        first = false;
        if (!AppendEncoded(output, capacity, &used, control->name)) return false;
        if (used + 1 >= capacity) return false;
        output[used++] = '=';
        output[used] = '\0';
        if (!AppendEncoded(output, capacity, &used, control->value)) return false;
    }
    return true;
}

Boolean DocumentBuildPost(Document *document, UInt16 submitIndex,
                          Char *action, UInt16 actionCapacity,
                          Char *body, UInt16 bodyCapacity)
{
    DocumentControl *submit;
    DocumentForm *form;
    UInt16 used;
    UInt16 i;
    Boolean first;
    if (submitIndex >= document->controlCount) return false;
    submit = &document->controls[submitIndex];
    if (submit->formIndex >= document->formCount) return false;
    form = &document->forms[submit->formIndex];
    if (StrCompare(form->method, "post") != 0) return false;
    if (StrLen(form->action) + 1 > actionCapacity) return false;
    StrCopy(action, form->action);
    used = 0;
    body[0] = '\0';
    first = true;
    for (i = 0; i < document->controlCount; ++i) {
        DocumentControl *control;
        control = &document->controls[i];
        if (control->formIndex != submit->formIndex || !control->name[0]) continue;
        if (control->type == 2 && i != submitIndex) continue;
        if (!first) {
            if (used + 1 >= bodyCapacity) return false;
            body[used++] = '&';
        }
        first = false;
        if (!AppendEncoded(body, bodyCapacity, &used, control->name)) return false;
        if (used + 1 >= bodyCapacity) return false;
        body[used++] = '=';
        body[used] = '\0';
        if (!AppendEncoded(body, bodyCapacity, &used, control->value)) return false;
    }
    return true;
}

const Char *DocumentLinkAt(Document *document, Coord screenX, Coord screenY,
                           const RectangleType *bounds)
{
    UInt16 i;
    DocumentItem *item;
    Coord documentX;
    Coord documentY;
    documentX = screenX - bounds->topLeft.x;
    documentY = screenY - bounds->topLeft.y + document->scrollY;
    for (i = 0; i < document->itemCount; ++i) {
        item = &document->items[i];
        if (item->linkOffset && documentX >= item->x && documentX < item->x + item->width &&
            documentY >= item->y && documentY < item->y + item->height)
            return document->pool + item->linkOffset;
    }
    return NULL;
}

Boolean DocumentHighlightLinkAt(Document *document, Coord screenX, Coord screenY,
                                const RectangleType *bounds)
{
    UInt16 i;
    UInt16 linkOffset;
    DocumentItem *item;
    Coord documentX;
    Coord documentY;
    Coord screenItemY;
    RectangleType highlight;
    Boolean found;
    documentX = screenX - bounds->topLeft.x;
    documentY = screenY - bounds->topLeft.y + document->scrollY;
    linkOffset = 0;
    for (i = 0; i < document->itemCount; ++i) {
        item = &document->items[i];
        if (item->linkOffset && documentX >= item->x && documentX < item->x + item->width &&
            documentY >= item->y && documentY < item->y + item->height) {
            linkOffset = item->linkOffset;
            break;
        }
    }
    if (!linkOffset) return false;
    found = false;
    for (i = 0; i < document->itemCount; ++i) {
        item = &document->items[i];
        if (item->linkOffset != linkOffset) continue;
        screenItemY = bounds->topLeft.y + item->y - document->scrollY;
        if (screenItemY + item->height <= bounds->topLeft.y ||
            screenItemY >= bounds->topLeft.y + bounds->extent.y) continue;
        highlight.topLeft.x = bounds->topLeft.x + item->x;
        highlight.topLeft.y = screenItemY;
        highlight.extent.x = item->width;
        highlight.extent.y = item->height;
        WinInvertRectangle(&highlight, 0);
        found = true;
    }
    return found;
}

void DocumentScroll(Document *document, Int16 delta, Coord viewportHeight)
{
    Int32 target;
    Int32 maximum;
    maximum = document->contentHeight > viewportHeight ?
              document->contentHeight - viewportHeight : 0;
    target = (Int32)document->scrollY + delta;
    if (target < 0) target = 0;
    if (target > maximum) target = maximum;
    document->scrollY = (Coord)target;
}
