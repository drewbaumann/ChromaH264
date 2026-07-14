//
//  chroma_h264.c
//  ChromaH264 — the only code in this framework the app can see. See chroma_h264.h.
//

#include "chroma_h264.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <string.h>

struct ch264_decoder {
    AVCodecContext *ctx;
    AVPacket *packet;
    AVFrame *frame;
    CVPixelBufferPoolRef pool;
    int pool_width;
    int pool_height;
};

// ---------------------------------------------------------------------------
// CVPixelBuffer plumbing
// ---------------------------------------------------------------------------

/// A 4320x2160 NV12 buffer is ~14MB. Allocating one per frame at 60fps is 840MB/s of
/// churn through the allocator on a device that is simultaneously rendering an immersive
/// scene at 90Hz. Pool them; the pool recycles buffers the renderer has finished with.
static CVPixelBufferPoolRef make_pool(int width, int height) {
    const void *pool_keys[]   = { kCVPixelBufferPoolMinimumBufferCountKey };
    int min_buffers           = 6;
    CFNumberRef min_ref       = CFNumberCreate(NULL, kCFNumberIntType, &min_buffers);
    const void *pool_values[] = { min_ref };
    CFDictionaryRef pool_attrs = CFDictionaryCreate(NULL, pool_keys, pool_values, 1,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);

    OSType fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;   // NV12
    CFNumberRef fmt_ref = CFNumberCreate(NULL, kCFNumberSInt32Type, &(SInt32){ (SInt32)fmt });
    CFNumberRef w_ref   = CFNumberCreate(NULL, kCFNumberIntType, &width);
    CFNumberRef h_ref   = CFNumberCreate(NULL, kCFNumberIntType, &height);
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);

    // Metal compatibility is not optional here: these buffers are handed to
    // AVSampleBufferVideoRenderer and end up as textures on a RealityKit entity. Without
    // it the frame takes a CPU round-trip somewhere in the compositor.
    const void *keys[] = {
        kCVPixelBufferPixelFormatTypeKey,
        kCVPixelBufferWidthKey,
        kCVPixelBufferHeightKey,
        kCVPixelBufferMetalCompatibilityKey,
        kCVPixelBufferIOSurfacePropertiesKey,
    };
    const void *values[] = { fmt_ref, w_ref, h_ref, kCFBooleanTrue, empty };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);

    CVPixelBufferPoolRef pool = NULL;
    CVPixelBufferPoolCreate(NULL, pool_attrs, attrs, &pool);

    CFRelease(attrs);
    CFRelease(empty);
    CFRelease(h_ref);
    CFRelease(w_ref);
    CFRelease(fmt_ref);
    CFRelease(pool_attrs);
    CFRelease(min_ref);
    return pool;
}

/// Tag the buffer so the renderer knows how to interpret the samples.
///
/// An untagged buffer is not "neutral" — downstream something picks a default, and if it
/// guesses differently from the encoder the picture comes out with the wrong colours and
/// nobody can tell you why. H.264 at this size is BT.709 in practice; carry through what
/// the bitstream declared when it declared anything.
static void tag_colour(CVPixelBufferRef pb, const AVCodecContext *ctx) {
    CFStringRef primaries = kCVImageBufferColorPrimaries_ITU_R_709_2;
    CFStringRef transfer  = kCVImageBufferTransferFunction_ITU_R_709_2;
    CFStringRef matrix    = kCVImageBufferYCbCrMatrix_ITU_R_709_2;

    if (ctx->color_primaries == AVCOL_PRI_BT2020) {
        primaries = kCVImageBufferColorPrimaries_ITU_R_2020;
        matrix    = kCVImageBufferYCbCrMatrix_ITU_R_2020;
    }
    if (ctx->color_trc == AVCOL_TRC_SMPTE2084) {
        transfer = kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    } else if (ctx->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        transfer = kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    }

    CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey,   primaries, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, transfer,  kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey,      matrix,    kCVAttachmentMode_ShouldPropagate);
}

/// yuv420p (3 planes, what the h264 decoder produces) → NV12 (2 planes, what CoreVideo
/// and the renderer want). Y copies row by row; U and V interleave into one plane.
///
/// Row-by-row rather than one big memcpy because the source stride (AVFrame linesize) and
/// the destination stride (CVPixelBuffer bytesPerRow) are both padded, independently, and
/// are not equal. Assuming they were is a classic way to get a picture with a diagonal
/// shear through it.
static void copy_yuv420p_to_nv12(const AVFrame *f, CVPixelBufferRef pb) {
    const int w = f->width;
    const int h = f->height;

    uint8_t *dst_y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const size_t dst_y_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const uint8_t *src_y = f->data[0];
    const int src_y_stride = f->linesize[0];
    for (int row = 0; row < h; row++) {
        memcpy(dst_y + (size_t)row * dst_y_stride, src_y + (size_t)row * src_y_stride, (size_t)w);
    }

    uint8_t *dst_uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t dst_uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    const uint8_t *src_u = f->data[1];
    const uint8_t *src_v = f->data[2];
    const int su = f->linesize[1];
    const int sv = f->linesize[2];
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    for (int row = 0; row < ch; row++) {
        uint8_t *out = dst_uv + (size_t)row * dst_uv_stride;
        const uint8_t *u = src_u + (size_t)row * su;
        const uint8_t *v = src_v + (size_t)row * sv;
        // Simple enough for the compiler to autovectorise into NEON; leave it simple.
        for (int col = 0; col < cw; col++) {
            out[2 * col]     = u[col];
            out[2 * col + 1] = v[col];
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ch264_decoder *ch264_create(const uint8_t *extradata, int extradata_size, int thread_count) {
    // Inside this dylib `avcodec_find_decoder` binds to OUR libavcodec, which is built
    // with --enable-decoder=h264. If this ever returns NULL again, symbol isolation has
    // regressed — that is the exact failure this framework exists to make impossible.
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;

    ch264_decoder *d = calloc(1, sizeof(ch264_decoder));
    if (!d) return NULL;

    d->ctx = avcodec_alloc_context3(codec);
    if (!d->ctx) { ch264_destroy(d); return NULL; }

    d->ctx->thread_count = thread_count;               // 0 = one per core
    d->ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (extradata && extradata_size > 0) {
        // AV_INPUT_BUFFER_PADDING_SIZE of slack: the bitstream readers over-read by
        // design, and a tight allocation here is a heap overread waiting to happen.
        d->ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->ctx->extradata) { ch264_destroy(d); return NULL; }
        memcpy(d->ctx->extradata, extradata, (size_t)extradata_size);
        d->ctx->extradata_size = extradata_size;
    }

    if (avcodec_open2(d->ctx, codec, NULL) < 0) { ch264_destroy(d); return NULL; }

    d->packet = av_packet_alloc();
    d->frame  = av_frame_alloc();
    if (!d->packet || !d->frame) { ch264_destroy(d); return NULL; }

    return d;
}

void ch264_destroy(ch264_decoder *d) {
    if (!d) return;
    if (d->frame)  av_frame_free(&d->frame);
    if (d->packet) av_packet_free(&d->packet);
    if (d->ctx)    avcodec_free_context(&d->ctx);
    if (d->pool)   CVPixelBufferPoolRelease(d->pool);
    free(d);
}

int ch264_thread_count(const ch264_decoder *d) {
    return (d && d->ctx) ? d->ctx->thread_count : 0;
}

void ch264_reset(ch264_decoder *d) {
    if (d && d->ctx) avcodec_flush_buffers(d->ctx);
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

int ch264_send(ch264_decoder *d, const uint8_t *data, int size, int64_t pts) {
    if (!d || !d->ctx || !d->packet) return CH264_ERR_ALLOC;
    d->packet->data = (uint8_t *)data;
    d->packet->size = size;
    d->packet->pts  = pts;
    int rc = avcodec_send_packet(d->ctx, d->packet);
    av_packet_unref(d->packet);
    if (rc == 0) return CH264_OK;
    if (rc == AVERROR(EAGAIN)) return CH264_NEED_MORE;   // receive first, then resend
    if (rc == AVERROR_EOF) return CH264_EOF;
    return CH264_ERR_DECODE;
}

int ch264_drain(ch264_decoder *d) {
    if (!d || !d->ctx) return CH264_ERR_ALLOC;
    int rc = avcodec_send_packet(d->ctx, NULL);
    return (rc == 0 || rc == AVERROR_EOF) ? CH264_OK : CH264_ERR_DECODE;
}

int ch264_receive(ch264_decoder *d, CVPixelBufferRef *out, int64_t *pts) {
    if (!d || !d->ctx || !d->frame || !out) return CH264_ERR_ALLOC;
    *out = NULL;

    int rc = avcodec_receive_frame(d->ctx, d->frame);
    if (rc == AVERROR(EAGAIN)) return CH264_NEED_MORE;
    if (rc == AVERROR_EOF)     return CH264_EOF;
    if (rc < 0)                return CH264_ERR_DECODE;

    const AVFrame *f = d->frame;

    // The decoder can only give us what the stream contains. 10-bit H.264 exists; this
    // path is 8-bit yuv420p only, and silently mangling a 10-bit frame into an 8-bit
    // buffer would produce a picture that is subtly, unexplainably wrong. Refuse instead.
    if (f->format != AV_PIX_FMT_YUV420P) {
        av_frame_unref(d->frame);
        return CH264_ERR_DECODE;
    }

    if (!d->pool || d->pool_width != f->width || d->pool_height != f->height) {
        if (d->pool) CVPixelBufferPoolRelease(d->pool);
        d->pool = make_pool(f->width, f->height);
        d->pool_width  = f->width;
        d->pool_height = f->height;
    }
    if (!d->pool) { av_frame_unref(d->frame); return CH264_ERR_ALLOC; }

    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferPoolCreatePixelBuffer(NULL, d->pool, &pb) != kCVReturnSuccess || !pb) {
        av_frame_unref(d->frame);
        return CH264_ERR_ALLOC;
    }

    CVPixelBufferLockBaseAddress(pb, 0);
    copy_yuv420p_to_nv12(f, pb);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    tag_colour(pb, d->ctx);

    if (pts) *pts = (f->pts != AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
    *out = pb;   // caller owns

    av_frame_unref(d->frame);
    return CH264_OK;
}
