#include "../media/mpp_abi_constants.h"

#if __has_include(<rockchip/mpp_frame.h>)
#include <rockchip/mpp_err.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_type.h>
#define MPP_TEST_HAS_SDK_HEADERS 1
#elif __has_include(<mpp_frame.h>)
#include <mpp_err.h>
#include <mpp_frame.h>
#include <rk_type.h>
#define MPP_TEST_HAS_SDK_HEADERS 1
#endif

#include <cassert>
#include <cstdio>

int main()
{
    static_assert(mpp::abi::kContextEncoder == 1,
                  "MPP encoder context ABI value changed");
    static_assert(mpp::abi::kCodingAvc == 7,
                  "MPP H.264 coding ABI value changed");
    static_assert(mpp::abi::kCodingHevc == 0x01000004,
                  "MPP H.265 coding ABI value changed");
    static_assert(mpp::abi::kFormatYuv420Sp == 0,
                  "MPP NV12 ABI value changed");
    static_assert(mpp::abi::kFormatYuv420P == 4,
                  "MPP I420 ABI value changed");
    static_assert(mpp::abi::kFormatYuv420P != 1,
                  "value 1 is the 10-bit semiplanar format");
    static_assert(mpp::abi::kErrorTimeout == -8,
                  "MPP timeout ABI value changed");

#ifdef MPP_TEST_HAS_SDK_HEADERS
    static_assert(mpp::abi::kContextEncoder == static_cast<int>(MPP_CTX_ENC),
                  "context value differs from installed MPP headers");
    static_assert(mpp::abi::kCodingAvc ==
                      static_cast<int>(MPP_VIDEO_CodingAVC),
                  "AVC value differs from installed MPP headers");
    static_assert(mpp::abi::kCodingHevc ==
                      static_cast<int>(MPP_VIDEO_CodingHEVC),
                  "HEVC value differs from installed MPP headers");
    static_assert(mpp::abi::kFormatYuv420Sp ==
                      static_cast<int>(MPP_FMT_YUV420SP),
                  "NV12 value differs from installed MPP headers");
    static_assert(mpp::abi::kFormatYuv420P ==
                      static_cast<int>(MPP_FMT_YUV420P),
                  "I420 value differs from installed MPP headers");
    static_assert(mpp::abi::kErrorTimeout ==
                      static_cast<int>(MPP_ERR_TIMEOUT),
                  "timeout value differs from installed MPP headers");
#endif

    assert(mpp::abi::kFormatYuv420P == 4);
    std::puts("mpp_abi_constants tests passed");
    return 0;
}
