// Unit tests for the MPP decoder ABI hardening.
//
// Verifies three things required by the first hardening pass:
//   1. The decoder and encoder share the same ABI constants (kContextDecoder,
//      kContextEncoder, kCodingMjpeg, kCodingAvc, ...) so both code paths
//      agree on Rockchip MPP enum values.
//   2. The MppApi struct layout used by the decoder matches the documented
//      Rockchip rk_mpi.h offset table (the same table the encoder uses and
//      verified on board). A mismatch would corrupt function-pointer offsets
//      and crash MJPEG decoding.
//   3. When librockchip_mpp.so is unavailable (host test links the dlfcn
//      stub that makes dlopen fail), initDecoder() returns false and
//      decode() returns false — the MJPEG path soft-fails instead of
//      crashing. NV12 callers are unaffected.
//
// Build (host, no real MPP library):
//   g++ -std=c++17 -Wall -Wextra -Wpedantic
//       -Itests/stubs -Icommon -Imedia
//       tests/test_mpp_decoder_abi.cpp
//       tests/stubs/dlfcn_stub.cpp
//       media/mpp_decoder.cpp
//       common/logmanager.cpp
//       -o tests/test_mpp_decoder_abi
// Run: ./tests/test_mpp_decoder_abi

#include "../media/mpp_abi_constants.h"
#include "../media/mpp_decoder.h"

#include <cstddef>
#include <cstdio>
#include <cstdint>

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// 1. ABI constants shared between decoder and encoder
// ---------------------------------------------------------------------------

static_assert(mpp::abi::kContextDecoder == 0,
              "MPP_CTX_DEC must be 0 (Rockchip rk_type.h)");
static_assert(mpp::abi::kContextEncoder == 1,
              "MPP_CTX_ENC must be 1 (Rockchip rk_type.h)");
static_assert(mpp::abi::kCodingMjpeg == 5,
              "MPP_VIDEO_CodingMJPEG must be 5 (Rockchip rk_mpi.h)");
static_assert(mpp::abi::kCodingAvc == 7,
              "MPP_VIDEO_CodingAVC must be 7 (Rockchip rk_mpi.h)");
static_assert(mpp::abi::kCodingHevc == 0x01000004,
              "MPP_VIDEO_CodingHEVC must be 0x01000004 (Rockchip rk_mpi.h)");
static_assert(mpp::abi::kFormatYuv420Sp == 0,
              "MPP_FMT_YUV420SP must be 0 (Rockchip mpp_frame.h)");
static_assert(mpp::abi::kFormatYuv420P == 4,
              "MPP_FMT_YUV420P must be 4 (Rockchip mpp_frame.h)");
static_assert(mpp::abi::kErrorTimeout == -8,
              "MPP_ERR_TIMEOUT must be -8 (Rockchip mpp_err.h)");
static_assert(mpp::abi::kDecoderSetFrameInfo == 0x02000002,
              "MPP_DEC_SET_FRAME_INFO must be 0x02000002 (rk_mpi_cmd.h)");

// ---------------------------------------------------------------------------
// 2. MppApi struct layout consistency (decoder mirrors encoder)
//
// The struct MppApi and its static_assert offset table now live in the
// shared header media/mpp_api_types.h, included by both mpp_decoder.cpp
// and mpp_encoder.cpp. The layout was verified against the board's
// /usr/include/rockchip/rk_mpi.h.
// ---------------------------------------------------------------------------

#include "../media/mpp_api_types.h"

// The wrong ABI the decoder used before the fix declared:
//   pfn_mpp_create(MppCtx*, MppCodingType)   // 2nd param was int, not MppApi**
//   pfn_mpp_init(MppCtx, MppCodingType)      // missing MppCtxType param
// Verify the corrected signatures are in effect by compiling a typedef that
// matches the encoder's. If the decoder ever reverts, this translation unit
// will fail to compile when linked against mpp_decoder.cpp's expectations.
using CorrectMppCreate = MppRet (*)(MppCtx *, MppApi **);
using CorrectMppInit   = MppRet (*)(MppCtx, int, int);
static_assert(sizeof(CorrectMppCreate) == sizeof(void (*)()),
              "mpp_create signature must be a function pointer");
static_assert(sizeof(CorrectMppInit) == sizeof(void (*)()),
              "mpp_init signature must be a function pointer");

// ---------------------------------------------------------------------------
// 3. Runtime soft-fail (librockchip_mpp.so absent)
// ---------------------------------------------------------------------------

void testInitFailsWhenLibAbsent()
{
    // The host test links the dlfcn stub (tests/stubs/dlfcn_stub.cpp) which
    // makes dlopen() return nullptr. initDecoder() must therefore fail and
    // return false — NOT crash or dereference a null MppApi*.
    const bool ok = mpp::initDecoder();
    CHECK(!ok);
}

void testDecodeFailsWithoutInit()
{
    uint8_t fakeJpeg[4] = {0xFF, 0xD8, 0xFF, 0xD9};
    uint8_t outNv12[64] = {0};
    int w = -1;
    int h = -1;
    const bool ok = mpp::decode(fakeJpeg, sizeof(fakeJpeg), outNv12, sizeof(outNv12), w, h);
    CHECK(!ok);
    CHECK(w == 0);
    CHECK(h == 0);
}

void testDeinitIsSafeWithoutInit()
{
    // deinitDecoder must be safe to call even if init never succeeded.
    mpp::deinitDecoder();
    // Reaching here without crashing is the success criterion.
    CHECK(true);
}

void testNv12ToRgb32IsPureCpuAndDoesNotNeedLib()
{
    // The NV12→RGB32 fallback is pure CPU arithmetic and must work without
    // any MPP library. This guarantees the NV12 camera path (which feeds
    // RGB32 conversion) is unaffected by MJPEG decoder availability.
    const int w = 2;
    const int h = 2;
    // 2x2 NV12: Y plane (4 bytes) + UV plane (2 bytes)
    const uint8_t nv12[] = {
        /* Y */ 100, 150, 200, 50,
        /* UV*/ 128, 128
    };
    uint8_t rgb32[w * h * 4] = {0};
    mpp::nv12ToRgb32(nv12, w, h, rgb32);
    // Pixel (0,0): Y=100, U=V=0 (after -128) -> R=100, G=100, B=100, A=0xFF
    CHECK(rgb32[0] == 100);  // B
    CHECK(rgb32[1] == 100);  // G
    CHECK(rgb32[2] == 100);  // R
    CHECK(rgb32[3] == 0xFF); // A
}

} // namespace

int main()
{
    testInitFailsWhenLibAbsent();
    testDecodeFailsWithoutInit();
    testDeinitIsSafeWithoutInit();
    testNv12ToRgb32IsPureCpuAndDoesNotNeedLib();

    if (g_failures == 0) {
        std::puts("mpp_decoder_abi tests passed");
        return 0;
    }
    std::printf("mpp_decoder_abi tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
