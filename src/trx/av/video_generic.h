#pragma once

#include <trx/gl/api.h>

#include <libavutil/pixfmt.h>

// The layout the decoder hands video frames over in, and how the texture
// they go into reads them.
#define VIDEO_PIXEL_FORMAT AV_PIX_FMT_BGRA
#define VIDEO_TEX_FORMAT GL_BGRA
#define VIDEO_TEX_TYPE GL_UNSIGNED_INT_8_8_8_8_REV
