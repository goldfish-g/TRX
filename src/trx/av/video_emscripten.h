#pragma once

#include <trx/gl/api.h>

#include <libavutil/pixfmt.h>

// The layout the decoder hands video frames over in, and how the texture
// they go into reads them. WebGL has no BGRA textures.
#define VIDEO_PIXEL_FORMAT AV_PIX_FMT_RGBA
#define VIDEO_TEX_FORMAT GL_RGBA
#define VIDEO_TEX_TYPE GL_UNSIGNED_BYTE
