//
//  chroma_h264.h
//  ChromaH264 — a self-contained software H.264 decoder framework.
//
//  THIS HEADER IS THE ENTIRE SEAM, AND THAT IS THE POINT.
//
//  Everything below is compiled into a DYNAMIC framework whose export list contains only
//  these `ch264_*` symbols; every FFmpeg symbol inside is hidden and resolves within the
//  dylib. Callers never see `avcodec_*` at all, so the framework cannot collide with any
//  other FFmpeg build a host app might link. (Two FFmpeg builds cannot safely share
//  symbols in one address space anyway — different configure flags mean different struct
//  layouts, so cross-bound calls are ABI roulette.) That shape is also what the LGPL
//  wants — dynamic linkage, relinkable.
//
//  The C boundary also means CVPixelBuffer construction happens HERE, next to the frame,
//  instead of marshalling AVFrame internals across into Swift.
//

#ifndef CHROMA_H264_H
#define CHROMA_H264_H

#include <stdint.h>
#include <CoreVideo/CoreVideo.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The framework is compiled -fvisibility=hidden so that no FFmpeg symbol can escape and
/// collide with another libavcodec in the host process. That hides EVERYTHING, including
/// these — and an export list cannot re-export a symbol the compiler already marked
/// private_extern. So the seven functions that are meant to be public must say so
/// explicitly.
///
/// The build's isolation self-check exists for exactly this: a dylib exporting nothing
/// links, loads, and fails at the call.
#define CH264_API __attribute__((visibility("default")))

typedef struct ch264_decoder ch264_decoder;

/// Result codes. Deliberately distinct from "0 = ok" so a caller cannot mistake
/// "need more data" (routine, happens constantly) for success.
typedef enum {
    CH264_OK          =  0,   ///< frame produced
    CH264_NEED_MORE   =  1,   ///< not an error: feed another packet
    CH264_EOF         =  2,   ///< drained
    CH264_ERR_DECODE  = -1,
    CH264_ERR_ALLOC   = -2,
    CH264_ERR_NO_CODEC = -3,  ///< should now be UNREACHABLE — it was the symptom of the
                              ///< symbol collision this whole framework exists to prevent
} ch264_status;

/// `extradata` is the avcC atom from the sample description. AVAssetReader hands us
/// AVCC (length-prefixed) samples, not Annex-B; without avcC every packet is garbage.
///
/// `thread_count` 0 = auto (one per core). Frame threading plus NEON is where a software
/// H.264 decoder's throughput comes from — do not disable either.
CH264_API ch264_decoder *ch264_create(const uint8_t *extradata, int extradata_size, int thread_count);
CH264_API void ch264_destroy(ch264_decoder *d);

/// What the decoder actually chose, once open. Log it — "I asked for threads" and
/// "I got threads" are different claims.
CH264_API int ch264_thread_count(const ch264_decoder *d);

/// Feed one compressed AVCC sample. `pts` is carried through to the matching frame.
CH264_API int ch264_send(ch264_decoder *d, const uint8_t *data, int size, int64_t pts);

/// Signal end-of-stream so buffered frames drain out.
CH264_API int ch264_drain(ch264_decoder *d);

/// Pull a decoded frame, converted to an NV12 CVPixelBuffer drawn from an internal pool.
/// On CH264_OK the caller OWNS `*out` and must CFRelease it.
CH264_API int ch264_receive(ch264_decoder *d, CVPixelBufferRef *out, int64_t *pts);

/// Drop buffered state. Call on seek — otherwise the decoder happily emits frames from
/// before the seek and the picture jumps backwards.
CH264_API void ch264_reset(ch264_decoder *d);

#ifdef __cplusplus
}
#endif

#endif /* CHROMA_H264_H */
