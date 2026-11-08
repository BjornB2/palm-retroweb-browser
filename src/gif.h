#ifndef RETROWEB_GIF_H
#define RETROWEB_GIF_H

#include <PalmOS.h>

typedef Boolean (*GifEventPump)(void *context);

#define gifErrLzwDecode (appErrorClass | 1)

/* Reserve the LZW tables before decoded bitmaps fragment the Palm heap. */
Boolean GifPrepareDecoder(void);
void GifReleaseDecoder(void);

Boolean GifGetFirstFrameDimensions(const UInt8 *data, UInt32 length,
                                   UInt16 *widthOut, UInt16 *heightOut);

/* Decode the first frame of a GIF87a/GIF89a image to a Palm OS 8-bit bitmap. */
BitmapType *GifDecodeFirstFrame(UInt8 *data, UInt32 length,
                                UInt16 *widthOut, UInt16 *heightOut,
                                Err *errorOut, GifEventPump eventPump,
                                void *eventContext);

#endif

