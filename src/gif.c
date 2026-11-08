#include "gif.h"

#define GIF_MAX_CODES 4096

/* A 256-entry RGB palette is 1 KB; keep it off the Palm OS 68K stack. */
static RGBColorType gGifPalette[256];
static UInt8 gGifPalmPalette[256];
static UInt8 *gGifWorkspace = NULL;
static UInt16 *gGifPrefix = NULL;
static UInt8 *gGifSuffix = NULL;
static UInt8 *gGifStack = NULL;

Boolean GifPrepareDecoder(void)
{
    if (gGifWorkspace) return true;
    gGifWorkspace = (UInt8 *)MemPtrNew(GIF_MAX_CODES * 4);
    if (!gGifWorkspace) return false;
    gGifPrefix = (UInt16 *)gGifWorkspace;
    gGifSuffix = gGifWorkspace + GIF_MAX_CODES * sizeof(UInt16);
    gGifStack = gGifSuffix + GIF_MAX_CODES;
    return true;
}

void GifReleaseDecoder(void)
{
    if (gGifWorkspace) MemPtrFree(gGifWorkspace);
    gGifWorkspace = NULL;
    gGifPrefix = NULL;
    gGifSuffix = NULL;
    gGifStack = NULL;
}

typedef struct {
    const UInt8 *data;
    UInt32 length;
    UInt32 position;
} GifReader;

static Boolean ReadByte(GifReader *reader, UInt8 *value)
{
    if (reader->position >= reader->length) return false;
    *value = reader->data[reader->position++];
    return true;
}

static Boolean ReadWord(GifReader *reader, UInt16 *value)
{
    UInt8 low;
    UInt8 high;
    if (!ReadByte(reader, &low) || !ReadByte(reader, &high)) return false;
    *value = (UInt16)(low | ((UInt16)high << 8));
    return true;
}

static Boolean SkipBytes(GifReader *reader, UInt32 count)
{
    if (reader->position + count > reader->length) return false;
    reader->position += count;
    return true;
}

static Boolean SkipSubBlocks(GifReader *reader)
{
    UInt8 size;
    do {
        if (!ReadByte(reader, &size)) return false;
        if (!SkipBytes(reader, size)) return false;
    } while (size != 0);
    return true;
}

Boolean GifGetFirstFrameDimensions(const UInt8 *data, UInt32 length,
                                   UInt16 *widthOut, UInt16 *heightOut)
{
    GifReader reader;
    UInt16 logicalWidth;
    UInt16 logicalHeight;
    UInt16 width;
    UInt16 height;
    UInt8 packed;
    UInt8 ignored;
    UInt8 marker;
    UInt8 paletteBits;
    if (!data || length < 13 ||
        (MemCmp(data, "GIF87a", 6) != 0 && MemCmp(data, "GIF89a", 6) != 0))
        return false;
    reader.data = data;
    reader.length = length;
    reader.position = 6;
    if (!ReadWord(&reader, &logicalWidth) || !ReadWord(&reader, &logicalHeight) ||
        !ReadByte(&reader, &packed) || !ReadByte(&reader, &ignored) ||
        !ReadByte(&reader, &ignored)) return false;
    if (packed & 0x80) {
        paletteBits = (UInt8)((packed & 7) + 1);
        if (!SkipBytes(&reader, 3UL * (1UL << paletteBits))) return false;
    }
    for (;;) {
        if (!ReadByte(&reader, &marker)) return false;
        if (marker == 0x21) {
            if (!ReadByte(&reader, &ignored) || !SkipSubBlocks(&reader)) return false;
            continue;
        }
        if (marker != 0x2c) return false;
        if (!SkipBytes(&reader, 4) || !ReadWord(&reader, &width) ||
            !ReadWord(&reader, &height)) return false;
        if (!width || !height || width > 320 || height > 320) return false;
        if (widthOut) *widthOut = width;
        if (heightOut) *heightOut = height;
        return true;
    }
}

static UInt16 ReadCode(const UInt8 *compressed, UInt32 compressedLength,
                       UInt32 *bitPosition, UInt8 codeSize)
{
    UInt32 bytePosition;
    UInt32 value;
    UInt8 shift;
    bytePosition = *bitPosition >> 3;
    shift = (UInt8)(*bitPosition & 7);
    if (bytePosition >= compressedLength) return 0xffff;
    value = compressed[bytePosition];
    if (bytePosition + 1 < compressedLength)
        value |= ((UInt32)compressed[bytePosition + 1]) << 8;
    if (bytePosition + 2 < compressedLength)
        value |= ((UInt32)compressed[bytePosition + 2]) << 16;
    value >>= shift;
    *bitPosition += codeSize;
    return (UInt16)(value & ((1UL << codeSize) - 1));
}

typedef struct {
    UInt8 *bits;
    UInt16 width;
    UInt16 height;
    UInt16 rowBytes;
    UInt16 x;
    UInt16 row;
    UInt8 pass;
    Boolean interlaced;
} GifOutput;

static Boolean WritePixel(GifOutput *output, UInt8 paletteIndex)
{
    static const UInt8 starts[4] = { 0, 4, 2, 1 };
    static const UInt8 steps[4] = { 8, 8, 4, 2 };
    if (output->row >= output->height) return false;
    output->bits[(UInt32)output->row * output->rowBytes + output->x] =
        gGifPalmPalette[paletteIndex];
    output->x++;
    if (output->x < output->width) return true;
    output->x = 0;
    if (!output->interlaced) {
        output->row++;
        return true;
    }
    output->row += steps[output->pass];
    while (output->row >= output->height && output->pass < 3) {
        output->pass++;
        output->row = starts[output->pass];
    }
    return true;
}

static Boolean DecodeLzw(const UInt8 *compressed, UInt32 compressedLength,
                         UInt8 minimumCodeSize, GifOutput *output,
                         UInt32 pixelCount, GifEventPump eventPump,
                         void *eventContext)
{
    UInt16 *prefix;
    UInt8 *suffix;
    UInt8 *stack;
    UInt16 clearCode;
    UInt16 endCode;
    UInt16 nextCode;
    UInt16 oldCode;
    UInt16 code;
    UInt16 inCode;
    UInt16 stackSize;
    UInt8 codeSize;
    UInt8 first;
    UInt32 bitPosition;
    UInt32 outputPosition;
    UInt16 i;
    Boolean ok;

    if (minimumCodeSize > 8) return false;
    if (!GifPrepareDecoder()) return false;
    prefix = gGifPrefix;
    suffix = gGifSuffix;
    stack = gGifStack;
    clearCode = (UInt16)(1 << minimumCodeSize);
    endCode = clearCode + 1;
    nextCode = endCode + 1;
    codeSize = minimumCodeSize + 1;
    for (i = 0; i < clearCode; ++i) {
        prefix[i] = 0xffff;
        suffix[i] = (UInt8)i;
    }
    oldCode = 0xffff;
    first = 0;
    bitPosition = 0;
    outputPosition = 0;
    ok = true;

    while (outputPosition < pixelCount) {
        if ((outputPosition & 1023) == 0 && eventPump && !eventPump(eventContext)) {
            ok = false;
            break;
        }
        code = ReadCode(compressed, compressedLength, &bitPosition, codeSize);
        if (code == 0xffff) { ok = false; break; }
        if (code == clearCode) {
            codeSize = minimumCodeSize + 1;
            nextCode = endCode + 1;
            oldCode = 0xffff;
            continue;
        }
        if (code == endCode) break;
        if (oldCode == 0xffff) {
            if (code >= clearCode) { ok = false; break; }
            if (!WritePixel(output, suffix[code])) { ok = false; break; }
            outputPosition++;
            first = suffix[code];
            oldCode = code;
            continue;
        }

        inCode = code;
        stackSize = 0;
        if (code >= nextCode) {
            if (code != nextCode || stackSize >= GIF_MAX_CODES) { ok = false; break; }
            stack[stackSize++] = first;
            code = oldCode;
        }
        while (code >= clearCode) {
            if (code >= nextCode || stackSize >= GIF_MAX_CODES) { ok = false; break; }
            stack[stackSize++] = suffix[code];
            code = prefix[code];
        }
        if (!ok || code >= clearCode) { ok = false; break; }
        first = suffix[code];
        stack[stackSize++] = first;
        while (stackSize && outputPosition < pixelCount) {
            if (!WritePixel(output, stack[--stackSize])) { ok = false; break; }
            outputPosition++;
        }
        if (!ok) break;

        if (nextCode < GIF_MAX_CODES) {
            prefix[nextCode] = oldCode;
            suffix[nextCode] = first;
            nextCode++;
            if (nextCode == (UInt16)(1 << codeSize) && codeSize < 12) codeSize++;
        }
        oldCode = inCode;
    }
    if (outputPosition < pixelCount) ok = false;
    return ok;
}

BitmapType *GifDecodeFirstFrame(UInt8 *data, UInt32 length,
                                UInt16 *widthOut, UInt16 *heightOut,
                                Err *errorOut, GifEventPump eventPump,
                                void *eventContext)
{
    GifReader reader;
    UInt16 paletteSize;
    UInt16 logicalWidth;
    UInt16 logicalHeight;
    UInt16 left;
    UInt16 top;
    UInt16 width;
    UInt16 height;
    UInt8 packed;
    UInt8 background;
    UInt8 aspect;
    UInt8 marker;
    UInt8 label;
    UInt8 minimumCodeSize;
    UInt8 blockSize;
    UInt8 *compressed;
    UInt32 compressedLength;
    BitmapType *bitmap;
    UInt8 *bits;
    UInt16 bitmapWidth;
    UInt16 bitmapHeight;
    UInt16 rowBytes;
    UInt16 i;
    UInt16 localPaletteSize;
    Boolean interlaced;
    Err err;
    GifOutput output;

    if (errorOut) *errorOut = dmErrInvalidParam;
    if (!data || length < 13 ||
        (MemCmp(data, "GIF87a", 6) != 0 && MemCmp(data, "GIF89a", 6) != 0)) return NULL;
    reader.data = data;
    reader.length = length;
    reader.position = 6;
    if (!ReadWord(&reader, &logicalWidth) || !ReadWord(&reader, &logicalHeight) ||
        !ReadByte(&reader, &packed) || !ReadByte(&reader, &background) ||
        !ReadByte(&reader, &aspect)) return NULL;
    paletteSize = 0;
    if (packed & 0x80) {
        paletteSize = (UInt16)(1 << ((packed & 7) + 1));
        for (i = 0; i < paletteSize; ++i) {
            gGifPalette[i].index = (UInt8)i;
            if (!ReadByte(&reader, &gGifPalette[i].r) || !ReadByte(&reader, &gGifPalette[i].g) ||
                !ReadByte(&reader, &gGifPalette[i].b)) return NULL;
        }
    }

    for (;;) {
        if (!ReadByte(&reader, &marker)) return NULL;
        if (marker == 0x3b) return NULL;
        if (marker == 0x21) {
            if (!ReadByte(&reader, &label) || !SkipSubBlocks(&reader)) return NULL;
            continue;
        }
        if (marker != 0x2c) return NULL;
        if (!ReadWord(&reader, &left) || !ReadWord(&reader, &top) ||
            !ReadWord(&reader, &width) || !ReadWord(&reader, &height) ||
            !ReadByte(&reader, &packed)) return NULL;
        if (!width || !height || width > 320 || height > 320) return NULL;
        interlaced = (packed & 0x40) != 0;
        if (packed & 0x80) {
            localPaletteSize = (UInt16)(1 << ((packed & 7) + 1));
            paletteSize = localPaletteSize;
            for (i = 0; i < paletteSize; ++i) {
                gGifPalette[i].index = (UInt8)i;
                if (!ReadByte(&reader, &gGifPalette[i].r) || !ReadByte(&reader, &gGifPalette[i].g) ||
                    !ReadByte(&reader, &gGifPalette[i].b)) return NULL;
            }
        }
        break;
    }
    if (!paletteSize || !ReadByte(&reader, &minimumCodeSize)) return NULL;

    compressedLength = 0;
    /* Compact GIF data sub-blocks into the already-owned response buffer.
     * The destination always trails the unread source, so overlap is safe and
     * no second image-sized allocation is needed. */
    compressed = data;
    for (;;) {
        if (!ReadByte(&reader, &blockSize)) return NULL;
        if (blockSize == 0) break;
        if (reader.position + blockSize > reader.length) return NULL;
        MemMove(compressed + compressedLength, reader.data + reader.position, blockSize);
        compressedLength += blockSize;
        reader.position += blockSize;
    }

    bitmap = BmpCreate(width, height, 8, NULL, &err);
    if (!bitmap || err) {
        if (errorOut) *errorOut = err ? err : memErrNotEnoughSpace;
        return NULL;
    }
    BmpGetDimensions(bitmap, &bitmapWidth, &bitmapHeight, &rowBytes);
    bits = (UInt8 *)BmpGetBits(bitmap);
    for (i = 0; i < paletteSize; ++i)
        gGifPalmPalette[i] = WinRGBToIndex(&gGifPalette[i]);
    MemSet(&output, sizeof(output), 0);
    output.bits = bits;
    output.width = width;
    output.height = height;
    output.rowBytes = rowBytes;
    output.interlaced = interlaced;
    if (!DecodeLzw(compressed, compressedLength, minimumCodeSize, &output,
                   (UInt32)width * height, eventPump, eventContext)) {
        BmpDelete(bitmap);
        if (errorOut) *errorOut = gifErrLzwDecode;
        return NULL;
    }
    if (widthOut) *widthOut = width;
    if (heightOut) *heightOut = height;
    if (errorOut) *errorOut = errNone;
    return bitmap;
}
