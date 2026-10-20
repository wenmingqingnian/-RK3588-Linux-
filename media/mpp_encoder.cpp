#include "mpp_encoder.h"
#include "mpp_abi_constants.h"
#include "mpp_frame_layout.h"
#include "logmanager.h"

#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>
#include <chrono>

static const char *MODULE = "mpp_enc";

// ---------------------------------------------------------------------------
// MPP API type definitions (from rockchip/mpp headers)
//
// Common types (MppCtx, MppApi, pfn_mpp_create, ...) are shared with
// mpp_decoder.cpp via mpp_api_types.h to keep the ABI layout in sync.
// ---------------------------------------------------------------------------

#include "mpp_api_types.h"

// Encoder-specific typedef.
typedef void *MppEncCfg;

// MPP context types.
#define MPP_CTX_ENC mpp::abi::kContextEncoder

// MPP coding types.
#define MPP_VIDEO_CodingAVC  mpp::abi::kCodingAvc
#define MPP_VIDEO_CodingHEVC mpp::abi::kCodingHevc

// MPP encoder RC modes.
#define MPP_ENC_RC_MODE_VBR mpp::abi::kRateControlVbr

// MPP encoder control commands (from rk_mpi_cmd.h, verified on board).
// Base = CMD_MODULE_CODEC(0x00300000) | CMD_CTX_ID_ENC(0x00020000) = 0x00320000
#define MPP_ENC_SET_CFG            mpp::abi::kEncoderSetConfig
#define MPP_ENC_GET_CFG            mpp::abi::kEncoderGetConfig
#define MPP_ENC_GET_EXTRA_INFO     mpp::abi::kEncoderGetExtraInfo
#define MPP_ENC_SET_IDR_FRAME_REQ  mpp::abi::kEncoderSetIdrFrameReq

// ---------------------------------------------------------------------------
// Encoder-specific function pointer types
// ---------------------------------------------------------------------------

typedef MppRet (*pfn_mpp_enc_cfg_init)(MppEncCfg *cfg);
typedef MppRet (*pfn_mpp_enc_cfg_deinit)(MppEncCfg cfg);
typedef MppRet (*pfn_mpp_enc_cfg_set_s32)(MppEncCfg cfg, const char *name, int val);
// Buffer group management — from mpp_buffer.h:
// mpp_buffer_group_get(group, type, mode, tag, caller) — 5 params
// mpp_buffer_get_with_tag(group, buffer, size, tag, caller) — 5 params (with group!)
// mpp_buffer_get_ptr_with_caller(buffer, caller) — 2 params
// mpp_buffer_put_with_caller(buffer, caller) — 2 params
typedef void *MppBufferGroup;
typedef MppRet (*pfn_mpp_buffer_group_get)(MppBufferGroup *group, int type,
                                           int mode, const char *tag,
                                           const char *caller);
typedef MppRet (*pfn_mpp_buffer_group_put)(MppBufferGroup group);
typedef MppRet (*pfn_mpp_buffer_get_with_tag)(MppBufferGroup group, MppBuffer *buffer,
                                              size_t size, const char *tag,
                                              const char *caller);
typedef void *(*pfn_mpp_buffer_get_ptr_with_caller)(MppBuffer buffer, const char *caller);
typedef MppRet (*pfn_mpp_buffer_put_with_caller)(MppBuffer buffer, const char *caller);
typedef MppRet (*pfn_mpp_buffer_sync_begin_f)(MppBuffer buffer, int ro, const char *caller);
typedef MppRet (*pfn_mpp_buffer_sync_end_f)(MppBuffer buffer, int ro, const char *caller);
typedef MppRet (*pfn_mpp_frame_init)(MppFrame *frame);
typedef MppRet (*pfn_mpp_frame_deinit)(MppFrame *frame);
typedef void (*pfn_mpp_frame_set_width)(MppFrame frame, uint32_t width);
typedef void (*pfn_mpp_frame_set_height)(MppFrame frame, uint32_t height);
typedef void (*pfn_mpp_frame_set_hor_stride)(MppFrame frame, uint32_t stride);
typedef void (*pfn_mpp_frame_set_ver_stride)(MppFrame frame, uint32_t stride);
typedef void (*pfn_mpp_frame_set_fmt)(MppFrame frame, MppFrameFormat fmt);
typedef void (*pfn_mpp_frame_set_buffer)(MppFrame frame, MppBuffer buffer);
typedef void (*pfn_mpp_frame_set_eos)(MppFrame frame, int eos);
typedef void *(*pfn_mpp_packet_get_pos)(MppPacket packet);
typedef size_t (*pfn_mpp_packet_get_length)(MppPacket packet);
typedef MppRet (*pfn_mpp_packet_deinit)(MppPacket *packet);

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static void *s_libHandle = nullptr;
static MppCtx s_ctx = nullptr;
static MppApi *s_mpi = nullptr;
static MppEncCfg s_encCfg = nullptr;
static MppBufferGroup s_frmGrp = nullptr;
static bool s_encoderReady = false;
static bool s_libLoaded = false;
static bool s_isH265 = false;

// IDR request flag: set by forceIDR() from any thread, consumed by encode()
// on the encoder thread. Atomic so no lock is needed; the lazy-trigger
// pattern keeps MPP API access single-threaded.
static std::atomic<bool> s_forceIDRRequest{false};

// Encoder dimensions (used by encode() to compute buffer sizes).
// hor_stride/ver_stride are 16-byte aligned for MPP VPU hardware access.
static uint32_t s_width = 0;
static uint32_t s_height = 0;
static uint32_t s_horStride = 0;
static uint32_t s_verStride = 0;
static mpp::frame_layout::Layout s_frameLayout;

// Resolved function pointers.
static pfn_mpp_create                    s_mpp_create = nullptr;
static pfn_mpp_init                      s_mpp_init = nullptr;
static pfn_mpp_destroy                   s_mpp_destroy = nullptr;
static pfn_mpp_enc_cfg_init              s_mpp_enc_cfg_init = nullptr;
static pfn_mpp_enc_cfg_deinit            s_mpp_enc_cfg_deinit = nullptr;
static pfn_mpp_enc_cfg_set_s32           s_mpp_enc_cfg_set_s32 = nullptr;
static pfn_mpp_buffer_group_get          s_mpp_buffer_group_get = nullptr;
static pfn_mpp_buffer_group_put          s_mpp_buffer_group_put = nullptr;
static pfn_mpp_buffer_get_with_tag       s_mpp_buffer_get_with_tag = nullptr;
static pfn_mpp_buffer_get_ptr_with_caller s_mpp_buffer_get_ptr_with_caller = nullptr;
static pfn_mpp_buffer_put_with_caller    s_mpp_buffer_put_with_caller = nullptr;
static pfn_mpp_buffer_sync_begin_f       s_mpp_buffer_sync_begin = nullptr;
static pfn_mpp_buffer_sync_end_f         s_mpp_buffer_sync_end = nullptr;
static pfn_mpp_frame_init                s_mpp_frame_init = nullptr;
static pfn_mpp_frame_deinit              s_mpp_frame_deinit = nullptr;
static pfn_mpp_frame_set_width           s_mpp_frame_set_width = nullptr;
static pfn_mpp_frame_set_height          s_mpp_frame_set_height = nullptr;
static pfn_mpp_frame_set_hor_stride      s_mpp_frame_set_hor_stride = nullptr;
static pfn_mpp_frame_set_ver_stride      s_mpp_frame_set_ver_stride = nullptr;
static pfn_mpp_frame_set_fmt             s_mpp_frame_set_fmt = nullptr;
static pfn_mpp_frame_set_buffer          s_mpp_frame_set_buffer = nullptr;
static pfn_mpp_frame_set_eos             s_mpp_frame_set_eos = nullptr;
static pfn_mpp_packet_get_pos            s_mpp_packet_get_pos = nullptr;
static pfn_mpp_packet_get_length         s_mpp_packet_get_length = nullptr;
static pfn_mpp_packet_deinit             s_mpp_packet_deinit = nullptr;

// ---------------------------------------------------------------------------
// Internal: load MPP library and resolve symbols
// ---------------------------------------------------------------------------

static bool loadMppLib()
{
    if (s_libLoaded)
        return true;

    // Try already-loaded first (decoder may have loaded it).
    s_libHandle = dlopen("librockchip_mpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!s_libHandle)
        s_libHandle = dlopen("librockchip_mpp.so", RTLD_LAZY);

    if (!s_libHandle) {
        VLOG_WARN(MODULE, "dlopen(librockchip_mpp.so) failed: %s", dlerror());
        return false;
    }

    // Resolve global API functions with per-symbol error logging.
    #define RESOLVE_SYM(var, name) \
        var = reinterpret_cast<decltype(var)>(dlsym(s_libHandle, name)); \
        if (!var) VLOG_WARN(MODULE, "dlsym(%s) failed: %s", name, dlerror())

    RESOLVE_SYM(s_mpp_create, "mpp_create");
    RESOLVE_SYM(s_mpp_init, "mpp_init");
    RESOLVE_SYM(s_mpp_destroy, "mpp_destroy");
    RESOLVE_SYM(s_mpp_enc_cfg_init, "mpp_enc_cfg_init");
    RESOLVE_SYM(s_mpp_enc_cfg_deinit, "mpp_enc_cfg_deinit");
    RESOLVE_SYM(s_mpp_enc_cfg_set_s32, "mpp_enc_cfg_set_s32");
    RESOLVE_SYM(s_mpp_buffer_group_get, "mpp_buffer_group_get");
    RESOLVE_SYM(s_mpp_buffer_group_put, "mpp_buffer_group_put");
    RESOLVE_SYM(s_mpp_buffer_get_with_tag, "mpp_buffer_get_with_tag");
    RESOLVE_SYM(s_mpp_buffer_get_ptr_with_caller, "mpp_buffer_get_ptr_with_caller");
    RESOLVE_SYM(s_mpp_buffer_put_with_caller, "mpp_buffer_put_with_caller");
    RESOLVE_SYM(s_mpp_buffer_sync_begin, "mpp_buffer_sync_begin_f");
    RESOLVE_SYM(s_mpp_buffer_sync_end, "mpp_buffer_sync_end_f");
    RESOLVE_SYM(s_mpp_frame_init, "mpp_frame_init");
    RESOLVE_SYM(s_mpp_frame_deinit, "mpp_frame_deinit");
    RESOLVE_SYM(s_mpp_frame_set_width, "mpp_frame_set_width");
    RESOLVE_SYM(s_mpp_frame_set_height, "mpp_frame_set_height");
    RESOLVE_SYM(s_mpp_frame_set_hor_stride, "mpp_frame_set_hor_stride");
    RESOLVE_SYM(s_mpp_frame_set_ver_stride, "mpp_frame_set_ver_stride");
    RESOLVE_SYM(s_mpp_frame_set_fmt, "mpp_frame_set_fmt");
    RESOLVE_SYM(s_mpp_frame_set_buffer, "mpp_frame_set_buffer");
    RESOLVE_SYM(s_mpp_frame_set_eos, "mpp_frame_set_eos");  // Optional
    RESOLVE_SYM(s_mpp_packet_get_pos, "mpp_packet_get_pos");
    RESOLVE_SYM(s_mpp_packet_get_length, "mpp_packet_get_length");
    RESOLVE_SYM(s_mpp_packet_deinit, "mpp_packet_deinit");

    #undef RESOLVE_SYM

    if (!s_mpp_buffer_sync_begin || !s_mpp_buffer_sync_end) {
        VLOG_WARN(MODULE, "%s",
                  "MPP cache sync API incomplete; continuing without explicit sync");
        s_mpp_buffer_sync_begin = nullptr;
        s_mpp_buffer_sync_end = nullptr;
    }

    // Verify all required symbols (mpp_frame_set_eos is optional).
    if (!s_mpp_create || !s_mpp_init || !s_mpp_destroy ||
        !s_mpp_enc_cfg_init || !s_mpp_enc_cfg_deinit || !s_mpp_enc_cfg_set_s32 ||
        !s_mpp_buffer_group_get || !s_mpp_buffer_group_put ||
        !s_mpp_buffer_get_with_tag ||
        !s_mpp_buffer_get_ptr_with_caller || !s_mpp_buffer_put_with_caller ||
        !s_mpp_frame_init || !s_mpp_frame_deinit ||
        !s_mpp_frame_set_width || !s_mpp_frame_set_height ||
        !s_mpp_frame_set_hor_stride || !s_mpp_frame_set_ver_stride ||
        !s_mpp_frame_set_fmt || !s_mpp_frame_set_buffer ||
        !s_mpp_packet_get_pos || !s_mpp_packet_get_length ||
        !s_mpp_packet_deinit) {
        VLOG_ERROR(MODULE, "%s",
                   "missing required MPP encoder symbols in librockchip_mpp.so");
        dlclose(s_libHandle);
        s_libHandle = nullptr;
        return false;
    }
    s_libLoaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool mpp::isAvailable()
{
    if (!s_libLoaded)
        loadMppLib();
    return s_libLoaded;
}

bool mpp::isH265()
{
    return s_encoderReady && s_isH265;
}

bool mpp::forceIDR()
{
    if (!s_encoderReady) {
        return false;
    }
    // Lazy-trigger: just set the flag. The actual MPP control call happens
    // inside encode() on the encoder thread, so the MPI interface is never
    // accessed concurrently from two threads.
    s_forceIDRRequest.store(true, std::memory_order_release);
    return true;
}

// Consume the IDR request flag. Called by encode() right before pushing the
// frame to the encoder. Returns true if an IDR should be forced for this
// frame, false otherwise.
static bool consumeIdrRequest()
{
    // exchange() is a single atomic RMW; relaxed is sufficient because the
    // encoder is the only consumer and the only synchronization needed is
    // the flag itself (no dependent memory).
    return s_forceIDRRequest.exchange(false, std::memory_order_acq_rel);
}

static bool setEncoderConfigValue(MppEncCfg cfg, const char *key, int value)
{
    const MppRet ret = s_mpp_enc_cfg_set_s32(cfg, key, value);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "mpp_enc_cfg_set_s32(%s=%d) failed: %d",
                   key, value, ret);
        return false;
    }
    return true;
}

bool mpp::initEncoder(const EncoderConfig &config)
{
    if (s_encoderReady)
        return true;

    const frame_layout::Layout layout = frame_layout::Layout::forI420(
        static_cast<std::size_t>(config.width),
        static_cast<std::size_t>(config.height), 16);
    if (!layout.valid() || config.fps <= 0 || config.bitrate <= 0 ||
        config.bitrate > std::numeric_limits<int>::max() / 2) {
        VLOG_ERROR(MODULE,
                   "invalid encoder config: %dx%d@%d, bitrate=%d",
                   config.width, config.height, config.fps, config.bitrate);
        return false;
    }

    if (!loadMppLib())
        return false;

    // Determine MPP coding type.
    MppCodingType codingType;
    if (config.codec == CodecType::H265)
        codingType = MPP_VIDEO_CodingHEVC;
    else
        codingType = MPP_VIDEO_CodingAVC;

    // Step 1: Create MPP context and MPI interface (project1 flow).
    MppRet ret = s_mpp_create(&s_ctx, &s_mpi);
    if (ret != MPP_OK || !s_ctx || !s_mpi) {
        VLOG_ERROR(MODULE, "mpp_create failed: %d", ret);
        return false;
    }
    if (!s_mpi->control || !s_mpi->encode_put_frame ||
        !s_mpi->encode_get_packet) {
        VLOG_ERROR(MODULE, "%s",
                   "MPP API table is missing encoder operations");
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
        return false;
    }

    // Step 2: Initialize encoder context (MPP_CTX_ENC = 1).
    ret = s_mpp_init(s_ctx, MPP_CTX_ENC, codingType);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "mpp_init(ENC, %d) failed: %d", codingType, ret);
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
        return false;
    }

    // Step 3: Create and configure encoder parameters.
    ret = s_mpp_enc_cfg_init(&s_encCfg);
    if (ret != MPP_OK || !s_encCfg) {
        VLOG_ERROR(MODULE, "mpp_enc_cfg_init failed: %d", ret);
        goto FAIL;
    }

    // Get default config from encoder.
    ret = s_mpi->control(s_ctx, MPP_ENC_GET_CFG, s_encCfg);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "MPP_ENC_GET_CFG failed: %d", ret);
        goto FAIL;
    }

    // Step 3a/3b: image and rate-control parameters.
    if (!(setEncoderConfigValue(s_encCfg, "prep:width", config.width) &&
        setEncoderConfigValue(s_encCfg, "prep:height", config.height) &&
        setEncoderConfigValue(
            s_encCfg, "prep:hor_stride",
            static_cast<int>(layout.horizontalStride)) &&
        setEncoderConfigValue(
            s_encCfg, "prep:ver_stride",
            static_cast<int>(layout.verticalStride)) &&
        setEncoderConfigValue(s_encCfg, "prep:format", MPP_FMT_YUV420P) &&
        setEncoderConfigValue(s_encCfg, "rc:mode", MPP_ENC_RC_MODE_VBR) &&
        setEncoderConfigValue(s_encCfg, "rc:bps_target", config.bitrate) &&
        setEncoderConfigValue(s_encCfg, "rc:bps_max", config.bitrate * 2) &&
        setEncoderConfigValue(s_encCfg, "rc:bps_min", config.bitrate / 2) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_in_flex", 0) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_in_num", config.fps) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_in_denom", 1) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_out_flex", 0) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_out_num", config.fps) &&
        setEncoderConfigValue(s_encCfg, "rc:fps_out_denom", 1) &&
        setEncoderConfigValue(
            s_encCfg, "rc:gop",
            config.gop > 0 ? config.gop : config.fps))) {
        goto FAIL;
    }

    // Step 3c: Codec-specific parameters.
    if (codingType == MPP_VIDEO_CodingAVC) {
        const bool codecConfigSet =
            setEncoderConfigValue(
                s_encCfg, "codec:type", MPP_VIDEO_CodingAVC) &&
            setEncoderConfigValue(s_encCfg, "h264:profile", 66) &&
            setEncoderConfigValue(s_encCfg, "h264:level", 40) &&
            setEncoderConfigValue(s_encCfg, "h264:cabac_en", 0) &&
            setEncoderConfigValue(s_encCfg, "h264:qp_init", 26) &&
            setEncoderConfigValue(s_encCfg, "h264:qp_min", 20) &&
            setEncoderConfigValue(s_encCfg, "h264:qp_max", 35);
        if (!codecConfigSet)
            goto FAIL;
    } else {
        const bool codecConfigSet =
            setEncoderConfigValue(
                s_encCfg, "codec:type", MPP_VIDEO_CodingHEVC) &&
            setEncoderConfigValue(s_encCfg, "h265:profile", 1) &&
            setEncoderConfigValue(s_encCfg, "h265:level", 120);
        if (!codecConfigSet)
            goto FAIL;
    }

    // Step 3d: Apply configuration.
    ret = s_mpi->control(s_ctx, MPP_ENC_SET_CFG, s_encCfg);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "MPP_ENC_SET_CFG failed: %d", ret);
        goto FAIL;
    }

    // Step 4: Create ION buffer group for input frames.
    // project1 uses MPP_BUFFER_TYPE_ION — MPP internally falls back to DMA_HEAP on kernel 5.10+.
    ret = s_mpp_buffer_group_get(
        &s_frmGrp, MPP_BUFFER_TYPE_ION, abi::kBufferModeInternal,
                                 "enc_group", "initEncoder");
    if (ret != MPP_OK || !s_frmGrp) {
        VLOG_ERROR(MODULE, "buffer_group_get(ION) failed: %d", ret);
        goto FAIL;
    }

    // Record encoder state.
    s_width = config.width;
    s_height = config.height;
    s_horStride = static_cast<uint32_t>(layout.horizontalStride);
    s_verStride = static_cast<uint32_t>(layout.verticalStride);
    s_frameLayout = layout;
    s_isH265 = (codingType == MPP_VIDEO_CodingHEVC);
    s_encoderReady = true;
    VLOG_INFO(MODULE, "MPP %s encoder initialized: %dx%d@%dfps, bitrate=%d, gop=%d",
              (codingType == MPP_VIDEO_CodingHEVC) ? "H.265" : "H.264",
              config.width, config.height, config.fps, config.bitrate,
              config.gop > 0 ? config.gop : config.fps);
    return true;

FAIL:
    if (s_frmGrp) {
        s_mpp_buffer_group_put(s_frmGrp);
        s_frmGrp = nullptr;
    }
    if (s_encCfg) {
        s_mpp_enc_cfg_deinit(s_encCfg);
        s_encCfg = nullptr;
    }
    if (s_ctx) {
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
    }
    s_frameLayout = frame_layout::Layout{};
    return false;
}

void mpp::deinitEncoder()
{
    if (s_frmGrp) {
        s_mpp_buffer_group_put(s_frmGrp);
        s_frmGrp = nullptr;
    }
    if (s_encCfg) {
        s_mpp_enc_cfg_deinit(s_encCfg);
        s_encCfg = nullptr;
    }
    if (s_ctx) {
        s_mpp_destroy(s_ctx);
        s_ctx = nullptr;
        s_mpi = nullptr;
    }

    // Do not close libHandle — decoder may still be using it.
    s_encoderReady = false;
    s_isH265 = false;
    s_frameLayout = frame_layout::Layout{};
    // Clear any pending IDR request so it does not fire against a freshly
    // re-initialized encoder on the next initEncoder() call.
    s_forceIDRRequest.store(false, std::memory_order_release);
    VLOG_INFO(MODULE, "%s", "MPP encoder released");
}

bool mpp::encode(const uint8_t *yuvData, uint8_t *outEncoded,
                 size_t &outSize, size_t maxOutSize)
{
    outSize = 0;
    if (!s_encoderReady || !yuvData || !outEncoded || maxOutSize == 0 ||
        !s_frameLayout.valid()) {
        return false;
    }

    MppRet ret = MPP_NOK;
    MppFrame frame = nullptr;
    MppBuffer buffer = nullptr;
    MppPacket packet = nullptr;

    const auto releaseFrameResources = [&]() {
        if (packet)
            s_mpp_packet_deinit(&packet);
        if (buffer)
            s_mpp_buffer_put_with_caller(buffer, "encode");
        if (frame)
            s_mpp_frame_deinit(&frame);
    };

    // Step 1: initialize the frame.
    ret = s_mpp_frame_init(&frame);
    if (ret != MPP_OK || !frame) {
        VLOG_ERROR(MODULE, "mpp_frame_init failed: %d", ret);
        return false;
    }

    // Step 2: allocate an aligned DMA buffer and pack the contiguous I420 frame.
    const size_t sourceSize =
        static_cast<size_t>(s_width) * static_cast<size_t>(s_height) * 3U / 2U;
    const size_t bufferSize = s_frameLayout.bufferSize;
    ret = s_mpp_buffer_get_with_tag(
        s_frmGrp, &buffer, bufferSize, "enc_frm", "encode");
    if (ret != MPP_OK || !buffer) {
        VLOG_ERROR(MODULE, "buffer alloc failed (size=%llu): %d",
                   static_cast<unsigned long long>(bufferSize), ret);
        releaseFrameResources();
        return false;
    }

    void *bufPtr = s_mpp_buffer_get_ptr_with_caller(buffer, "encode");
    if (!bufPtr) {
        VLOG_ERROR(MODULE, "%s",
                   "mpp_buffer_get_ptr_with_caller returned null");
        releaseFrameResources();
        return false;
    }

    if (s_mpp_buffer_sync_begin) {
        ret = s_mpp_buffer_sync_begin(buffer, 0, "encode");
        if (ret != MPP_OK) {
            VLOG_ERROR(MODULE, "mpp_buffer_sync_begin failed: %d", ret);
            releaseFrameResources();
            return false;
        }
    }

    const bool copied = frame_layout::copyI420ToAligned(
        yuvData, sourceSize, static_cast<uint8_t *>(bufPtr),
        bufferSize, s_frameLayout);

    if (s_mpp_buffer_sync_end) {
        ret = s_mpp_buffer_sync_end(buffer, 0, "encode");
        if (ret != MPP_OK) {
            VLOG_ERROR(MODULE, "mpp_buffer_sync_end failed: %d", ret);
            releaseFrameResources();
            return false;
        }
    }
    if (!copied) {
        VLOG_ERROR(MODULE, "%s",
                   "failed to pack I420 frame into aligned MPP buffer");
        releaseFrameResources();
        return false;
    }

    // Step 3: Configure MppFrame.
    s_mpp_frame_set_width(frame, s_width);
    s_mpp_frame_set_height(frame, s_height);
    s_mpp_frame_set_hor_stride(frame, s_horStride);
    s_mpp_frame_set_ver_stride(frame, s_verStride);
    s_mpp_frame_set_fmt(frame, MPP_FMT_YUV420P);
    s_mpp_frame_set_buffer(frame, buffer);
    if (s_mpp_frame_set_eos)
        s_mpp_frame_set_eos(frame, 0);

    // Step 3a: Honor a pending IDR request (set by forceIDR() from another
    // thread). Must be called before encode_put_frame() so MPP knows to
    // produce an IDR for THIS frame. The control command is issued here on
    // the encoder thread, keeping MPP API access single-threaded.
    //
    // IMPORTANT (board-verified 2026-07-20): on the RK3588S board's libmpp,
    // MPP_ENC_SET_IDR_FRAME_REQ has an undocumented parameter requirement.
    // Testing showed:
    //   - nullptr      -> returns -3 (MPP_ERR_MALLOC), no crash, no IDR
    //   - uint32_t* =1 -> returns MPP_OK, but causes segfault in the next
    //                     encode_put_frame() (internal state corrupted).
    // The correct parameter type is MppEncRefCfg (a struct pointer), which
    // requires dlsym-loading mpp_enc_ref_cfg_init / _set / _deinit and
    // building a ref-cfg descriptor. Until that is implemented, the control
    // call is DISABLED to avoid the segfault. forceIDR() still works as a
    // no-op flag consumer; clients fall back to the natural GOP cycle (~1s
    // at gop=30/fps=30) for the first decodable frame.
    // TODO: implement MppEncRefCfg-based forceIDR (see rockchip
    // mpi_enc_test.c enc_test_force_idr()).
    if (consumeIdrRequest()) {
        VLOG_DEBUG(MODULE,
                   "forceIDR requested (control disabled, awaiting MppEncRefCfg impl)");
    }

    // Step 4: use the project1-proven asynchronous API. packet must remain
    // null so MPP supplies a correctly sized output packet.
    ret = s_mpi->encode_put_frame(s_ctx, frame);
    if (ret != MPP_OK) {
        VLOG_ERROR(MODULE, "encode_put_frame failed: %d", ret);
        releaseFrameResources();
        return false;
    }

    for (int retry = 0; retry < 50; ++retry) {
        ret = s_mpi->encode_get_packet(s_ctx, &packet);
        if (ret == MPP_OK && packet)
            break;
        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (ret != MPP_OK || !packet) {
        VLOG_ERROR(MODULE, "encode_get_packet returned no packet: %d", ret);
        releaseFrameResources();
        return false;
    }

    // Step 5: copy the compressed packet from its current position.
    void *pktData = s_mpp_packet_get_pos(packet);
    const size_t pktSize = s_mpp_packet_get_length(packet);
    if (!pktData || pktSize == 0) {
        VLOG_ERROR(MODULE, "%s", "encoder returned an empty packet");
        releaseFrameResources();
        return false;
    }
    if (pktSize > maxOutSize) {
        VLOG_ERROR(MODULE,
                   "encoded size (%llu) exceeds output buffer (%llu)",
                   static_cast<unsigned long long>(pktSize),
                   static_cast<unsigned long long>(maxOutSize));
        releaseFrameResources();
        return false;
    }

    memcpy(outEncoded, pktData, pktSize);
    outSize = pktSize;
    releaseFrameResources();

    VLOG_DEBUG(MODULE, "MPP encode: %dx%d -> %llu bytes", s_width, s_height,
               static_cast<unsigned long long>(outSize));
    return true;
}

bool mpp::getHeader(uint8_t *outHeader, size_t &headerSize, size_t maxOutSize)
{
    if (!s_encoderReady || !outHeader || maxOutSize == 0)
        return false;

    // Get SPS/PPS via MPP_ENC_GET_EXTRA_INFO (project1 flow).
    MppPacket packet = nullptr;
    MppRet ret = s_mpi->control(s_ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
    if (ret != MPP_OK || !packet) {
        VLOG_ERROR(MODULE, "MPP_ENC_GET_EXTRA_INFO failed: %d", ret);
        return false;
    }

    void *pktData = s_mpp_packet_get_pos(packet);
    size_t pktSize = s_mpp_packet_get_length(packet);

    if (pktData && pktSize > 0) {
        if (pktSize > maxOutSize) {
            VLOG_ERROR(MODULE,
                       "header size (%llu) exceeds buffer (%llu)",
                       static_cast<unsigned long long>(pktSize),
                       static_cast<unsigned long long>(maxOutSize));
            // 注意:不要对 packet 调用 mpp_packet_deinit。
            // MPP_ENC_GET_EXTRA_INFO 返回的是 encoder 内部 buffer 的只读引用,
            // 释放它会破坏 encoder 内部状态,导致后续编码失败或崩溃。
            headerSize = 0;
            return false;
        }
        memcpy(outHeader, pktData, pktSize);
        headerSize = pktSize;
    } else {
        headerSize = 0;
        ret = MPP_NOK;
    }

    // 注意:不调用 s_mpp_packet_deinit(&packet)。
    // MPP_ENC_GET_EXTRA_INFO 返回的 packet 是 encoder 内部拥有的引用,
    // 调用者只读访问,不应释放(见 Rockchip MPP API 文档)。
    VLOG_DEBUG(MODULE, "MPP header: %llu bytes",
               static_cast<unsigned long long>(headerSize));
    return (ret == MPP_OK && headerSize > 0);
}
