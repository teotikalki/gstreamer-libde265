/*
 * GStreamer HEVC/H.265 video codec.
 *
 * Copyright (c) 2014 struktur AG, Joachim Bauch <bauch@struktur.de>
 *
 * This file is part of gstreamer-libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libde265-dec.h"

#if !defined(LIBDE265_NUMERIC_VERSION) || LIBDE265_NUMERIC_VERSION < 0x00070000
#error "You need libde265 0.7 or newer to compile this plugin."
#endif

#if !defined(LIBDE265_ENCODED_VERSION)
#define LIBDE265_ENCODED_VERSION(h, m, l) ((h) << 24 | (m) << 16 | (l) << 8)
#endif
#if !defined(LIBDE265_CHECK_VERSION)
#define LIBDE265_CHECK_VERSION(h, m, l) (LIBDE265_NUMERIC_VERSION >= LIBDE265_ENCODED_VERSION(h, m, l))
#endif

#if !LIBDE265_CHECK_VERSION(1,0,0)
// libde265 < 1.0 only supported 8 bits per pixel
#define de265_get_bits_per_pixel(image, plane) 8
#endif

#if !LIBDE265_CHECK_VERSION(2,0,0)
// "de265_flush_data" got renamed in 2.0.0
#define de265_push_end_of_stream de265_flush_data

#define GET_IMAGE_BITS_PER_PIXEL(img, spec, channel) \
    de265_get_bits_per_pixel(img, channel)
#define RELEASE_PICTURE(img)
#else
#define GET_IMAGE_BITS_PER_PIXEL(img, spec, channel) \
    de265_get_bits_per_pixel_from_spec(spec, channel)
#define RELEASE_PICTURE(img) de265_release_picture(img)
#endif

// use two decoder threads if no information about
// available CPU cores can be retrieved
#define DEFAULT_THREAD_COUNT        2

#define parent_class gst_libde265_dec_parent_class
G_DEFINE_TYPE (GstLibde265Dec, gst_libde265_dec, VIDEO_DECODER_TYPE);

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265")
    );

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
#if GST_CHECK_VERSION(1,0,0)
        GST_VIDEO_CAPS_MAKE ("{ I420 }")
#else
        GST_VIDEO_CAPS_YUV ("{ I420 }")
#endif
    )
    );

enum
{
  PROP_0,
  PROP_MODE,
  PROP_FRAMERATE,
  PROP_MAX_THREADS,
  PROP_LAST
};

#define DEFAULT_MODE        GST_TYPE_LIBDE265_DEC_PACKETIZED
#define DEFAULT_FPS_N       0
#define DEFAULT_FPS_D       1
#define DEFAULT_MAX_THREADS 0


#define GST_TYPE_LIBDE265_DEC_MODE (gst_libde265_dec_mode_get_type ())
static GType
gst_libde265_dec_mode_get_type (void)
{
  static GType libde265_dec_mode_type = 0;
  static const GEnumValue libde265_dec_mode_types[] = {
    {GST_TYPE_LIBDE265_DEC_PACKETIZED,
        "Packetized H.265 bitstream with packet lengths "
          "instead of startcodes", "packetized"},
    {GST_TYPE_LIBDE265_DEC_RAW,
        "Raw H.265 bitstream including startcodes", "raw"},
    {0, NULL, NULL}
  };

  if (!libde265_dec_mode_type) {
    libde265_dec_mode_type =
        g_enum_register_static ("GstLibde265DecMode", libde265_dec_mode_types);
  }
  return libde265_dec_mode_type;
}

static void gst_libde265_dec_finalize (GObject * object);

static void gst_libde265_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_libde265_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_libde265_dec_start (VIDEO_DECODER_BASE * parse);
static gboolean gst_libde265_dec_stop (VIDEO_DECODER_BASE * parse);
static gboolean gst_libde265_dec_set_format (VIDEO_DECODER_BASE * parse,
    VIDEO_STATE * state);
#if GST_CHECK_VERSION(1,2,0)
static gboolean gst_libde265_dec_flush (VIDEO_DECODER_BASE * parse);
#elif GST_CHECK_VERSION(1,0,0)
static gboolean gst_libde265_dec_reset (VIDEO_DECODER_BASE * parse,
    gboolean hard);
#else
static gboolean gst_libde265_dec_reset (VIDEO_DECODER_BASE * parse);
#endif
static GstFlowReturn gst_libde265_dec_handle_frame (VIDEO_DECODER_BASE * parse,
    VIDEO_FRAME * frame);
static GstFlowReturn _gst_libde265_image_available (VIDEO_DECODER_BASE * parse,
    int width, int height, GstVideoFormat format);

static void
gst_libde265_dec_class_init (GstLibde265DecClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  VIDEO_DECODER_CLASS *decoder_class = VIDEO_DECODER_GET_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_libde265_dec_finalize;
  gobject_class->set_property = gst_libde265_dec_set_property;
  gobject_class->get_property = gst_libde265_dec_get_property;

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Input mode",
          "Input mode of data to decode", GST_TYPE_LIBDE265_DEC_MODE,
          DEFAULT_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FRAMERATE, gst_param_spec_fraction ("framerate", "Frame Rate", "Frame rate of images in raw stream", 0, 1,       // min
          100, 1,               // max
          DEFAULT_FPS_N, DEFAULT_FPS_D,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_THREADS,
      g_param_spec_int ("max-threads", "Maximum decode threads",
          "Maximum number of worker threads to spawn. (0 = auto)",
          0, G_MAXINT, DEFAULT_MAX_THREADS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  decoder_class->start = GST_DEBUG_FUNCPTR (gst_libde265_dec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_libde265_dec_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_libde265_dec_set_format);
#if GST_CHECK_VERSION(1,2,0)
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_libde265_dec_flush);
#else
  decoder_class->reset = GST_DEBUG_FUNCPTR (gst_libde265_dec_reset);
#endif
  decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_libde265_dec_handle_frame);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_details_simple (gstelement_class,
      "HEVC/H.265 parser",
      "Codec/Parser/Converter/Video",
      "Decodes HEVC/H.265 video streams using libde265",
      "struktur AG <opensource@struktur.de>");
}

static inline void
_gst_libde265_dec_reset_decoder (GstLibde265Dec * dec)
{
  dec->ctx = NULL;
  dec->width = -1;
  dec->height = -1;
  dec->buffer_full = 0;
  dec->codec_data = NULL;
  dec->codec_data_size = 0;
#if GST_CHECK_VERSION(1,0,0)
  dec->frame_number = -1;
  dec->input_state = NULL;
  dec->output_state = NULL;
#endif
}

static void
gst_libde265_dec_init (GstLibde265Dec * dec)
{
  dec->mode = DEFAULT_MODE;
  dec->fps_n = DEFAULT_FPS_N;
  dec->fps_d = DEFAULT_FPS_D;
  dec->max_threads = DEFAULT_MAX_THREADS;
  dec->length_size = 4;
  _gst_libde265_dec_reset_decoder (dec);
#if GST_CHECK_VERSION(1,0,0)
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (dec), TRUE);
#else
  dec->parent.packetized = TRUE;
#endif
}

static inline void
_gst_libde265_dec_free_decoder (GstLibde265Dec * dec)
{
  if (dec->ctx != NULL) {
    de265_free_decoder (dec->ctx);
  }
  free (dec->codec_data);
#if GST_CHECK_VERSION(1,0,0)
  if (dec->input_state != NULL) {
    gst_video_codec_state_unref (dec->input_state);
  }
  if (dec->output_state != NULL) {
    gst_video_codec_state_unref (dec->output_state);
  }
#endif
  _gst_libde265_dec_reset_decoder (dec);
}

static void
gst_libde265_dec_finalize (GObject * object)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  _gst_libde265_dec_free_decoder (dec);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_libde265_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  switch (prop_id) {
    case PROP_MODE:
      dec->mode = g_value_get_enum (value);
      GST_DEBUG ("Mode set to %d", dec->mode);
      break;
    case PROP_FRAMERATE:
      dec->fps_n = gst_value_get_fraction_numerator (value);
      dec->fps_d = gst_value_get_fraction_denominator (value);
      GST_DEBUG ("Framerate set to %d/%d", dec->fps_n, dec->fps_d);
      break;
    case PROP_MAX_THREADS:
      dec->max_threads = g_value_get_int (value);
      if (dec->max_threads) {
        GST_DEBUG_OBJECT (dec, "Max. threads set to %d", dec->max_threads);
      } else {
        GST_DEBUG_OBJECT (dec, "Max. threads set to auto");
      }
      break;
    default:
      break;
  }
}

static void
gst_libde265_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, dec->mode);
      break;
    case PROP_FRAMERATE:
      gst_value_set_fraction (value, dec->fps_n, dec->fps_d);
      break;
    case PROP_MAX_THREADS:
      g_value_set_int (value, dec->max_threads);
      break;
    default:
      break;
  }
}

static inline GstVideoFormat
_gst_libde265_get_video_format (enum de265_chroma chroma, int bits_per_pixel)
{
  GstVideoFormat result = GST_VIDEO_FORMAT_UNKNOWN;
  switch (chroma) {
    case de265_chroma_mono:
      result = GST_VIDEO_FORMAT_GRAY8;
      break;
    case de265_chroma_420:
#if GST_CHECK_VERSION(1,0,0)
      switch (bits_per_pixel) {
        case 8:
          result = GST_VIDEO_FORMAT_I420;
          break;
        case 9:
          result = GST_VIDEO_FORMAT_I420_10LE;
          break;
        case 10:
          result = GST_VIDEO_FORMAT_I420_10LE;
          break;
        default:
          if (bits_per_pixel > 10 && bits_per_pixel <= 16) {
            result = GST_VIDEO_FORMAT_I420_10LE;
          } else {
            GST_DEBUG
                ("Unsupported output colorspace %d with %d bits per pixel",
                chroma, bits_per_pixel);
          }
          break;
      }
#else
      result = GST_VIDEO_FORMAT_I420;
#endif
      break;
    case de265_chroma_422:
#if GST_CHECK_VERSION(1,0,0)
      switch (bits_per_pixel) {
        case 8:
          result = GST_VIDEO_FORMAT_Y42B;
          break;
        case 9:
          result = GST_VIDEO_FORMAT_I422_10LE;
          break;
        case 10:
          result = GST_VIDEO_FORMAT_I422_10LE;
          break;
        default:
          if (bits_per_pixel > 10 && bits_per_pixel <= 16) {
            result = GST_VIDEO_FORMAT_I422_10LE;
          } else {
            GST_DEBUG
                ("Unsupported output colorspace %d with %d bits per pixel",
                chroma, bits_per_pixel);
          }
          break;
      }
#else
      result = GST_VIDEO_FORMAT_Y42B;
#endif
      break;
    case de265_chroma_444:
#if GST_CHECK_VERSION(1,0,0)
      switch (bits_per_pixel) {
        case 8:
          result = GST_VIDEO_FORMAT_Y444;
          break;
        case 9:
          result = GST_VIDEO_FORMAT_Y444_10LE;
          break;
        case 10:
          result = GST_VIDEO_FORMAT_Y444_10LE;
          break;
        default:
          if (bits_per_pixel > 10 && bits_per_pixel <= 16) {
            result = GST_VIDEO_FORMAT_Y444_10LE;
          } else {
            GST_DEBUG
                ("Unsupported output colorspace %d with %d bits per pixel",
                chroma, bits_per_pixel);
          }
          break;
      }
#else
      result = GST_VIDEO_FORMAT_Y444;
#endif
      break;
    default:
      GST_DEBUG ("Unsupported output colorspace %d", chroma);
      break;
  }
  return result;
}

/*
 * Direct rendering code needs GStreamer 1.0
 * to have support for refcounted frames.
 */
#if GST_CHECK_VERSION(1,0,0)
struct GstLibde265FrameRef
{
  VIDEO_DECODER_BASE *decoder;
  VIDEO_FRAME *frame;
  GstVideoFrame vframe;
  GstBuffer *buffer;
  int mapped;
};

#if !LIBDE265_CHECK_VERSION(2,0,0)
static inline enum de265_chroma
_gst_libde265_image_format_to_chroma (enum de265_image_format format)
{
  switch (format) {
    case de265_image_format_mono8:
      return de265_chroma_mono;
    case de265_image_format_YUV420P8:
      return de265_chroma_420;
    case de265_image_format_YUV422P8:
      return de265_chroma_422;
    case de265_image_format_YUV444P8:
      return de265_chroma_444;
    default:
      g_assert (0);
      return 0;
  }
}
#endif

static void
gst_libde265_dec_release_frame_ref (struct GstLibde265FrameRef *ref)
{
  if (ref->mapped) {
    gst_video_frame_unmap (&ref->vframe);
  }
  gst_video_codec_frame_unref (ref->frame);
  gst_buffer_replace (&ref->buffer, NULL);
  g_free (ref);
}

static int
#if !LIBDE265_CHECK_VERSION(2,0,0)
gst_libde265_dec_get_buffer (de265_decoder_context * ctx,
    struct de265_image_spec *spec, struct de265_image *img, void *userdata)
#else
gst_libde265_dec_get_buffer (struct de265_image_intern *img,
    const struct de265_image_spec *spec, void *userdata)
#endif
{
  VIDEO_DECODER_BASE *base = (VIDEO_DECODER_BASE *) userdata;
  GstLibde265Dec *dec = GST_LIBDE265_DEC (base);
  VIDEO_FRAME *frame;
  int i;

  frame = GET_FRAME (base, dec->frame_number);
  if (G_UNLIKELY (frame == NULL)) {
    // should not happen...
    GST_WARNING_OBJECT (base, "Couldn't get codec frame !");
    goto fallback;
  }

  GST_VIDEO_CODEC_FRAME_FLAG_UNSET (frame,
      GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);

  int width =
      (spec->width + spec->alignment - 1) / spec->alignment * spec->alignment;
  int height = spec->height;

  if (width != spec->visible_width || height != spec->visible_height) {
    // clipping not supported for now
    goto fallback;
  }

  enum de265_chroma chroma =
#if !LIBDE265_CHECK_VERSION(2,0,0)
      _gst_libde265_image_format_to_chroma (spec->format);
#else
      spec->chroma;
#endif
  if (chroma != de265_chroma_mono) {
    if (GET_IMAGE_BITS_PER_PIXEL (img, spec,
            0) != GET_IMAGE_BITS_PER_PIXEL (img, spec, 1)
        || GET_IMAGE_BITS_PER_PIXEL (img, spec,
            0) != GET_IMAGE_BITS_PER_PIXEL (img, spec, 2)
        || GET_IMAGE_BITS_PER_PIXEL (img, spec,
            1) != GET_IMAGE_BITS_PER_PIXEL (img, spec, 2)) {
      GST_DEBUG_OBJECT (dec,
          "input format has multiple bits per pixel (%d/%d/%d)",
          GET_IMAGE_BITS_PER_PIXEL (img, spec, 0),
          GET_IMAGE_BITS_PER_PIXEL (img, spec, 1),
          GET_IMAGE_BITS_PER_PIXEL (img, spec, 2));
      goto fallback;
    }
  }

  int bits_per_pixel = GET_IMAGE_BITS_PER_PIXEL (img, spec, 0);
  GstVideoFormat format =
      _gst_libde265_get_video_format (chroma, bits_per_pixel);
  if (format == GST_VIDEO_FORMAT_UNKNOWN) {
    goto fallback;
  }

  const GstVideoFormatInfo *format_info = gst_video_format_get_info (format);
  if (GST_VIDEO_FORMAT_INFO_BITS (format_info) != bits_per_pixel) {
    GST_DEBUG_OBJECT (dec,
        "output format doesn't provide enough bits per pixel (%d/%d)",
        GST_VIDEO_FORMAT_INFO_BITS (format_info), bits_per_pixel);
    goto fallback;
  }

  GstFlowReturn ret = _gst_libde265_image_available (base, width, height,
      format);
  if (G_UNLIKELY (ret != GST_FLOW_OK)) {
    GST_ERROR_OBJECT (dec, "Failed to notify about available image");
    goto fallback;
  }

  ret = ALLOC_OUTPUT_FRAME (GST_VIDEO_DECODER (dec), frame);
  if (G_UNLIKELY (ret != GST_FLOW_OK)) {
    GST_ERROR_OBJECT (dec, "Failed to allocate output buffer");
    goto fallback;
  }

  struct GstLibde265FrameRef *ref =
      (struct GstLibde265FrameRef *) g_malloc0 (sizeof (*ref));
  g_assert (ref != NULL);
  ref->decoder = base;
  ref->frame = frame;

  gst_buffer_replace (&ref->buffer, frame->output_buffer);
  gst_buffer_replace (&frame->output_buffer, NULL);

  GstVideoInfo *info = &dec->output_state->info;
  if (!gst_video_frame_map (&ref->vframe, info, ref->buffer, GST_MAP_READWRITE)) {
    GST_ERROR_OBJECT (dec, "Failed to map frame output buffer");
    goto error;
  }

  ref->mapped = TRUE;
  if (GST_VIDEO_FRAME_PLANE_STRIDE (&ref->vframe,
          0) < width * GST_VIDEO_FRAME_COMP_PSTRIDE (&ref->vframe, 0)) {
    GST_DEBUG_OBJECT (dec, "plane 0: pitch too small (%d/%d*%d)",
        GST_VIDEO_FRAME_PLANE_STRIDE (&ref->vframe, 0), width,
        GST_VIDEO_FRAME_COMP_PSTRIDE (&ref->vframe, 0));
    goto error;
  }

  if (GST_VIDEO_FRAME_COMP_HEIGHT (&ref->vframe, 0) < height) {
    GST_DEBUG_OBJECT (dec, "plane 0: lines too few (%d/%d)",
        GST_VIDEO_FRAME_COMP_HEIGHT (&ref->vframe, 0), height);
    goto error;
  }

  for (i = 0; i < 3; i++) {
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE (&ref->vframe, i);
    if (stride % spec->alignment) {
      GST_DEBUG_OBJECT (dec, "plane %d: pitch not aligned (%d%%%d)",
          i, stride, spec->alignment);
      goto error;
    }

    uint8_t *data = GST_VIDEO_FRAME_PLANE_DATA (&ref->vframe, i);
    if ((uintptr_t) (data) % spec->alignment) {
      GST_DEBUG_OBJECT (dec, "plane %d not aligned", i);
      goto error;
    }
#if !LIBDE265_CHECK_VERSION(2,0,0)
    de265_set_image_plane (img, i, data, stride, ref);
#else
    de265_set_image_plane_intern (img, i, data, stride, ref);
#endif
  }
  return 1;

error:
  gst_libde265_dec_release_frame_ref (ref);

fallback:
#if !LIBDE265_CHECK_VERSION(2,0,0)
  return de265_get_default_image_allocation_functions ()->get_buffer (ctx,
      spec, img, userdata);
#else
  return de265_get_default_image_allocation_functions ()->get_buffer (img,
      spec, userdata);
#endif
}

static void
#if !LIBDE265_CHECK_VERSION(2,0,0)
gst_libde265_dec_release_buffer (de265_decoder_context * ctx,
    struct de265_image *img, void *userdata)
#else
gst_libde265_dec_release_buffer (struct de265_image_intern *img, void *userdata)
#endif
{
  VIDEO_DECODER_BASE *base = (VIDEO_DECODER_BASE *) userdata;
  struct GstLibde265FrameRef *ref = (struct GstLibde265FrameRef *)
#if !LIBDE265_CHECK_VERSION(2,0,0)
      de265_get_image_plane_user_data (img, 0);
#else
      de265_get_image_plane_user_data_intern (img, 0);
#endif
  if (ref == NULL) {
    de265_get_default_image_allocation_functions ()->release_buffer (
#if !LIBDE265_CHECK_VERSION(2,0,0)
        ctx,
#endif
        img, userdata);
    return;
  }
  gst_libde265_dec_release_frame_ref (ref);
  (void) base;                  // unused
}
#endif

static gboolean
gst_libde265_dec_start (VIDEO_DECODER_BASE * parse)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);
  int threads = dec->max_threads;

  _gst_libde265_dec_free_decoder (dec);
  dec->ctx = de265_new_decoder ();
  if (dec->ctx == NULL) {
    return FALSE;
  }
#if LIBDE265_CHECK_VERSION(2,0,0)
  de265_set_CPU_capabilities (dec->ctx, de265_cpu_capability_all);
#endif

  if (threads == 0) {
#if defined(_SC_NPROC_ONLN)
    threads = sysconf (_SC_NPROC_ONLN);
#elif defined(_SC_NPROCESSORS_ONLN)
    threads = sysconf (_SC_NPROCESSORS_ONLN);
#else
#warning "Don't know how to get number of CPU cores, will use the default thread count"
    threads = DEFAULT_THREAD_COUNT;
#endif
    if (threads <= 0) {
      threads = DEFAULT_THREAD_COUNT;
    }
#if !LIBDE265_CHECK_VERSION(2,0,0)
    // XXX: We start more threads than cores for now, as some threads
    // might get blocked while waiting for dependent data. Having more
    // threads increases decoding speed by about 10%
    threads *= 2;
#endif
  }
#if LIBDE265_CHECK_VERSION(2,0,0)
  de265_set_max_decode_frames_parallel (dec->ctx, threads > 1 ? threads : 1);
#endif
  if (threads > 1) {
    if (threads > 32) {
      // TODO: this limit should come from the libde265 headers
      threads = 32;
    }
    de265_start_worker_threads (dec->ctx, threads);
#if LIBDE265_CHECK_VERSION(2,0,0)
  } else {
    // Make sure to start at least one processing thread.
    de265_start_worker_threads (dec->ctx, 1);
#endif
  }
  GST_INFO ("Using libde265 %s with %d worker threads", de265_get_version (),
      threads);

#if GST_CHECK_VERSION(1,0,0)
  struct de265_image_allocation allocation;
  allocation.get_buffer = gst_libde265_dec_get_buffer;
  allocation.release_buffer = gst_libde265_dec_release_buffer;
#if LIBDE265_CHECK_VERSION(2,0,0)
  allocation.allocation_userdata = parse;
  de265_set_image_allocation_functions (dec->ctx, &allocation);
#else
  de265_set_image_allocation_functions (dec->ctx, &allocation, parse);
#endif
#endif

#if !LIBDE265_CHECK_VERSION(2,0,0)
  // NOTE: we explicitly disable hash checks for now
  de265_set_parameter_bool (dec->ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH,
      0);
#else
  de265_suppress_faulty_pictures (dec->ctx, 0);
#endif
  return TRUE;
}

static gboolean
gst_libde265_dec_stop (VIDEO_DECODER_BASE * parse)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);

  _gst_libde265_dec_free_decoder (dec);

  return TRUE;
}

#if GST_CHECK_VERSION(1,2,0)
static gboolean
gst_libde265_dec_flush (VIDEO_DECODER_BASE * parse)
#elif GST_CHECK_VERSION(1,0,0)
static gboolean
gst_libde265_dec_reset (VIDEO_DECODER_BASE * parse, gboolean hard)
#else
static gboolean
gst_libde265_dec_reset (VIDEO_DECODER_BASE * parse)
#endif
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);

  de265_reset (dec->ctx);
  dec->buffer_full = 0;
  if (dec->codec_data != NULL && dec->mode == GST_TYPE_LIBDE265_DEC_RAW) {
#if !LIBDE265_CHECK_VERSION(2,0,0)
    int more;
#endif
    de265_error err =
        de265_push_data (dec->ctx, dec->codec_data, dec->codec_data_size, 0,
        NULL);
    if (!de265_isOK (err)) {
      GST_ELEMENT_ERROR (parse, STREAM, DECODE,
          ("Failed to push codec data: %s (code=%d)",
              de265_get_error_text (err), err), (NULL));
      return FALSE;
    }
    de265_push_end_of_NAL (dec->ctx);
#if !LIBDE265_CHECK_VERSION(2,0,0)
    do {
      err = de265_decode (dec->ctx, &more);
      switch (err) {
        case DE265_OK:
          break;

        case DE265_ERROR_IMAGE_BUFFER_FULL:
        case DE265_ERROR_WAITING_FOR_INPUT_DATA:
          // not really an error
          more = 0;
          break;

        default:
          if (!de265_isOK (err)) {
            GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                ("Failed to decode codec data: %s (code=%d)",
                    de265_get_error_text (err), err), (NULL));
            return FALSE;
          }
      }
    } while (more);
#endif
  }

  return TRUE;
}

static GstFlowReturn
_gst_libde265_image_available (VIDEO_DECODER_BASE * parse,
    int width, int height, GstVideoFormat format)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);

  if (G_UNLIKELY (width != dec->width || height != dec->height)) {
#if GST_CHECK_VERSION(1,0,0)
    GstVideoCodecState *state =
        gst_video_decoder_set_output_state (parse, format, width,
        height, dec->input_state);
    g_assert (state != NULL);
    if (dec->fps_n > 0) {
      state->info.fps_n = dec->fps_n;
      state->info.fps_d = dec->fps_d;
    } else if (state->info.fps_d == 0
        || (state->info.fps_n / (float) state->info.fps_d) > 1000) {
      // TODO(fancycode): is 24/1 a sane default or can we get it from the container somehow?
      GST_WARNING ("Framerate is too high (%d/%d), defaulting to 24/1",
          state->info.fps_n, state->info.fps_d);
      state->info.fps_n = 24;
      state->info.fps_d = 1;
    }
    gst_video_decoder_negotiate (parse);
    if (dec->output_state != NULL) {
      gst_video_codec_state_unref (dec->output_state);
    }
    dec->output_state = state;
#else
    GstVideoState *state = gst_base_video_decoder_get_state (parse);
    g_assert (state != NULL);
    state->format = format;
    state->width = width;
    state->height = height;
    if (dec->fps_n > 0) {
      state->fps_n = dec->fps_n;
      state->fps_d = dec->fps_d;
    } else if (state->fps_d == 0
        || (state->fps_n / (float) state->fps_d) > 1000) {
      // TODO(fancycode): is 24/1 a sane default or can we get it from the container somehow?
      GST_WARNING ("Framerate is too high (%d/%d), defaulting to 24/1",
          state->fps_n, state->fps_d);
      state->fps_n = 24;
      state->fps_d = 1;
    }
    gst_base_video_decoder_set_src_caps (parse);
#endif
    GST_DEBUG ("Frame dimensions are %d x %d", width, height);
    dec->width = width;
    dec->height = height;
  }

  return GST_FLOW_OK;
}

static gboolean
gst_libde265_dec_set_format (VIDEO_DECODER_BASE * parse, VIDEO_STATE * state)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);

#if GST_CHECK_VERSION(1,0,0)
  if (dec->input_state != NULL) {
    gst_video_codec_state_unref (dec->input_state);
  }
  dec->input_state = state;
  if (state != NULL) {
    gst_video_codec_state_ref (state);
  }
#endif
  if (state != NULL && state->caps != NULL) {
    GstStructure *str;
    const GValue *value;
    str = gst_caps_get_structure (state->caps, 0);
    if ((value = gst_structure_get_value (str, "codec_data"))) {
#if GST_CHECK_VERSION(1,0,0)
      GstMapInfo info;
#endif
      guint8 *data;
      gsize size;
      GstBuffer *buf;
      de265_error err;
#if !LIBDE265_CHECK_VERSION(2,0,0)
      int more;
#endif

      buf = gst_value_get_buffer (value);
#if GST_CHECK_VERSION(1,0,0)
      if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
        GST_ELEMENT_ERROR (parse, STREAM, DECODE,
            ("Failed to map codec data"), (NULL));
        return FALSE;
      }
      data = info.data;
      size = info.size;
#else
      data = GST_BUFFER_DATA (buf);
      size = GST_BUFFER_SIZE (buf);
#endif
      free (dec->codec_data);
      dec->codec_data = malloc (size);
      g_assert (dec->codec_data != NULL);
      dec->codec_data_size = size;
      memcpy (dec->codec_data, data, size);
      if (size > 3 && (data[0] || data[1] || data[2] > 1)) {
        // encoded in "hvcC" format (assume version 0)
        dec->mode = GST_TYPE_LIBDE265_DEC_PACKETIZED;
        if (size > 22) {
          int i;
          if (data[0] != 0) {
            GST_ELEMENT_WARNING (parse, STREAM,
                DECODE, ("Unsupported extra data version %d, decoding may fail",
                    data[0]), (NULL));
          }
          dec->length_size = (data[21] & 3) + 1;
          int num_param_sets = data[22];
          int pos = 23;
          for (i = 0; i < num_param_sets; i++) {
            int j;
            if (pos + 3 > size) {
              GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                  ("Buffer underrun in extra header (%d >= %ld)", pos + 3,
                      size), (NULL));
              return FALSE;
            }
            // ignore flags + NAL type (1 byte)
            int nal_count = data[pos + 1] << 8 | data[pos + 2];
            pos += 3;
            for (j = 0; j < nal_count; j++) {
              if (pos + 2 > size) {
                GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                    ("Buffer underrun in extra nal header (%d >= %ld)", pos + 2,
                        size), (NULL));
                return FALSE;
              }
              int nal_size = data[pos] << 8 | data[pos + 1];
              if (pos + 2 + nal_size > size) {
                GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                    ("Buffer underrun in extra nal (%d >= %ld)",
                        pos + 2 + nal_size, size), (NULL));
                return FALSE;
              }
              err =
                  de265_push_NAL (dec->ctx, data + pos + 2, nal_size, 0, NULL);
              if (!de265_isOK (err)) {
                GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                    ("Failed to push data: %s (%d)", de265_get_error_text (err),
                        err), (NULL));
                return FALSE;
              }
              pos += 2 + nal_size;
            }
          }
        }
        GST_DEBUG ("Assuming packetized data (%d bytes length)",
            dec->length_size);
      } else {
        dec->mode = GST_TYPE_LIBDE265_DEC_RAW;
        GST_DEBUG ("Assuming non-packetized data");
        err = de265_push_data (dec->ctx, data, size, 0, NULL);
        if (!de265_isOK (err)) {
#if GST_CHECK_VERSION(1,0,0)
          gst_buffer_unmap (buf, &info);
#endif
          GST_ELEMENT_ERROR (parse, STREAM, DECODE,
              ("Failed to push codec data: %s (code=%d)",
                  de265_get_error_text (err), err), (NULL));
          return FALSE;
        }
      }
#if GST_CHECK_VERSION(1,0,0)
      gst_buffer_unmap (buf, &info);
#endif
      de265_push_end_of_NAL (dec->ctx);
#if !LIBDE265_CHECK_VERSION(2,0,0)
      do {
        err = de265_decode (dec->ctx, &more);
        switch (err) {
          case DE265_OK:
            break;

          case DE265_ERROR_IMAGE_BUFFER_FULL:
          case DE265_ERROR_WAITING_FOR_INPUT_DATA:
            // not really an error
            more = 0;
            break;

          default:
            if (!de265_isOK (err)) {
              GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                  ("Failed to decode codec data: %s (code=%d)",
                      de265_get_error_text (err), err), (NULL));
              return FALSE;
            }
        }
      } while (more);
#endif
    } else if ((value = gst_structure_get_value (str, "stream-format"))) {
      const gchar *str = g_value_get_string (value);
      if (strcmp (str, "byte-stream") == 0) {
        dec->mode = GST_TYPE_LIBDE265_DEC_RAW;
        GST_DEBUG ("Assuming raw byte-stream");
      }
    }
  }

  return TRUE;
}

static GstFlowReturn
gst_libde265_dec_handle_frame (VIDEO_DECODER_BASE * parse, VIDEO_FRAME * frame)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (parse);
  uint8_t *frame_data;
  uint8_t *end_data;
  const struct de265_image *img;
  de265_error ret = DE265_OK;
#if !LIBDE265_CHECK_VERSION(2,0,0)
  int more = 0;
#endif
  de265_PTS pts = (de265_PTS) FRAME_PTS (frame);
  gsize size;

#if GST_CHECK_VERSION(1,0,0)
  GstMapInfo info;
  if (!gst_buffer_map (frame->input_buffer, &info, GST_MAP_READ)) {
    GST_ERROR_OBJECT (dec, "Failed to map input buffer");
    return GST_FLOW_ERROR;
  }

  frame_data = info.data;
  size = info.size;
#else
  frame_data = GST_BUFFER_DATA (frame->sink_buffer);
  size = GST_BUFFER_SIZE (frame->sink_buffer);
#endif
  end_data = frame_data + size;

#if GST_CHECK_VERSION(1,0,0)
  GST_VIDEO_CODEC_FRAME_FLAG_SET (frame,
      GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);
#endif
  if (size > 0) {
    if (dec->mode == GST_TYPE_LIBDE265_DEC_PACKETIZED) {
      // stream contains length fields and NALs
      uint8_t *start_data = frame_data;
      while (start_data + dec->length_size <= end_data) {
        int nal_size = 0;
        int i;
        for (i = 0; i < dec->length_size; i++) {
          nal_size = (nal_size << 8) | start_data[i];
        }
        if (start_data + dec->length_size + nal_size > end_data) {
          GST_ELEMENT_ERROR (parse, STREAM, DECODE,
              ("Overflow in input data, check data mode"), (NULL));
          goto error_input;
        }
        ret =
            de265_push_NAL (dec->ctx, start_data + dec->length_size, nal_size,
            pts, NULL);
        if (ret != DE265_OK) {
          GST_ELEMENT_ERROR (parse, STREAM, DECODE,
              ("Error while pushing data: %s (code=%d)",
                  de265_get_error_text (ret), ret), (NULL));
          goto error_input;
        }
        start_data += dec->length_size + nal_size;
      }
    } else {
      ret = de265_push_data (dec->ctx, frame_data, size, pts, NULL);
      if (ret != DE265_OK) {
        GST_ELEMENT_ERROR (parse, STREAM, DECODE,
            ("Error while pushing data: %s (code=%d)",
                de265_get_error_text (ret), ret), (NULL));
        goto error_input;
      }
    }
  } else {
    ret = de265_push_end_of_stream (dec->ctx);
    if (ret != DE265_OK) {
      GST_ELEMENT_ERROR (parse, STREAM, DECODE,
          ("Error while flushing data: %s (code=%d)",
              de265_get_error_text (ret), ret), (NULL));
      goto error_input;
    }
  }
#if GST_CHECK_VERSION(1,0,0)
  gst_buffer_unmap (frame->input_buffer, &info);
#endif

  // decode as much as possible
#if GST_CHECK_VERSION(1,0,0)
  dec->frame_number = frame->system_frame_number;
#endif

#if !LIBDE265_CHECK_VERSION(2,0,0)
  do {
    ret = de265_decode (dec->ctx, &more);
  } while (more && ret == DE265_OK);

  switch (ret) {
    case DE265_OK:
    case DE265_ERROR_WAITING_FOR_INPUT_DATA:
      break;

    case DE265_ERROR_IMAGE_BUFFER_FULL:
      dec->buffer_full = 1;
      if ((img = de265_peek_next_picture (dec->ctx)) == NULL) {
        return GST_FLOW_OK;
      }
      break;

    default:
      GST_ELEMENT_ERROR (parse, STREAM, DECODE,
          ("Error while decoding: %s (code=%d)", de265_get_error_text (ret),
              ret), (NULL));
      return GST_FLOW_ERROR;
  }
#else
  ret = de265_get_action (dec->ctx, 1);
  // TODO(fancycode): Evaluate response?
#endif

  while ((ret = de265_get_warning (dec->ctx)) != DE265_OK) {
    GST_ELEMENT_WARNING (parse, STREAM, DECODE,
        ("%s (code=%d)", de265_get_error_text (ret), ret), (NULL));
  }

  img = de265_get_next_picture (dec->ctx);
  if (img == NULL) {
    // need more data
#if GST_CHECK_VERSION(1,0,0)
    gst_video_codec_frame_unref (frame);
#endif
    return GST_FLOW_OK;
  }
#if GST_CHECK_VERSION(1,0,0)
  struct GstLibde265FrameRef *ref =
      (struct GstLibde265FrameRef *) de265_get_image_plane_user_data (img, 0);
  if (ref != NULL) {
    // decoder is using direct rendering
    gst_video_codec_frame_unref (frame);
    VIDEO_FRAME *out_frame = gst_video_codec_frame_ref (ref->frame);
    gst_buffer_replace (&out_frame->output_buffer, ref->buffer);
    gst_buffer_replace (&ref->buffer, NULL);
    FRAME_PTS (out_frame) = (GstClockTime) de265_get_image_PTS (img);
    ret = FINISH_FRAME (parse, out_frame);
    RELEASE_PICTURE (img);
    return ret;
  }

  GST_VIDEO_CODEC_FRAME_FLAG_UNSET (frame,
      GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);
#endif

  int bits_per_pixel = MAX (MAX (de265_get_bits_per_pixel (img, 0),
          de265_get_bits_per_pixel (img, 1)), de265_get_bits_per_pixel (img,
          2));

  GstVideoFormat format =
      _gst_libde265_get_video_format (de265_get_chroma_format (img),
      bits_per_pixel);
  if (format == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (dec, "Unsupported image format");
    RELEASE_PICTURE (img);
    return GST_FLOW_ERROR;
  }

  GstFlowReturn result =
      _gst_libde265_image_available (parse, de265_get_image_width (img, 0),
      de265_get_image_height (img, 0), format);
  if (result != GST_FLOW_OK) {
    GST_ERROR_OBJECT (dec, "Failed to notify about available image");
    RELEASE_PICTURE (img);
    return result;
  }

  result = ALLOC_OUTPUT_FRAME (parse, frame);
  if (result != GST_FLOW_OK) {
    GST_ERROR_OBJECT (dec, "Failed to allocate output frame");
    RELEASE_PICTURE (img);
    return result;
  }

  uint8_t *dest;
#if GST_CHECK_VERSION(1,0,0)
  if (!gst_buffer_map (frame->output_buffer, &info, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (dec, "Failed to map output buffer");
    RELEASE_PICTURE (img);
    return GST_FLOW_ERROR;
  }

  dest = info.data;
#else
  dest = GST_BUFFER_DATA (frame->src_buffer);
#endif

#if GST_CHECK_VERSION(1,0,0)
  const GstVideoFormatInfo *format_info = gst_video_format_get_info (format);
  int max_bits_per_pixel = GST_VIDEO_FORMAT_INFO_BITS (format_info);
#else
  int max_bits_per_pixel = 8;
#endif

  int plane;
  for (plane = 0; plane < 3; plane++) {
    int stride;
    int pos;
    int width = de265_get_image_width (img, plane);
    int height = de265_get_image_height (img, plane);
    const uint8_t *src = de265_get_image_plane (img, plane, &stride);
    int dst_stride = width * ((max_bits_per_pixel + 7) / 8);
    int plane_bits_per_pixel = de265_get_bits_per_pixel (img, plane);
    if (plane_bits_per_pixel > max_bits_per_pixel && max_bits_per_pixel > 8) {
      // More bits per pixel in this plane than supported by the output format
      int shift = (plane_bits_per_pixel - max_bits_per_pixel);
      int size = MIN (stride, dst_stride);
      while (height--) {
        uint16_t *s = (uint16_t *) src;
        uint16_t *d = (uint16_t *) dest;
        for (pos = 0; pos < size / 2; pos++) {
          *d = *s >> shift;
          d++;
          s++;
        }
        src += stride;
        dest += dst_stride;
      }
    } else if (plane_bits_per_pixel > max_bits_per_pixel
        && max_bits_per_pixel == 8) {
      // More bits per pixel in this plane than supported by the output format
      int shift = (plane_bits_per_pixel - max_bits_per_pixel);
      int size = MIN (stride, dst_stride);
      while (height--) {
        uint16_t *s = (uint16_t *) src;
        uint8_t *d = (uint8_t *) dest;
        for (pos = 0; pos < size; pos++) {
          *d = *s >> shift;
          d++;
          s++;
        }
        src += stride;
        dest += dst_stride;
      }
    } else if (plane_bits_per_pixel < max_bits_per_pixel
        && plane_bits_per_pixel > 8) {
      // Less bits per pixel in this plane than the rest of the picture
      // but more than 8bpp.
      int shift = (plane_bits_per_pixel - max_bits_per_pixel);
      int size = MIN (stride, dst_stride);
      while (height--) {
        uint16_t *s = (uint16_t *) src;
        uint16_t *d = (uint16_t *) dest;
        for (pos = 0; pos < size / 2; pos++) {
          *d = *s >> shift;
          d++;
          s++;
        }
        src += stride;
        dest += dst_stride;
      }
    } else if (plane_bits_per_pixel < max_bits_per_pixel
        && plane_bits_per_pixel == 8) {
      // 8 bits per pixel in this plane, which is less than the rest of the picture.
      int shift = (max_bits_per_pixel - plane_bits_per_pixel);
      int size = MIN (stride, dst_stride);
      while (height--) {
        uint8_t *s = (uint8_t *) src;
        uint16_t *d = (uint16_t *) dest;
        for (pos = 0; pos < size; pos++) {
          *d = *s << shift;
          d++;
          s++;
        }
        src += stride;
        dest += dst_stride;
      }
    } else {
      // Bits per pixel of image match output format.
      if (stride == width) {
        memcpy (dest, src, height * stride);
        dest += (height * stride);
      } else {
        while (height--) {
          memcpy (dest, src, width);
          src += stride;
          dest += width;
        }
      }
    }
  }
#if GST_CHECK_VERSION(1,0,0)
  gst_buffer_unmap (frame->output_buffer, &info);
#endif
  FRAME_PTS (frame) = (GstClockTime) de265_get_image_PTS (img);
  RELEASE_PICTURE (img);
  return FINISH_FRAME (parse, frame);

error_input:
#if GST_CHECK_VERSION(1,0,0)
  gst_buffer_unmap (frame->input_buffer, &info);
#endif
  return GST_FLOW_ERROR;
}

gboolean
gst_libde265_dec_plugin_init (GstPlugin * plugin)
{
  /* create an elementfactory for the libde265 decoder element */
  if (!gst_element_register (plugin, "libde265dec",
          GST_RANK_PRIMARY, GST_TYPE_LIBDE265_DEC))
    return FALSE;

  return TRUE;
}
