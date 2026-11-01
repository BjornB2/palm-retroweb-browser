#ifndef RETROWEB_GIF_H
#define RETROWEB_GIF_H

#include <PalmOS.h>

typedef Boolean (*GifEventPump)(void *context);

/* Decode the first frame of a GIF87a/GIF89a image to a Palm OS 8-bit bitmap. */
BitmapType *GifDecodeFirstFrame(const UInt8 *data, UInt32 length,
                                UInt16 *widthOut, UInt16 *heightOut,
                                Err *errorOut, GifEventPump eventPump,
                                void *eventContext);

#endif

