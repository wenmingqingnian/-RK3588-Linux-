#include "mpp_decoder.h"
#include "mpp_abi_constants.h"
#include "logmanager.h"

#include <dlfcn.h>
#include <cstring>
#include <new>
#include <thread>
#include <chrono>

static const char *MODULE = "mpp_dec";

// ---------------------------------------------------------------------------
// MPP API type definitions
//
// Common types (MppCtx, MppApi, pfn_mpp_create, ...) are shared with
// mpp_encoder.cpp via mpp_api_types.h to keep the ABI layout in sync.
//
// The correct ABI (verified on the RK3588S board) is:
//   mpp_create(MppCtx *ctx, MppApi **api)
//   mpp_init(MppCtx ctx, MppCtxType ctxType, MppCodingType codingType)
// followed by decode calls through the MppApi function table:
//   mpi->decode_put_packet(ctx, packet)
//   mpi->decode_get_frame(ctx, &frame)
// ---------------------------------------------------------------------------

#include "mpp_api_types.h"

// Decoder-specific MPP constants.
#define MPP_CTX_DEC mpp::abi::kContextDecoder
#define MPP_CODING_MJPEG mpp::abi::kCodingMjpeg
#define MPP_DEC_SET_FRAME_INFO mpp::abi::kDecoderSetFrameInfo

// ---------------------------------------------------------------------------
// Decoder-specific function pointer types
// ---------------------------------------------------------------------------

typedef MppRet (*pfn_mpp_packet_init)(MppPacket *packet, const void *data, size_t size);
typedef MppRet (*pfn_mpp_packet_deinit)(MppPacket *packet);
typedef MppRet (*pfn_mpp_frame_init)(MppFrame *frame);
typedef MppRet (*pfn_mpp_frame_deinit)(MppFrame *frame);
typedef uint32_t (*pfn_mpp_frame_get_width)(MppFrame frame);
typedef uint32_t (*pfn_mpp_frame_get_height)(MppFrame frame);
typedef uint32_t (*pfn_mpp_frame_get_hor_stride)(MppFrame frame);
typedef uint32_t (*pfn_mpp_frame_get_ver_stride)(MppFrame frame);
typedef MppFrameFormat (*pfn_mpp_frame_get_fmt)(MppFrame frame);
typedef int (*pfn_mpp_frame_get_errinfo)(MppFrame frame);
typedef int (*pfn_mpp_frame_get_discard)(MppFrame frame);
typedef MppBuffer (*pfn_mpp_frame_get_buffer)(MppFrame frame);
typedef void *(*pfn_mpp_buffer_get_ptr_with_caller)(MppBuffer buffer, const char *caller);
typedef MppRet (*pfn_mpp_buffer_sync_begin_f)(MppBuffer buffer, int ro, const char *caller);
typedef MppRet (*pfn_mpp_buffer_sync_end_f)(MppBuffer buffer, int ro, const char *caller);

// ---------------------------------------------------------------------------
// Static function pointers and state
// ---------------------------------------------------------------------------

static void *s_libHandle = nullptr;
static MppCtx s_ctx = nullptr;
static MppApi *s_mpi = nullptr;
static bool s_decoderReady = false;

// NV12 output buffer allocated once when the frame size is first known.
static uint8_t *s_nv12Buffer = nullptr;
static int s_nv12BufSize = 0;

static pfn_mpp_create               s_mpp_create = nullptr;
static pfn_mpp_init                 s_mpp_init = nullptr;
static pfn_mpp_destroy              s_mpp_destroy = nullptr;
static pfn_mpp_packet_init          s_mpp_packet_init = nullptr;
static pfn_mpp_packet_deinit        s_mpp_packet_deinit = nullptr;
static pfn_mpp_frame_init           s_mpp_frame_init = nullptr;
static pfn_mpp_frame_deinit         s_mpp_frame_deinit = nullptr;
static pfn_mpp_frame_get_width      s_mpp_frame_get_width = nullptr;
static pfn_mpp_frame_get_height     s_mpp_frame_get_height = nullptr;
static pfn_mpp_frame_get_hor_stride s_mpp_frame_get_hor_stride = nullptr;
static pfn_mpp_frame_get_ver_stride s_mpp_frame_get_ver_stride = nullptr;
static pfn_mpp_frame_get_fmt        s_mpp_frame_get_fmt = nullptr;
static pfn_mpp_frame_get_errinfo    s_mpp_frame_get_errinfo = nullptr;
static pfn_mpp_frame_get_discard    s_mpp_frame_get_discard = nullptr;
static pfn_mpp_frame_get_buffer     s_mpp_frame_get_buffer = nullptr;
static pfn_mpp_buffer_get_ptr_with_caller s_mpp_buffer_get_ptr_with_caller = nullptr;
static pfn_mpp_buffer_sync_begin_f  s_mpp_buffer_sync_begin = nullptr;
static pfn_mpp_buffer_sync_end_f    s_mpp_buffer_sync_end = nullptr;

// ---------------------------------------------------------------------------
// Internal: ensure NV12 buffer is large enough
// ---------------------------------------------------------------------------

static bool ensureNv12Buffer(int width, int height)
{
    if (width <= 0 || height <= 0) {
        VLOG_ERROR(MODULE, "invalid NV12 dimensions: %dx%d", width, height);
        return false;
    }
    const long long size64 = static_cast<long long>(width) * height * 3 / 2;
    if (size64 > 0x7FFFFFFFLL) {
        VLOG_ERROR(MODULE, "NV12 buffer size overflow: %dx%d", width, height);
        return false;
    }
    const int size = static_cast<int>(size64);
    if (s_nv12Buffer && s_nv12BufSize >= size)
        return true;

    delete[] s_nv12Buffer;
    s_nv12Buffer = new(std::nothrow) uint8_t[size];
    if (!s_nv12Buffer) {
        VLOG_ERROR(MODULE, "failed to allocate NV12 buffer (%d bytes)", size);
        s_nv12BufSize = 0;
        return false;
    }
    s_nv12BufSize = size;
    return true;
}

// ---------------------------------------------------------------------------
// Internal: load MPP library and resolve symbols
// ---------------------------------------------------------------------------

static bool loadMppLib()
{
    if (s_libHandle != nullptr)
        return true;

    // Try already-loaded first (encoder may have loaded it).
    s_libHandle = dlopen("librockchip_mpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!s_libHandle)
        s_libHandle = dlopen("librockchip_mpp.so", RTLD_LAZY);

    if (!s_libHandle) {
        VLOG_WARN(MODULE, "dlopen(librockchip_mpp.so) failed: %s", dlerror());
        return false;
    }

    #define RESOLVE_SYM(var, name) \
        var = reinterpret_cast<decltype(var)>(dlsym(s_libHandle, name)); \
        if (!var) VLOG_WARN(MODULE, "dlsym(%s) failed: %s", name, dlerror())

    RESOLVE_SYM(s_mpp_create, "mpp_create");
    RESOLVE_SYM(s_mpp_init, "mpp_init");
    RESOLVE_SYM(s_mpp_destroy, "mpp_destroy");
    RESOLVE_SYM(s_mpp_packet_init, "mpp_packet_init");
    RESOLVE_SYM(s_mpp_packet_deinit, "mpp_packet_deinit");
    RESOLVE_SYM(s_mpp_frame_init, "mpp_frame_init");
    RESOLVE_SYM(s_mpp_frame_deinit, "mpp_frame_deinit");
    RESOLVE_SYM(s_mpp_frame_get_width, "mpp_frame_get_width");
    RESOLVE_SYM(s_mpp_frame_get_height, "mpp_frame_get_height");
    RESOLVE_SYM(s_mpp_frame_get_hor_stride, "mpp_frame_get_hor_stride");
    RESOLVE_SYM(s_mpp_frame_get_ver_stride, "mpp_frame_get_ver_stride");
    RESOLVE_SYM(s_mpp_frame_get_fmt, "mpp_frame_get_fmt");
    RESOLVE_SYM(s_mpp_frame_get_errinfo, "mpp_frame_get_errinfo");
    RESOLVE_SYM(s_mpp_frame_get_discard, "mpp_frame_get_discard");
    RESOLVE_SYM(s_mpp_frame_get_buffer, "mpp_frame_get_buffer");
    RESOLVE_SYM(s_mpp_buffer_get_ptr_with_caller, "mpp_buffer_get_ptr_with_caller");
    RESOLVE_SYM(s_mpp_buffer_sync_begin, "mpp_buffer_sync_begin_f");
    RESOLVE_SYM(s_mpp_buffer_sync_end, "mpp_buffer_sync_end_f");

    #undef RESOLVE_SYM

    if (!s_mpp_buffer_sync_begin || !s_mpp_buffer_sync_end) {
        VLOG_WARN(MODULE, "%s",
                  "MPP buffer sync symbols unavailable; continuing without "
                  "explicit decoder cache sync");
        s_mpp_buffer_sync_begin = nullptr;
        s_mpp_buffer_sync_end = nullptr;
    }

    // Verify all required symbols. mpp_create/mpp_init must use the correct
    // ABI (MppApi** output + 3-arg init); the resolved function pointer types
    // above enforce that at compile time.
    if (!s_mpp_create || !s_mpp_init || !s_mpp_destroy ||
        !s_mpp_packet_init || !s_mpp_packet_deinit ||
        !s_mpp_frame_init || !s_mpp_frame_deinit ||
        !s_mpp_frame_get_width || !s_mpp_frame_get_height ||
        !s_mpp_frame_get_hor_stride || !s_mpp_frame_get_ver_stride ||
        !s_mpp_frame_get_fmt || !s_mpp_frame_get_errinfo ||
        !s_mpp_frame_get_discard || !s_mpp_frame_get_buffer ||
        !s_mpp_buffer_get_ptr_with_caller) {
        VLOG_ERROR(MODULE, "%s",
                   "missing required MPP decoder symbols in librockchip_mpp.so; "
                   "MJPEG decoder disabled");
        dlclose(s_libHandle);
        s_libHandle = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool mpp::initDecoder()
{
    if (s_decoderReady)
        return true;

    if (!loadMppLib())
        return false;

    // Step 1: create MPP context and retrieve the MppApi function table.
    // The real mpp_create writes the MppApi* into s_mpi; the previous code
    // passed MPP_CODING_MJPEG as the second argument, which is an ABI error.
    MppRet ret = s_mpp_create(&s_ctx, &s_mpi);
    if (ret != MPP_OK || !s_ctx || !s_mpi) {
        VLOG_ERROR(MODULE, "mpp_create failed: %d (MJPEG decoder disabled)", ret);
        s_ctx = nullptr;
        s_mpi = nullptr;
        return false;
    }

    // Step 2: verify the MppApi table exposes the decoder operations before
    // using them. A table missing decode_put_packet/decode_get_frame cannot
    // drive MJPEG decoding and must soft-fail.
    if (!s_mpi->decode_put_packet || !s_mpi->decode_get_frame) {
        VLOG_ERROR(MODULE, "%s",
                   "MppApi table missing decode_put_packet/decode_get_frame; "
                   "MJPEG decoder disabled");
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
        return false;
    }

    // Step 3: initialize the decoder context (MPP_CTX_DEC=0, MJPEG=5).
    ret = s_mpp_init(s_ctx, MPP_CTX_DEC, MPP_CODING_MJPEG);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "mpp_init(DEC, MJPEG) failed: %d (decoder disabled)", ret);
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
        return false;
    }

    s_decoderReady = true;
    VLOG_INFO(MODULE, "%s", "MPP MJPEG decoder initialized (MppApi ABI)");
    return true;
}

void mpp::deinitDecoder()
{
    if (s_ctx) {
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
    }

    delete[] s_nv12Buffer;
    s_nv12Buffer = nullptr;
    s_nv12BufSize = 0;

    // Do not close libHandle — encoder may still be using it.
    s_decoderReady = false;
    VLOG_INFO(MODULE, "%s", "MPP MJPEG decoder released");
}

bool mpp::decode(const uint8_t *jpegData, size_t jpegSize,
                 uint8_t *outNv12, size_t outNv12Size, int &width, int &height)
{
    width = 0;
    height = 0;
    if (!s_decoderReady || !jpegData || jpegSize == 0)
        return false;

    // Build MPP packet from JPEG data.
    MppPacket packet = nullptr;
    MppRet ret = s_mpp_packet_init(&packet, jpegData, jpegSize);
    if (ret != MPP_OK || !packet) {
        VLOG_ERROR(MODULE, "mpp_packet_init failed: %d", ret);
        return false;
    }

    // Submit the JPEG packet through the MppApi function table.
    ret = s_mpi->decode_put_packet(s_ctx, packet);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "decode_put_packet failed: %d", ret);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    // Retrieve the decoded frame. MPP may need a short retry loop to produce
    // the frame (same pattern as the encoder's encode_get_packet polling).
    MppFrame frame = nullptr;
    for (int retry = 0; retry < 50; ++retry) {
        ret = s_mpi->decode_get_frame(s_ctx, &frame);
        if (ret == MPP_OK && frame)
            break;
        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (ret != MPP_OK || !frame) {
        VLOG_ERROR(MODULE, "decode_get_frame returned no frame: %d", ret);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    // Check for decode errors.
    if (s_mpp_frame_get_errinfo(frame) || s_mpp_frame_get_discard(frame)) {
        VLOG_ERROR(MODULE, "%s", "MPP decode error or discarded frame");
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    // Get frame properties.
    const uint32_t fw = s_mpp_frame_get_width(frame);
    const uint32_t fh = s_mpp_frame_get_height(frame);
    const uint32_t hstride = s_mpp_frame_get_hor_stride(frame);
    const uint32_t vstride = s_mpp_frame_get_ver_stride(frame);
    const MppFrameFormat fmt = s_mpp_frame_get_fmt(frame);

    // Verify NV12/I420 output.
    if (fmt != MPP_FMT_YUV420SP && fmt != MPP_FMT_YUV420P) {
        VLOG_ERROR(MODULE, "unexpected MPP output format: %d (expected NV12/NV21)", fmt);
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    width = static_cast<int>(fw);
    height = static_cast<int>(fh);

    // Ensure output buffer is large enough.
    if (!ensureNv12Buffer(width, height)) {
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    if (fw == 0 || fh == 0 || hstride < fw || vstride < fh ||
        (fw % 2) != 0 || (fh % 2) != 0) {
        VLOG_ERROR(MODULE, "invalid MPP frame layout: %ux%u stride %ux%u",
                   fw, fh, hstride, vstride);
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    // Copy decoded frame data to the caller's NV12 buffer.
    MppBuffer mppBuf = s_mpp_frame_get_buffer(frame);
    if (!mppBuf) {
        VLOG_ERROR(MODULE, "%s", "mpp_frame_get_buffer returned null");
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    void *mppPtr = s_mpp_buffer_get_ptr_with_caller(mppBuf, "decode");
    if (!mppPtr) {
        VLOG_ERROR(MODULE, "%s",
                   "mpp_buffer_get_ptr_with_caller returned null");
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        return false;
    }

    bool syncStarted = false;
    if (s_mpp_buffer_sync_begin) {
        ret = s_mpp_buffer_sync_begin(mppBuf, 1, "decode");
        if (ret != MPP_OK) {
            VLOG_ERROR(MODULE, "mpp_buffer_sync_begin failed: %d", ret);
            s_mpp_frame_deinit(&frame);
            s_mpp_packet_deinit(&packet);
            return false;
        }
        syncStarted = true;
    }

    // Validate caller-provided buffer against actual frame dimensions.
    const size_t requiredNv12Size = static_cast<size_t>(fw) * fh * 3 / 2;
    if (outNv12 && outNv12Size < requiredNv12Size) {
        VLOG_ERROR(MODULE,
                   "caller NV12 buffer too small: %llux%llu (need %llu, got %llu)",
                   static_cast<unsigned long long>(fw),
                   static_cast<unsigned long long>(fh),
                   static_cast<unsigned long long>(requiredNv12Size),
                   static_cast<unsigned long long>(outNv12Size));
        if (syncStarted && s_mpp_buffer_sync_end) {
            ret = s_mpp_buffer_sync_end(mppBuf, 1, "decode");
            if (ret != MPP_OK) {
                VLOG_WARN(MODULE, "mpp_buffer_sync_end failed: %d", ret);
            }
        }
        s_mpp_frame_deinit(&frame);
        s_mpp_packet_deinit(&packet);
        width = 0;
        height = 0;
        return false;
    }

    const uint8_t *src = static_cast<const uint8_t *>(mppPtr);
    uint8_t *dst = outNv12 ? outNv12 : s_nv12Buffer;

    // Copy Y plane (with stride → width padding removal).
    for (uint32_t row = 0; row < fh; row++) {
        memcpy(dst + row * fw, src + row * hstride, fw);
    }

    uint8_t *dstUV = dst + fw * fh;
    if (fmt == MPP_FMT_YUV420SP) {
        const uint8_t *srcUV = src + hstride * vstride;
        for (uint32_t row = 0; row < fh / 2; row++) {
            memcpy(dstUV + row * fw, srcUV + row * hstride, fw);
        }
    } else {
        const uint32_t chromaStride = hstride / 2;
        const uint32_t chromaHeight = vstride / 2;
        const uint8_t *srcU = src + hstride * vstride;
        const uint8_t *srcV = srcU + chromaStride * chromaHeight;
        for (uint32_t row = 0; row < fh / 2; row++) {
            for (uint32_t col = 0; col < fw / 2; col++) {
                dstUV[row * fw + col * 2] = srcU[row * chromaStride + col];
                dstUV[row * fw + col * 2 + 1] = srcV[row * chromaStride + col];
            }
        }
    }

    if (syncStarted && s_mpp_buffer_sync_end) {
        ret = s_mpp_buffer_sync_end(mppBuf, 1, "decode");
        if (ret != MPP_OK) {
            VLOG_WARN(MODULE, "mpp_buffer_sync_end failed: %d", ret);
        }
    }

    s_mpp_frame_deinit(&frame);
    s_mpp_packet_deinit(&packet);

    VLOG_DEBUG(MODULE, "MPP decode: %dx%d (stride %dx%d)", fw, fh, hstride, vstride);
    return true;
}

void mpp::nv12ToRgb32(const uint8_t *nv12Data, int width, int height,
                      uint8_t *outRgb32)
{
    if (!nv12Data || !outRgb32 || width <= 0 || height <= 0)
        return;

    const size_t yPlaneSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const uint8_t *yPlane = nv12Data;
    const uint8_t *uvPlane = nv12Data + yPlaneSize;
    uint8_t *dst = outRgb32;

    for (int row = 0; row < height; row++) {
        const uint8_t *yRow = yPlane + static_cast<size_t>(row) * width;
        const uint8_t *uvRow = uvPlane + static_cast<size_t>(row / 2) * width;

        for (int col = 0; col < width; col++) {
            int Y = yRow[col];
            int U = uvRow[(col / 2) * 2] - 128;
            int V = uvRow[(col / 2) * 2 + 1] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            // Clamp to [0, 255].
            R = (R < 0) ? 0 : (R > 255) ? 255 : R;
            G = (G < 0) ? 0 : (G > 255) ? 255 : G;
            B = (B < 0) ? 0 : (B > 255) ? 255 : B;

            // ARGB32 pixel (little-endian: B, G, R, A).
            *dst++ = static_cast<uint8_t>(B);
            *dst++ = static_cast<uint8_t>(G);
            *dst++ = static_cast<uint8_t>(R);
            *dst++ = 0xFF;
        }
    }
}
