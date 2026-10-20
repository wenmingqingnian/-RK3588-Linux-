#include "rga_converter.h"
#include "logmanager.h"

#include <cstring>
#include <cstdint>
#include <mutex>

// ---------------------------------------------------------------------------
// 板端 librga2 2.2.0 提供 /usr/include/rga/im2d.h，直接用官方 SDK ABI。
// Host（无 librga）走 __has_include 检测，自动降级 CPU。
// ---------------------------------------------------------------------------
#if defined(__has_include) && __has_include(<im2d.h>)
#define HAVE_RGA_IM2D 1
#include <im2d.h>
#include <RgaApi.h>
#include <rga.h>
#else
#define HAVE_RGA_IM2D 0
#endif

static const char *MODULE = "rga";

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

static bool g_rgaAvailable = false;
static std::mutex g_rgaLifecycleMutex;
static int g_rgaUsers = 0;

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

void rga::init()
{
    std::lock_guard<std::mutex> lock(g_rgaLifecycleMutex);
    ++g_rgaUsers;
    if (g_rgaUsers > 1)
        return;

#if HAVE_RGA_IM2D
    // Week10 has one CaptureThread per camera. Keep librga initialized until
    // the last capture thread exits so stopping Rear cannot disable Front.
    int ret = c_RkRgaInit();
    if (ret != 0) {
        VLOG_WARN(MODULE, "c_RkRgaInit() returned %d, RGA disabled; using CPU", ret);
        g_rgaAvailable = false;
        return;
    }
    g_rgaAvailable = true;
    VLOG_INFO(MODULE, "RGA hardware acceleration enabled (users=%d)",
              g_rgaUsers);
#else
    VLOG_INFO(MODULE, "RGA SDK not available (<im2d.h> not found); using CPU");
    g_rgaAvailable = false;
#endif
}

void rga::deinit()
{
    std::lock_guard<std::mutex> lock(g_rgaLifecycleMutex);
    if (g_rgaUsers <= 0)
        return;

    --g_rgaUsers;
    if (g_rgaUsers > 0)
        return;

#if HAVE_RGA_IM2D
    if (g_rgaAvailable) {
        c_RkRgaDeInit();
        g_rgaAvailable = false;
    }
#endif
}

bool rga::isAvailable()
{
    return g_rgaAvailable;
}

// ---------------------------------------------------------------------------
// RGA 硬件加速：NV12 -> RGBA8888
// ---------------------------------------------------------------------------
#if HAVE_RGA_IM2D

// RGA 硬件要求 buffer 起始地址 16 字节对齐，宽高 2 对齐（NV12 平面要求）。
// V4L2 MMAP buffer 已满足对齐，但 malloc 出来的用户态 buffer 不一定，所以
// 这里的 dst 也用 posix_memalign 分配（由调用方保证，见 capture_thread.cpp）。
static bool convertWithRga(const uint8_t *src, int width, int height,
                           uint8_t *dst)
{
    // RGA src: NV12, virtual address, width/height = 实际尺寸
    rga_buffer_t srcBuf = wrapbuffer_virtualaddr(
        const_cast<uint8_t *>(src), width, height, RK_FORMAT_YCbCr_420_SP);
    if (srcBuf.width == 0 && srcBuf.height == 0) {
        VLOG_WARN(MODULE, "wrapbuffer_virtualaddr(src) failed");
        return false;
    }

    // RGA dst: BGRA8888 (匹配 QImage::Format_RGB32 在小端机器上的字节序 B-G-R-A)
    // 注意：QImage::Format_RGB32 实际是 ARGB32（内存中 A在高字节,B在低字节），
    // 小端机器上字节序为 B,G,R,A。RK_FORMAT_BGRA_8888 字节序也是 B-G-R-A，正好匹配。
    rga_buffer_t dstBuf = wrapbuffer_virtualaddr(
        dst, width, height, RK_FORMAT_BGRA_8888);
    if (dstBuf.width == 0 && dstBuf.height == 0) {
        VLOG_WARN(MODULE, "wrapbuffer_virtualaddr(dst) failed");
        return false;
    }

    // imcvtcolor 专用于跨格式色彩转换（NV12 -> BGRA8888），不做缩放。
    // mode=IM_COLOR_SPACE_DEFAULT(=0) 对应 BT.601 limited range（IMX415 默认输出）。
    // librga2 2.2.0 的 IM_COLOR_SPACE_MODE 枚举中 DEFAULT=0、BT601_LIMIT=0<<8=0
    // 是同义值。其他可选：IM_YUV_BT601_FULL_RANGE=1<<8, IM_YUV_BT709_LIMIT_RANGE=2<<8
    // IM_YUV_BT709_FULL_RANGE=3<<8 等。如果画面发灰可尝试 4<<8(BT601_LIMIT_TO_FULL)。
    // sync=1 同步等待 RGA 完成转换。
    IM_STATUS status = imcvtcolor(srcBuf, dstBuf,
                                  RK_FORMAT_YCbCr_420_SP,
                                  RK_FORMAT_BGRA_8888,
                                  IM_COLOR_SPACE_DEFAULT,
                                  1 /* sync */);
    if (status != IM_STATUS_SUCCESS) {
        VLOG_WARN(MODULE, "imcvtcolor failed: %s", imStrError(status));
        return false;
    }
    return true;
}

#else  // !HAVE_RGA_IM2D
static bool convertWithRga(const uint8_t *, int, int, uint8_t *)
{
    return false;
}
#endif

// ---------------------------------------------------------------------------
// CPU 降级：NV12 -> RGBA8888（整数定点运算，BT.601 limited range）
// ---------------------------------------------------------------------------

static void convertWithCpu(const uint8_t *src, int width, int height,
                           uint8_t *dst)
{
    const uint8_t *yPlane  = src;
    const uint8_t *uvPlane = src + width * height;

    for (int y = 0; y < height; y++) {
        uint8_t *outRow = dst + y * width * 4;
        const uint8_t *yRow  = yPlane + y * width;
        const uint8_t *uvRow = uvPlane + (y / 2) * width;

        for (int x = 0; x < width; x++) {
            int Y = yRow[x];
            int U = uvRow[(x / 2) * 2]     - 128;
            int V = uvRow[(x / 2) * 2 + 1] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;

            uint8_t *px = outRow + x * 4;
            px[0] = static_cast<uint8_t>(B);
            px[1] = static_cast<uint8_t>(G);
            px[2] = static_cast<uint8_t>(R);
            px[3] = 0xFF;
        }
    }
}

// ---------------------------------------------------------------------------
// 统一入口
// ---------------------------------------------------------------------------

bool rga::nv12ToRgb32(const uint8_t *src, int width, int height, uint8_t *dst)
{
    if (!src || !dst || width <= 0 || height <= 0) {
        return false;
    }

    // 优先 RGA 硬件加速
    if (g_rgaAvailable) {
        if (convertWithRga(src, width, height, dst)) {
            return true;
        }
        // 单次失败后不立即禁用，下一帧仍可重试（可能是临时对齐问题）
        VLOG_WARN(MODULE, "RGA conversion failed, falling back to CPU");
    }

    // CPU 降级
    convertWithCpu(src, width, height, dst);
    return true;
}
