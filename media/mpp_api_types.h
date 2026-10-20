#ifndef MPP_API_TYPES_H
#define MPP_API_TYPES_H

// Shared MPP ABI type definitions for librockchip_mpp.so dynamic loading.
//
// Both mpp_decoder.cpp and mpp_encoder.cpp use dlopen/dlsym to access the
// Rockchip MPP library at runtime. The struct MppApi below MUST match the
// real MppApi_t in /usr/include/rockchip/rk_mpi.h exactly — any mismatch
// causes function-pointer offset errors and runtime crashes.
//
// Real struct layout (from board header, aarch64):
//   uint32_t size;                              // +0
//   uint32_t version;                           // +4
//   (*decode)(ctx, packet, frame);              // +8
//   (*decode_put_packet)(ctx, packet);          // +16
//   (*decode_get_frame)(ctx, frame);            // +24
//   (*encode)(ctx, frame, packet);              // +32
//   (*encode_put_frame)(ctx, frame);            // +40
//   (*encode_get_packet)(ctx, packet);          // +48
//   (*isp)(ctx, dst, src);                      // +56
//   (*isp_put_frame)(ctx, frame);               // +64
//   (*isp_get_frame)(ctx, frame);               // +72
//   (*poll)(ctx, type, timeout);                // +80
//   (*dequeue)(ctx, type, task);                // +88
//   (*enqueue)(ctx, type, task);                // +96
//   (*reset)(ctx);                              // +104
//   (*control)(ctx, cmd, param);                // +112
//   uint32_t reserv[16];                        // +120
// Total: 184 bytes

#include <cstddef>
#include <cstdint>

#include "mpp_abi_constants.h"

// Base opaque typedefs (mirror rockchip MPP headers).
typedef void *MppCtx;
typedef void *MppParam;
typedef void *MppPacket;
typedef void *MppFrame;
typedef void *MppBuffer;
typedef int   MppRet;
typedef int   MppCodingType;
typedef int   MpiCmd;
typedef int   MppFrameFormat;
typedef int   MppCtxType;
typedef int   MppPortType;
typedef int   MppPollType;
typedef void *MppTask;

// MPP return codes.
#define MPP_OK              0
#define MPP_NOK            (-1)
#define MPP_ERR_TIMEOUT     mpp::abi::kErrorTimeout

// MPP frame formats (subset). Values confirmed from board mpp_frame.h.
#define MPP_FMT_YUV420SP mpp::abi::kFormatYuv420Sp
#define MPP_FMT_YUV420P  mpp::abi::kFormatYuv420P

// MPP buffer type enum. Values confirmed from board mpp_buffer.h.
#define MPP_BUFFER_TYPE_ION mpp::abi::kBufferTypeIon

// MppApi structure — the MPI function table returned by mpp_create().
struct MppApi {
    uint32_t size;
    uint32_t version;
    // simple data flow interface
    MppRet (*decode)(MppCtx ctx, MppPacket packet, MppFrame *frame);
    MppRet (*decode_put_packet)(MppCtx ctx, MppPacket packet);
    MppRet (*decode_get_frame)(MppCtx ctx, MppFrame *frame);
    MppRet (*encode)(MppCtx ctx, MppFrame frame, MppPacket *packet);
    MppRet (*encode_put_frame)(MppCtx ctx, MppFrame frame);
    MppRet (*encode_get_packet)(MppCtx ctx, MppPacket *packet);
    // ISP interface (reserved)
    MppRet (*isp)(MppCtx ctx, MppFrame dst, MppFrame src);
    MppRet (*isp_put_frame)(MppCtx ctx, MppFrame frame);
    MppRet (*isp_get_frame)(MppCtx ctx, MppFrame *frame);
    // advance data flow interface
    MppRet (*poll)(MppCtx ctx, MppPortType type, MppPollType timeout);
    MppRet (*dequeue)(MppCtx ctx, MppPortType type, MppTask *task);
    MppRet (*enqueue)(MppCtx ctx, MppPortType type, MppTask task);
    // control interface
    MppRet (*reset)(MppCtx ctx);
    MppRet (*control)(MppCtx ctx, MpiCmd cmd, MppParam param);
    uint32_t reserv[16];
};

// Compile-time layout verification — checked in every translation unit
// that includes this header.
static_assert(offsetof(MppApi, size)              ==   0, "MppApi.size offset");
static_assert(offsetof(MppApi, version)           ==   4, "MppApi.version offset");
static_assert(offsetof(MppApi, decode)            ==   8, "MppApi.decode offset");
static_assert(offsetof(MppApi, decode_put_packet) ==  16, "MppApi.decode_put_packet offset");
static_assert(offsetof(MppApi, decode_get_frame)  ==  24, "MppApi.decode_get_frame offset");
static_assert(offsetof(MppApi, encode)            ==  32, "MppApi.encode offset");
static_assert(offsetof(MppApi, encode_put_frame)  ==  40, "MppApi.encode_put_frame offset");
static_assert(offsetof(MppApi, encode_get_packet) ==  48, "MppApi.encode_get_packet offset");
static_assert(offsetof(MppApi, isp)               ==  56, "MppApi.isp offset");
static_assert(offsetof(MppApi, isp_put_frame)     ==  64, "MppApi.isp_put_frame offset");
static_assert(offsetof(MppApi, isp_get_frame)     ==  72, "MppApi.isp_get_frame offset");
static_assert(offsetof(MppApi, poll)              ==  80, "MppApi.poll offset");
static_assert(offsetof(MppApi, dequeue)           ==  88, "MppApi.dequeue offset");
static_assert(offsetof(MppApi, enqueue)           ==  96, "MppApi.enqueue offset");
static_assert(offsetof(MppApi, reset)             == 104, "MppApi.reset offset");
static_assert(offsetof(MppApi, control)           == 112, "MppApi.control offset");
static_assert(offsetof(MppApi, reserv)            == 120, "MppApi.reserv offset");
static_assert(sizeof(MppApi) == 184, "MppApi total size must be 184 bytes");

// Common MPP API function pointer types.
typedef MppRet (*pfn_mpp_create)(MppCtx *ctx, MppApi **api);
typedef MppRet (*pfn_mpp_init)(MppCtx ctx, MppCtxType ctxType, MppCodingType codingType);
typedef MppRet (*pfn_mpp_destroy)(MppCtx ctx);

#endif // MPP_API_TYPES_H
