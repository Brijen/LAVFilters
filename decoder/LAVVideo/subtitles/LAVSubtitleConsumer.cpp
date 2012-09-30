/*
 *      Copyright (C) 2010-2012 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "stdafx.h"
#include "LAVSubtitleConsumer.h"

#define OFFSET(x) offsetof(LAVSubtitleConsumerContext, x)
static const SubRenderOption options[] = {
  { "name",           OFFSET(name),            SROPT_TYPE_STRING, SROPT_FLAG_READONLY },
  { "version",        OFFSET(version),         SROPT_TYPE_STRING, SROPT_FLAG_READONLY },
  { 0 }
};

#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

CLAVSubtitleConsumer::CLAVSubtitleConsumer(void)
  : CSubRenderOptionsImpl(::options, &context)
  , CUnknown(L"CLAVSubtitleConsumer", NULL)
  , m_pProvider(NULL)
  , m_SubtitleFrame(NULL)
  , m_evFrame(FALSE)
  , m_pSwsContext(NULL)
  , m_PixFmt(LAVPixFmt_None)
{
  ZeroMemory(&context, sizeof(context));
  context.name = TEXT(LAV_VIDEO);
  context.version = TEXT(LAV_VERSION_STR);
  m_evFrame.Reset();
}

CLAVSubtitleConsumer::~CLAVSubtitleConsumer(void)
{
  if (m_pProvider) {
    m_pProvider->Disconnect();
  }
  Disconnect();
}

STDMETHODIMP CLAVSubtitleConsumer::Connect(ISubRenderProvider *subtitleRenderer)
{
  SafeRelease(&m_pProvider);
  m_pProvider = subtitleRenderer;
  return S_OK;
}

STDMETHODIMP CLAVSubtitleConsumer::Disconnect(void)
{
  SafeRelease(&m_pProvider);
  if (m_pSwsContext) {
    sws_freeContext(m_pSwsContext);
    m_pSwsContext = NULL;
  }
  return S_OK;
}

STDMETHODIMP CLAVSubtitleConsumer::DeliverFrame(REFERENCE_TIME start, REFERENCE_TIME stop, ISubRenderFrame *subtitleFrame)
{
  ASSERT(m_SubtitleFrame == NULL);
  m_SubtitleFrame = subtitleFrame;
  m_evFrame.Set();

  return S_OK;
}

STDMETHODIMP CLAVSubtitleConsumer::RequestFrame(REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
{
  CheckPointer(m_pProvider, E_FAIL);
  return m_pProvider->RequestFrame(rtStart, rtStop);
}

STDMETHODIMP CLAVSubtitleConsumer::ProcessFrame(LAVPixelFormat pixFmt, int bpp, DWORD dwWidth, DWORD dwHeight, BYTE *data[4], int stride[4])
{
  CheckPointer(m_pProvider, E_FAIL);

  // Wait for the requested frame
  m_evFrame.Wait();

  if (m_SubtitleFrame != NULL) {
    int count = 0;
    if (FAILED(m_SubtitleFrame->GetBitmapCount(&count))) {
      count = 0;
    }

    RECT videoRect;
    ::SetRect(&videoRect, 0, 0, dwWidth, dwHeight);

    RECT subRect;
    m_SubtitleFrame->GetOutputRect(&subRect);

    ULONGLONG id;
    POINT position;
    SIZE size;
    const uint8_t *rgbData;
    int pitch;
    for (int i = 0; i < count; i++) {
      if (FAILED(m_SubtitleFrame->GetBitmap(i, &id, &position, &size, (LPCVOID *)&rgbData, &pitch))) {
        DbgLog((LOG_TRACE, 10, L"GetBitmap() failed on index %d", i));
        break;
      }
      ProcessSubtitleBitmap(pixFmt, bpp, videoRect, data, stride, subRect, position, size, rgbData, pitch);
    }
    SafeRelease(&m_SubtitleFrame);
  }

  return S_OK;
}

static struct {
  LAVPixelFormat pixfmt;
  PixelFormat ffpixfmt;
} lav_ff_subtitle_pixfmt_map[] = {
  { LAVPixFmt_YUV420,   PIX_FMT_YUVA420P },
  { LAVPixFmt_YUV420bX, PIX_FMT_YUVA420P },
  { LAVPixFmt_YUV422,   PIX_FMT_YUVA422P },
  { LAVPixFmt_YUV422bX, PIX_FMT_YUVA422P },
  { LAVPixFmt_YUV444,   PIX_FMT_YUVA444P },
  { LAVPixFmt_YUV444bX, PIX_FMT_YUVA444P },
  { LAVPixFmt_NV12,     PIX_FMT_YUVA420P },
  { LAVPixFmt_YUY2,     PIX_FMT_YUVA422P },
  { LAVPixFmt_RGB24,    PIX_FMT_BGRA     },
  { LAVPixFmt_RGB32,    PIX_FMT_BGRA     },
  { LAVPixFmt_ARGB32,   PIX_FMT_BGRA     },
};

static LAVPixFmtDesc ff_sub_pixfmt_desc[] = {
  { 1, 4, { 1, 2, 2, 1 }, { 1, 2, 2, 1 } }, ///< PIX_FMT_YUVA420P
  { 1, 4, { 1, 2, 2, 1 }, { 1, 1, 1, 1 } }, ///< PIX_FMT_YUVA422P
  { 1, 4, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } }, ///< PIX_FMT_YUVA444P
  { 4, 1, { 1 },          { 1 }          }, ///< PIX_FMT_BGRA
};

static LAVPixFmtDesc getFFSubPixelFormatDesc(PixelFormat pixFmt)
{
  int index = 0;
  switch(pixFmt) {
  case PIX_FMT_YUVA420P:
    index = 0;
    break;
  case PIX_FMT_YUVA422P:
    index = 1;
    break;
  case PIX_FMT_YUVA444P:
    index = 2;
    break;
  case PIX_FMT_BGRA:
    index = 3;
    break;
  default:
    ASSERT(0);
  }
  return ff_sub_pixfmt_desc[index];
}

static PixelFormat getFFPixFmtForSubtitle(LAVPixelFormat pixFmt)
{
  PixelFormat fmt = PIX_FMT_NONE;
  for(int i = 0; i < countof(lav_ff_subtitle_pixfmt_map); i++) {
    if (lav_ff_subtitle_pixfmt_map[i].pixfmt == pixFmt) {
      return lav_ff_subtitle_pixfmt_map[i].ffpixfmt;
    }
  }
  ASSERT(0);
  return PIX_FMT_NONE;
}

STDMETHODIMP CLAVSubtitleConsumer::SelectBlendFunction()
{
  switch (m_PixFmt) {
  case LAVPixFmt_RGB32:
  case LAVPixFmt_RGB24:
    blend = &CLAVSubtitleConsumer::blend_rgb_c;
    break;
  case LAVPixFmt_YUV420:
  case LAVPixFmt_YUV422:
  case LAVPixFmt_YUV444:
    blend = &CLAVSubtitleConsumer::blend_yuv_c<uint8_t>;
    break;
  case LAVPixFmt_YUV420bX:
  case LAVPixFmt_YUV422bX:
  case LAVPixFmt_YUV444bX:
    blend = &CLAVSubtitleConsumer::blend_yuv_c<int16_t>;
    break;
  default:
    DbgLog((LOG_ERROR, 10, L"ProcessSubtitleBitmap(): No Blend function available"));
    blend = NULL;
  }

  return S_OK;
}

STDMETHODIMP CLAVSubtitleConsumer::ProcessSubtitleBitmap(LAVPixelFormat pixFmt, int bpp, RECT videoRect, BYTE *videoData[4], int videoStride[4], RECT subRect, POINT subPosition, SIZE subSize, const uint8_t *rgbData, int pitch)
{
  if (subRect.left != 0 || subRect.top != 0) {
    DbgLog((LOG_ERROR, 10, L"ProcessSubtitleBitmap(): Left/Top in SubRect non-zero"));
  }

  BOOL bNeedScaling = FALSE;

  // We need scaling if the width is not the same, or the subtitle rect is higher then the video rect
  if (subRect.right != videoRect.right || subRect.bottom > videoRect.bottom) {
    bNeedScaling = TRUE;
  }

  if (pixFmt != LAVPixFmt_RGB32 && pixFmt != LAVPixFmt_RGB24) {
    bNeedScaling = TRUE;
  }

  if (m_PixFmt != pixFmt) {
    m_PixFmt = pixFmt;
    SelectBlendFunction();
  }

  BYTE *subData[4] = { NULL, NULL, NULL, NULL };
  int subStride[4] = { 0, 0, 0, 0 };

  // If we need scaling (either scaling or pixel conversion), do it here before starting the blend process
  if (bNeedScaling) {
    uint8_t *tmpBuf = NULL;
    const PixelFormat avPixFmt = getFFPixFmtForSubtitle(pixFmt);

    // Calculate scaled size
    // We must ensure that the scaled subs still fit into the video
    float subAR = (float)subRect.right / (float)subRect.bottom;
    RECT newRect = subRect;
    if (newRect.right != videoRect.right) {
      newRect.right = videoRect.right;
      newRect.bottom = (LONG)(newRect.right / subAR);
    }
    if (newRect.bottom > videoRect.bottom) {
      newRect.bottom = videoRect.bottom;
      newRect.right = (LONG)(newRect.bottom * subAR);
    }

    SIZE newSize;
    newSize.cx = FFALIGN((LONG)av_rescale(subSize.cx, videoRect.right, newRect.right), 2);
    newSize.cy = FFALIGN((LONG)av_rescale(subSize.cy, videoRect.bottom, newRect.bottom), 2);

    // And scaled position
    subPosition.x = (LONG)av_rescale(subPosition.x, newSize.cx, subSize.cx);
    subPosition.y = (LONG)av_rescale(subPosition.y, newSize.cy, subSize.cy);

    m_pSwsContext = sws_getCachedContext(m_pSwsContext, subSize.cx, subSize.cy, PIX_FMT_BGRA, newSize.cx, newSize.cy, avPixFmt, SWS_BILINEAR | SWS_PRINT_INFO, NULL, NULL, NULL);

    const uint8_t *src[4] = { (const uint8_t *)rgbData, NULL, NULL, NULL };
    const int srcStride[4] = { pitch * 4, 0, 0, 0 };

    const LAVPixFmtDesc desc = getFFSubPixelFormatDesc(avPixFmt);
    const int stride = FFALIGN(newSize.cx, 64) * desc.codedbytes;

    for (int plane = 0; plane < desc.planes; plane++) {
      subStride[plane]  = stride / desc.planeWidth[plane];
      const size_t size = subStride[plane] * (newSize.cy / desc.planeHeight[plane]);
      subData[plane]    = (BYTE *)av_mallocz(size + FF_INPUT_BUFFER_PADDING_SIZE);
    }

    // Un-pre-multiply alpha for YUV formats
    // TODO: Can we SIMD this? See ARGBUnattenuateRow_C/SSE2 in libyuv
    if (avPixFmt != PIX_FMT_BGRA) {
      tmpBuf = (uint8_t *)av_malloc(pitch * subSize.cy * 4);
      memcpy(tmpBuf, rgbData, pitch * subSize.cy * 4);
      for (int line = 0; line < subSize.cy; line++) {
        uint8_t *p = tmpBuf + line * pitch * 4;
        for (int col = 0; col < subSize.cx; col++) {
          if (p[3] != 0 && p[3] != 255) {
            p[0] = av_clip_uint8(p[0] * 255 / p[3]);
            p[1] = av_clip_uint8(p[1] * 255 / p[3]);
            p[2] = av_clip_uint8(p[2] * 255 / p[3]);
          }
          p += 4;
        }
      }
      src[0] = tmpBuf;
    }

    int ret = sws_scale(m_pSwsContext, src, srcStride, 0, subSize.cy, subData, subStride);
    subSize = newSize;

    if (tmpBuf)
      av_free(tmpBuf);
  } else {
    subData[0] = (BYTE *)rgbData;
    subStride[0] = pitch * 4;
  }

  ASSERT((subPosition.x + subSize.cx) <= videoRect.right);
  ASSERT((subPosition.y + subSize.cy) <= videoRect.bottom);

  if (blend)
    (this->*blend)(videoData, videoStride, videoRect, subData, subStride, subPosition, subSize, pixFmt, bpp);

  if (bNeedScaling) {
    for (int i = 0; i < 4; i++) {
      av_freep(&subData[i]);
    }
  }

  return S_OK;
}
