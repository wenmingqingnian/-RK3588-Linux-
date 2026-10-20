#ifndef MPP_DECODER_H
#define MPP_DECODER_H

#include <cstdint>
#include <cstddef>

// Rockchip MPP hardware decoder for MJPEG → NV12.
//
// Uses dlopen to load librockchip_mpp.so at runtime, so the application
// compiles without MPP SDK headers. On systems without MPP the decode()
// method returns false and callers should fall back to software decoding.
//
// Thread safety: not thread-safe. Each thread should use its own instance.

namespace mpp {

// Initialize the MPP decoder (loads librockchip_mpp.so via dlopen).
// Must be called once before using decode(). Safe to call multiple times.
// Returns true if MPP is available and initialized successfully.
bool initDecoder();

// Release MPP decoder resources. Safe to call even if init failed.
void deinitDecoder();

// Decode a JPEG frame to NV12 format using MPP hardware.
//
//   jpegData / jpegSize : input JPEG buffer
//   outNv12             : caller-allocated output buffer
//   outNv12Size         : size of outNv12 in bytes (≥ width * height * 3/2)
//   width / height      : decoded frame dimensions (set on success)
//
// Returns true on success, false on error (unsupported format, buffer too small, etc.).
// On failure, outNv12 contents are undefined.
bool decode(const uint8_t *jpegData, size_t jpegSize,
            uint8_t *outNv12, size_t outNv12Size, int &width, int &height);

// Convert NV12 to RGB32 using integer-only arithmetic (CPU fallback).
// Used when RGA is not available for the NV12→RGB32 step.
//
//   nv12Data            : NV12 frame buffer
//   width / height      : frame dimensions
//   outRgb32            : caller-allocated output buffer (width * height * 4 bytes)
void nv12ToRgb32(const uint8_t *nv12Data, int width, int height,
                 uint8_t *outRgb32);

} // namespace mpp

#endif // MPP_DECODER_H
