/*
 * test_mpp_direct.cpp — Direct-link Rockchip MPP encoder regression test.
 *
 * Build on the RK3588S board from the RK3588S_CarTerminal directory:
 *   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
 *       tests/test_mpp_direct.cpp -lrockchip_mpp -lpthread \
 *       -o test_mpp_direct
 *
 * Run:
 *   ./test_mpp_direct
 *
 * Success criteria:
 *   - all ten frames produce non-zero Annex-B output;
 *   - test_mpp_direct.h264 is non-empty.
 */

#include "../media/mpp_frame_layout.h"
#include "../media/mpp_abi_constants.h"

#if __has_include(<rockchip/mpp_buffer.h>)
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#else
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_mpi.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace {

static_assert(mpp::abi::kFormatYuv420P ==
                  static_cast<int>(MPP_FMT_YUV420P),
              "application I420 ABI constant differs from board headers");
static_assert(mpp::abi::kCodingAvc ==
                  static_cast<int>(MPP_VIDEO_CodingAVC),
              "application AVC ABI constant differs from board headers");
static_assert(mpp::abi::kCodingHevc ==
                  static_cast<int>(MPP_VIDEO_CodingHEVC),
              "application HEVC ABI constant differs from board headers");
static_assert(mpp::abi::kErrorTimeout ==
                  static_cast<int>(MPP_ERR_TIMEOUT),
              "application timeout ABI constant differs from board headers");

constexpr int kWidth = 800;
constexpr int kHeight = 600;
constexpr int kFps = 25;
constexpr int kBitrate = 2'000'000;
constexpr int kGop = 25;
constexpr int kFrameCount = 10;
constexpr std::size_t kAlignment = 16;

bool setConfig(MppEncCfg cfg, const char *key, int value)
{
    const MPP_RET ret = mpp_enc_cfg_set_s32(cfg, key, value);
    if (ret != MPP_OK) {
        std::fprintf(stderr, "mpp_enc_cfg_set_s32(%s=%d) failed: %d\n",
                     key, value, ret);
        return false;
    }
    return true;
}

bool hasAnnexBStartCode(const std::uint8_t *data, std::size_t size)
{
    if (!data)
        return false;

    for (std::size_t i = 0; i + 3U < size; ++i) {
        if (data[i] == 0 && data[i + 1U] == 0) {
            if (data[i + 2U] == 1)
                return true;
            if (i + 3U < size && data[i + 2U] == 0 && data[i + 3U] == 1)
                return true;
        }
    }
    return false;
}

void generateI420Frame(std::vector<std::uint8_t> &frame, int frameIndex)
{
    const std::size_t ySize =
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight);
    const std::size_t uvSize = ySize / 4U;

    std::uint8_t *yPlane = frame.data();
    std::uint8_t *uPlane = yPlane + ySize;
    std::uint8_t *vPlane = uPlane + uvSize;

    for (int row = 0; row < kHeight; ++row) {
        for (int column = 0; column < kWidth; ++column) {
            yPlane[static_cast<std::size_t>(row) * kWidth + column] =
                static_cast<std::uint8_t>((column + frameIndex * 8) & 0xFF);
        }
    }
    std::memset(uPlane, 96 + frameIndex, uvSize);
    std::memset(vPlane, 160 - frameIndex, uvSize);
}

} // namespace

int main()
{
    std::printf("=== Direct MPP Encoder Regression Test ===\n");
    std::printf("MPP_FMT_YUV420P=%d, MPP_FMT_YUV420SP=%d, "
                "MPP_VIDEO_CodingAVC=%d\n",
                static_cast<int>(MPP_FMT_YUV420P),
                static_cast<int>(MPP_FMT_YUV420SP),
                static_cast<int>(MPP_VIDEO_CodingAVC));

    const auto layout = mpp::frame_layout::Layout::forI420(
        kWidth, kHeight, kAlignment);
    if (!layout.valid()) {
        std::fprintf(stderr, "invalid I420 layout\n");
        return 1;
    }
    std::printf("Frame layout: %dx%d, stride=%zux%zu, buffer=%zu bytes\n",
                kWidth, kHeight, layout.horizontalStride,
                layout.verticalStride, layout.bufferSize);

    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
    MppEncCfg cfg = nullptr;
    MppBufferGroup group = nullptr;
    bool testPassed = false;

    do {
        MPP_RET ret = mpp_create(&ctx, &mpi);
        if (ret != MPP_OK || !ctx || !mpi) {
            std::fprintf(stderr, "mpp_create failed: %d\n", ret);
            break;
        }

        ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
        if (ret != MPP_OK) {
            std::fprintf(stderr, "mpp_init failed: %d\n", ret);
            break;
        }

        ret = mpp_enc_cfg_init(&cfg);
        if (ret != MPP_OK || !cfg) {
            std::fprintf(stderr, "mpp_enc_cfg_init failed: %d\n", ret);
            break;
        }

        ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
        if (ret != MPP_OK) {
            std::fprintf(stderr, "MPP_ENC_GET_CFG failed: %d\n", ret);
            break;
        }

        const bool configSet =
            setConfig(cfg, "prep:width", kWidth) &&
            setConfig(cfg, "prep:height", kHeight) &&
            setConfig(cfg, "prep:hor_stride",
                      static_cast<int>(layout.horizontalStride)) &&
            setConfig(cfg, "prep:ver_stride",
                      static_cast<int>(layout.verticalStride)) &&
            setConfig(cfg, "prep:format", MPP_FMT_YUV420P) &&
            setConfig(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR) &&
            setConfig(cfg, "rc:bps_target", kBitrate) &&
            setConfig(cfg, "rc:bps_max", kBitrate * 2) &&
            setConfig(cfg, "rc:bps_min", kBitrate / 2) &&
            setConfig(cfg, "rc:fps_in_flex", 0) &&
            setConfig(cfg, "rc:fps_in_num", kFps) &&
            setConfig(cfg, "rc:fps_in_denom", 1) &&
            setConfig(cfg, "rc:fps_out_flex", 0) &&
            setConfig(cfg, "rc:fps_out_num", kFps) &&
            setConfig(cfg, "rc:fps_out_denom", 1) &&
            setConfig(cfg, "rc:gop", kGop) &&
            setConfig(cfg, "codec:type", MPP_VIDEO_CodingAVC) &&
            setConfig(cfg, "h264:profile", 66) &&
            setConfig(cfg, "h264:level", 40) &&
            setConfig(cfg, "h264:cabac_en", 0) &&
            setConfig(cfg, "h264:qp_init", 26) &&
            setConfig(cfg, "h264:qp_min", 20) &&
            setConfig(cfg, "h264:qp_max", 35);
        if (!configSet)
            break;

        ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
        if (ret != MPP_OK) {
            std::fprintf(stderr, "MPP_ENC_SET_CFG failed: %d\n", ret);
            break;
        }

        ret = mpp_buffer_group_get(
            &group, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL,
            "test_mpp_direct", "main");
        if (ret != MPP_OK || !group) {
            std::fprintf(stderr, "mpp_buffer_group_get_internal failed: %d\n",
                         ret);
            break;
        }

        const std::size_t sourceSize =
            static_cast<std::size_t>(kWidth) *
            static_cast<std::size_t>(kHeight) * 3U / 2U;
        std::vector<std::uint8_t> source(sourceSize);
        std::ofstream output("test_mpp_direct.h264", std::ios::binary);
        if (!output) {
            std::fprintf(stderr, "failed to open test_mpp_direct.h264\n");
            break;
        }

        int encodedFrames = 0;
        std::size_t totalBytes = 0;

        for (int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex) {
            MppFrame frame = nullptr;
            MppBuffer buffer = nullptr;
            MppPacket packet = nullptr;

            generateI420Frame(source, frameIndex);

            ret = mpp_frame_init(&frame);
            if (ret != MPP_OK || !frame) {
                std::fprintf(stderr, "frame %d: mpp_frame_init failed: %d\n",
                             frameIndex, ret);
                break;
            }

            ret = mpp_buffer_get(group, &buffer, layout.bufferSize);
            if (ret != MPP_OK || !buffer) {
                std::fprintf(stderr, "frame %d: mpp_buffer_get failed: %d\n",
                             frameIndex, ret);
                mpp_frame_deinit(&frame);
                break;
            }

            auto *destination = static_cast<std::uint8_t *>(
                mpp_buffer_get_ptr(buffer));
            if (!destination) {
                std::fprintf(stderr,
                             "frame %d: mpp_buffer_get_ptr returned null\n",
                             frameIndex);
                mpp_buffer_put(buffer);
                mpp_frame_deinit(&frame);
                break;
            }

            ret = mpp_buffer_sync_begin(buffer);
            if (ret != MPP_OK) {
                std::fprintf(stderr,
                             "frame %d: mpp_buffer_sync_begin failed: %d\n",
                             frameIndex, ret);
                mpp_buffer_put(buffer);
                mpp_frame_deinit(&frame);
                break;
            }
            const bool copied = mpp::frame_layout::copyI420ToAligned(
                source.data(), source.size(), destination,
                layout.bufferSize, layout);
            ret = mpp_buffer_sync_end(buffer);
            if (!copied || ret != MPP_OK) {
                std::fprintf(stderr,
                             "frame %d: I420 copy/sync failed (sync=%d)\n",
                             frameIndex, ret);
                mpp_buffer_put(buffer);
                mpp_frame_deinit(&frame);
                break;
            }

            mpp_frame_set_width(frame, kWidth);
            mpp_frame_set_height(frame, kHeight);
            mpp_frame_set_hor_stride(
                frame, static_cast<RK_U32>(layout.horizontalStride));
            mpp_frame_set_ver_stride(
                frame, static_cast<RK_U32>(layout.verticalStride));
            mpp_frame_set_fmt(frame, MPP_FMT_YUV420P);
            mpp_frame_set_buffer(frame, buffer);
            mpp_frame_set_eos(frame, 0);

            ret = mpi->encode_put_frame(ctx, frame);
            if (ret == MPP_OK) {
                for (int retry = 0; retry < 50; ++retry) {
                    ret = mpi->encode_get_packet(ctx, &packet);
                    if (ret == MPP_OK && packet)
                        break;
                    if (ret != MPP_ERR_TIMEOUT)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            if (ret != MPP_OK || !packet) {
                std::fprintf(stderr,
                             "frame %d: encoder returned no packet (ret=%d)\n",
                             frameIndex, ret);
                if (packet)
                    mpp_packet_deinit(&packet);
                mpp_buffer_put(buffer);
                mpp_frame_deinit(&frame);
                break;
            }

            const auto *packetData = static_cast<const std::uint8_t *>(
                mpp_packet_get_pos(packet));
            const std::size_t packetSize = mpp_packet_get_length(packet);
            const bool validPacket =
                packetData && packetSize > 0 &&
                hasAnnexBStartCode(packetData, packetSize);

            std::printf("Frame %d: ret=%d, packet=%zu bytes, Annex-B=%s\n",
                        frameIndex, ret, packetSize,
                        validPacket ? "yes" : "no");

            if (validPacket) {
                output.write(reinterpret_cast<const char *>(packetData),
                             static_cast<std::streamsize>(packetSize));
                totalBytes += packetSize;
                ++encodedFrames;
            }

            mpp_packet_deinit(&packet);
            mpp_buffer_put(buffer);
            mpp_frame_deinit(&frame);

            if (!validPacket)
                break;
        }

        output.close();
        std::printf("Encoded %d/%d frames, total=%zu bytes\n",
                    encodedFrames, kFrameCount, totalBytes);
        testPassed =
            encodedFrames == kFrameCount && totalBytes > 0;
    } while (false);

    if (group)
        mpp_buffer_group_put(group);
    if (cfg)
        mpp_enc_cfg_deinit(cfg);
    if (ctx && mpi)
        mpi->reset(ctx);
    if (ctx)
        mpp_destroy(ctx);

    std::printf("=== RESULT: %s ===\n", testPassed ? "PASS" : "FAIL");
    return testPassed ? 0 : 1;
}
