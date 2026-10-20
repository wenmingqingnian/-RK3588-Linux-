#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <cstdint>
#include <cstddef>

// Rockchip MPP hardware encoder for YUV420P → H.264/H.265.
//
// Uses dlopen to load librockchip_mpp.so at runtime, so the application
// compiles without MPP SDK headers. On systems without MPP the encode()
// method returns false and callers should skip encoding.
//
// Based on project1 MppEncoder API flow:
//   mpp_create(ctx, mpi) -> mpp_init(ctx, ENC, type) -> mpp_enc_cfg_set_s32()
//   -> mpp_buffer_group_get(group, type, mode, usage, caller) [ION buffer pool]
//   -> mpp_buffer_get_with_tag(buf, size, tag, caller) [default internal group]
//   -> encode_put_frame/encode_get_packet -> mpp_enc_get_extra_info (SPS/PPS)
//
// Thread safety: not thread-safe. Each thread should use its own instance.

namespace mpp {

// Encoding codec type.
enum class CodecType {
    H264,
    H265
};

// Encoder configuration parameters.
struct EncoderConfig {
    CodecType codec;
    int width;
    int height;
    int fps;
    int bitrate;
    int gop;
};

// Check if MPP hardware encoder is available on this system.
// Returns true if librockchip_mpp.so loaded successfully.
bool isAvailable();

// Check if the current encoder is configured for H.265.
// Returns true if H.265, false if H.264 or encoder not initialized.
bool isH265();

// Initialize the MPP encoder with given configuration.
// Must be called once before using encode(). Safe to call multiple times.
// Returns true if MPP is available and encoder initialized successfully.
bool initEncoder(const EncoderConfig &config);

// Release MPP encoder resources. Safe to call even if init failed.
void deinitEncoder();

// Encode a YUV420P frame to H.264/H.265 bitstream using MPP hardware.
//
//   yuvData             : input YUV420P frame (width * height * 3/2 bytes)
//   outEncoded          : caller-allocated output buffer (Annex-B format)
//   outSize             : actual encoded size in bytes (set on success)
//   maxOutSize          : size of outEncoded buffer
//
// Returns true on success, false on error.
bool encode(const uint8_t *yuvData, uint8_t *outEncoded,
            size_t &outSize, size_t maxOutSize);

// Request that the next encoded frame be an IDR (keyframe).
//
// Thread-safe: sets an atomic flag that is checked by encode() before each
// encode_put_frame() call. The actual MPP control command is issued from
// inside encode() (which runs on the encoder thread), NOT from the caller's
// thread. This avoids concurrent access to the MPP API, which is not
// thread-safe.
//
// Use case: when an RTSP client sends a PLAY request, the server calls
// forceIDR() so the client receives an IDR frame immediately and can start
// decoding without waiting for the next GOP boundary (which at 30fps with
// GOP=30 can be up to 1 second).
//
// Returns true if the request was recorded (encoder is initialized),
// false if the encoder is not ready (request is a no-op in that case).
bool forceIDR();

// Get SPS/PPS header data from the encoder (for SDP and IDR prepending).
// Must be called after at least one successful encode().
//
//   outHeader           : caller-allocated output buffer
//   headerSize          : actual header size in bytes (set on success)
//   maxOutSize          : size of outHeader buffer
//
// Returns true on success, false on error.
bool getHeader(uint8_t *outHeader, size_t &headerSize, size_t maxOutSize);

} // namespace mpp

#endif // MPP_ENCODER_H
