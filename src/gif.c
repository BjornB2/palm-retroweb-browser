#include "gif.h"

#define GIF_MAX_CODES 4096

/* A 256-entry RGB palette is 1 KB; keep it off the Palm OS 68K stack. */
static RGBColorType gGifPalette[256];

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

static Boolean DecodeLzw(const UInt8 *compressed, UInt32 compressedLength,
                         UInt8 minimumCodeSize, UInt8 *pixels,
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
    prefix = (UInt16 *)MemPtrNew(GIF_MAX_CODES * sizeof(UInt16));
    suffix = (UInt8 *)MemPtrNew(GIF_MAX_CODES);
    stack = (UInt8 *)MemPtrNew(GIF_MAX_CODES);
    if (!prefix || !suffix || !stack) {
        if (prefix) MemPtrFree(prefix);
        if (suffix) MemPtrFree(suffix);
        if (stack) MemPtrFree(stack);
        return false;
    }
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
            pixels[outputPosition++] = suffix[code];
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
        while (stackSize && outputPosition < pixelCount)
            pixels[outputPosition++] = stack[--stackSize];

        if (nextCode < GIF_MAX_CODES) {
            prefix[nextCode] = oldCode;
            suffix[nextCode] = first;
            nextCode++;
            if (nextCode == (UInt16)(1 << codeSize) && codeSize < 12) codeSize++;
        }
        oldCode = inCode;
    }
    if (outputPosition < pixelCount) ok = false;
    MemPtrFree(prefix);
    MemPtrFree(suffix);
    MemPtrFree(stack);
    return ok;
}

static Boolean Deinterlace(UInt8 *pixels, UInt16 width, UInt16 height)
{
    static const UInt8 starts[4] = { 0, 4, 2, 1 };
    static const UInt8 steps[4] = { 8, 8, 4, 2 };
    UInt8 *copy;
    UInt32 sourceOffset;
    UInt16 pass;
    UInt16 row;
    copy = (UInt8 *)MemPtrNew((UInt32)width * height);
    if (!copy) return false;
    MemMove(copy, pixels, (UInt32)width * height);
    sourceOffset = 0;
    for (pass = 0; pass < 4; ++pass) {
        for (row = starts[pass]; row < height; row += steps[pass]) {
            MemMove(pixels + (UInt32)row * width, copy + sourceOffset, width);
            sourceOffset += width;
        }
    }
    MemPtrFree(copy);
    return true;
}

BitmapType *GifDecodeFirstFrame(const UInt8 *data, UInt32 length,
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
    UInt32 compressedCapacity;
    UInt8 *grown;
    UInt8 *pixels;
    BitmapType *bitmap;
    UInt8 *bits;
    UInt16 bitmapWidth;
    UInt16 bitmapHeight;
    UInt16 rowBytes;
    UInt16 x;
    UInt16 y;
    UInt16 i;
    UInt16 localPaletteSize;
    Boolean interlaced;
    Err err;

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

    compressedCapacity = 1024;
    compressedLength = 0;
    compressed = (UInt8 *)MemPtrNew(compressedCapacity);
    if (!compressed) return NULL;
    for (;;) {
        if (!ReadByte(&reader, &blockSize)) { MemPtrFree(compressed); return NULL; }
        if (blockSize == 0) break;
        if (reader.position + blockSize > reader.length) { MemPtrFree(compressed); return NULL; }
        if (compressedLength + blockSize > compressedCapacity) {
            UInt32 newCapacity;
            newCapacity = compressedCapacity * 2;
            while (newCapacity < compressedLength + blockSize) newCapacity *= 2;
            grown = (UInt8 *)MemPtrNew(newCapacity);
            if (!grown) { MemPtrFree(compressed); return NULL; }
            MemMove(grown, compressed, compressedLength);
            MemPtrFree(compressed);
            compressed = grown;
            compressedCapacity = newCapacity;
        }
        MemMove(compressed + compressedLength, reader.data + reader.position, blockSize);
        compressedLength += blockSize;
        reader.position += blockSize;
    }

    pixels = (UInt8 *)MemPtrNew((UInt32)width * height);
    if (!pixels) { MemPtrFree(compressed); return NULL; }
    if (!DecodeLzw(compressed, compressedLength, minimumCodeSize, pixels,
                   (UInt32)width * height, eventPump, eventContext)) {
        MemPtrFree(compressed);
        MemPtrFree(pixels);
        return NULL;
    }
    MemPtrFree(compressed);
    if (interlaced && !Deinterlace(pixels, width, height)) {
        MemPtrFree(pixels);
        return NULL;
    }

    bitmap = BmpCreate(width, height, 8, NULL, &err);
    if (!bitmap || err) {
        MemPtrFree(pixels);
        if (errorOut) *errorOut = err;
        return NULL;
    }
    BmpGetDimensions(bitmap, &bitmapWidth, &bitmapHeight, &rowBytes);
    bits = (UInt8 *)BmpGetBits(bitmap);
    for (y = 0; y < height; ++y) {
        if (eventPump && !eventPump(eventContext)) {
            BmpDelete(bitmap);
            MemPtrFree(pixels);
            return NULL;
        }
        for (x = 0; x < width; ++x) {
            UInt8 paletteIndex;
            paletteIndex = pixels[(UInt32)y * width + x];
            bits[(UInt32)y * rowBytes + x] = WinRGBToIndex(&gGifPalette[paletteIndex]);
        }
    }
    MemPtrFree(pixels);
    if (widthOut) *widthOut = width;
    if (heightOut) *heightOut = height;
    if (errorOut) *errorOut = errNone;
    return bitmap;
}
