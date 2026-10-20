// test_mpp_encoder.cpp — MPP hardware encoder unit test
//
// Build on RK3588S board:
//   g++ -std=c++17 -O2 -o test_mpp_encoder test_mpp_encoder.cpp ../media/mpp_encoder.cpp ../common/logmanager.cpp -I../media -I../common -ldl -lpthread
//
// Run:
//   ./test_mpp_encoder
//   ffplay -f h264 test_output.h264

#include "mpp_encoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>

// Generate a test YUV420P frame with color bars.
static void generateTestFrame(uint8_t *yuv, int width, int height) {
    int ySize = width * height;
    int uvSize = (width / 2) * (height / 2);

    // Y plane: horizontal gradient (0-255)
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            yuv[row * width + col] = static_cast<uint8_t>(col * 255 / width);
        }
    }

    // U plane: vertical gradient (0-255)
    uint8_t *uPlane = yuv + ySize;
    for (int row = 0; row < height / 2; row++) {
        for (int col = 0; col < width / 2; col++) {
            uPlane[row * (width / 2) + col] = static_cast<uint8_t>(row * 255 / (height / 2));
        }
    }

    // V plane: constant 128 (neutral)
    uint8_t *vPlane = uPlane + uvSize;
    memset(vPlane, 128, uvSize);
}

// Check if buffer starts with Annex-B start code (00 00 00 01).
static bool hasAnnexBStartCode(const uint8_t *data, size_t size) {
    return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

// Find NAL unit type in Annex-B stream.
static uint8_t findH264NalType(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            return data[i + 4] & 0x1F;
        }
    }
    return 0xFF;  // Not found
}

int main() {
    const int WIDTH = 800;
    const int HEIGHT = 600;
    const int FPS = 25;
    const int BITRATE = 2000000;
    const int GOP = 25;
    const int NUM_FRAMES = 10;
    const size_t MAX_BUF = 1024 * 1024;  // 1MB

    printf("=== MPP Hardware Encoder Test ===\n");
    printf("Resolution: %dx%d, FPS: %d, Bitrate: %d\n", WIDTH, HEIGHT, FPS, BITRATE);
    printf("Testing %d frames...\n\n", NUM_FRAMES);

    // Step 1: Check MPP availability.
    printf("[1] Checking MPP availability... ");
    if (!mpp::isAvailable()) {
        printf("FAILED\n");
        printf("    librockchip_mpp.so not found or missing symbols.\n");
        printf("    This test must run on RK3588S board with MPP library.\n");
        return 1;
    }
    printf("OK\n");

    // Step 2: Initialize encoder.
    printf("[2] Initializing MPP H.264 encoder... ");
    mpp::EncoderConfig config;
    config.codec = mpp::CodecType::H264;
    config.width = WIDTH;
    config.height = HEIGHT;
    config.fps = FPS;
    config.bitrate = BITRATE;
    config.gop = GOP;

    if (!mpp::initEncoder(config)) {
        printf("FAILED\n");
        printf("    MPP encoder initialization failed.\n");
        return 1;
    }
    printf("OK\n");

    // Step 3: Generate test frame.
    printf("[3] Generating test YUV420P frame (%dx%d)... ", WIDTH, HEIGHT);
    size_t frameSize = WIDTH * HEIGHT * 3 / 2;
    std::vector<uint8_t> yuvFrame(frameSize);
    generateTestFrame(yuvFrame.data(), WIDTH, HEIGHT);
    printf("OK (%zu bytes)\n", frameSize);

    // Step 4: Encode frames.
    printf("[4] Encoding %d frames...\n", NUM_FRAMES);
    std::vector<uint8_t> encodedBuf(MAX_BUF);
    std::vector<uint8_t> allEncoded;
    int idrCount = 0;
    int pFrameCount = 0;
    bool headerExtracted = false;
    std::vector<uint8_t> headerBuf(1024);
    size_t headerSize = 0;

    for (int i = 0; i < NUM_FRAMES; i++) {
        size_t encSize = 0;
        if (!mpp::encode(yuvFrame.data(), encodedBuf.data(), encSize, MAX_BUF)) {
            printf("    Frame %d: ENCODE FAILED\n", i);
            continue;
        }

        // Check Annex-B start code.
        if (!hasAnnexBStartCode(encodedBuf.data(), encSize)) {
            printf("    Frame %d: INVALID (no Annex-B start code), size=%zu\n", i, encSize);
            continue;
        }

        // Find NAL type.
        uint8_t nalType = findH264NalType(encodedBuf.data(), encSize);
        const char *frameType = "P";
        if (nalType == 7) {  // SPS
            frameType = "IDR (with SPS)";
            idrCount++;
        } else if (nalType == 5) {  // IDR
            frameType = "IDR";
            idrCount++;
        } else if (nalType == 1) {  // P-frame
            frameType = "P";
            pFrameCount++;
        }

        printf("    Frame %d: %s, NAL type=%d, size=%zu bytes\n",
               i, frameType, nalType, encSize);

        // Append to output file.
        allEncoded.insert(allEncoded.end(),
                         encodedBuf.begin(), encodedBuf.begin() + encSize);

        // Extract header after first frame.
        if (!headerExtracted && encSize > 0) {
            if (mpp::getHeader(headerBuf.data(), headerSize, headerBuf.size())) {
                printf("\n[5] SPS/PPS header extracted: %zu bytes\n", headerSize);
                if (hasAnnexBStartCode(headerBuf.data(), headerSize)) {
                    uint8_t spsType = headerBuf[4] & 0x1F;
                    printf("    First NAL type: %d (%s)\n",
                           spsType, spsType == 7 ? "SPS" : "unknown");
                }
                headerExtracted = true;
            } else {
                printf("\n[5] SPS/PPS extraction FAILED (may need more frames)\n");
            }
        }
    }

    // Step 6: Summary.
    printf("\n=== Test Summary ===\n");
    printf("Total frames encoded: %d\n", idrCount + pFrameCount);
    printf("IDR frames: %d\n", idrCount);
    printf("P frames: %d\n", pFrameCount);
    printf("Total encoded size: %zu bytes\n", allEncoded.size());
    printf("Average frame size: %zu bytes\n",
           (idrCount + pFrameCount) > 0 ? allEncoded.size() / (idrCount + pFrameCount) : 0);

    // Step 7: Save to file.
    const char *outputFile = "test_output.h264";
    std::ofstream ofs(outputFile, std::ios::binary);
    if (ofs.is_open()) {
        // Write SPS/PPS header first.
        if (headerExtracted && headerSize > 0) {
            ofs.write(reinterpret_cast<const char*>(headerBuf.data()), headerSize);
        }
        ofs.write(reinterpret_cast<const char*>(allEncoded.data()), allEncoded.size());
        ofs.close();
        printf("\nOutput saved to: %s (%zu bytes)\n", outputFile,
               allEncoded.size() + (headerExtracted ? headerSize : 0));
        printf("Verify with: ffplay -f h264 %s\n", outputFile);
    }

    // Step 8: Cleanup.
    printf("\n[6] Releasing MPP encoder... ");
    mpp::deinitEncoder();
    printf("OK\n");

    // Final result.
    printf("\n=== RESULT: %s ===\n",
           (idrCount > 0 && pFrameCount > 0) ? "PASS" : "FAIL");

    return (idrCount > 0 && pFrameCount > 0) ? 0 : 1;
}
