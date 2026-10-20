// Host test for the MPP MJPEG decoder happy path with a mocked MPP library.
//
// The mock intentionally makes mpp_frame_get_buffer() return an MppBuffer
// handle whose bytes are not pixel data. decode() must call
// mpp_buffer_get_ptr_with_caller() to obtain the real frame pointer.

#include "../media/mpp_decoder.h"

#include <cstdio>
#include <cstdint>

extern "C" void mpp_mock_reset_sync_counts();
extern "C" int mpp_mock_sync_begin_count();
extern "C" int mpp_mock_sync_end_count();

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

void testDecoderCopiesFromMppBufferPointer()
{
    CHECK(mpp::initDecoder());

    const uint8_t fakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
    uint8_t outNv12[6] = {0};
    int width = 0;
    int height = 0;

    CHECK(mpp::decode(fakeJpeg, sizeof(fakeJpeg), outNv12, sizeof(outNv12), width, height));
    CHECK(width == 2);
    CHECK(height == 2);

    CHECK(outNv12[0] == 10);
    CHECK(outNv12[1] == 20);
    CHECK(outNv12[2] == 30);
    CHECK(outNv12[3] == 40);
    CHECK(outNv12[4] == 128);
    CHECK(outNv12[5] == 128);

    mpp::deinitDecoder();
}

void testDecoderBalancesCacheSyncWhenCallerBufferIsTooSmall()
{
    CHECK(mpp::initDecoder());
    mpp_mock_reset_sync_counts();

    const uint8_t fakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
    uint8_t outNv12[5] = {0};
    int width = 0;
    int height = 0;

    CHECK(!mpp::decode(fakeJpeg, sizeof(fakeJpeg), outNv12,
                       sizeof(outNv12), width, height));
    CHECK(mpp_mock_sync_begin_count() == 1);
    CHECK(mpp_mock_sync_end_count() == 1);

    mpp::deinitDecoder();
}

} // namespace

int main()
{
    testDecoderCopiesFromMppBufferPointer();
    testDecoderBalancesCacheSyncWhenCallerBufferIsTooSmall();

    if (g_failures == 0) {
        std::puts("mpp_decoder_mock tests passed");
        return 0;
    }
    std::printf("mpp_decoder_mock tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
