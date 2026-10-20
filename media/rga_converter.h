#ifndef RGA_CONVERTER_H
#define RGA_CONVERTER_H

#include <cstdint>

namespace rga {

/**
 * @brief 初始化RGA硬件加速
 *
 * 板端存在 /usr/include/rga/im2d.h 时链接 librga2 (im2d API)；
 * 否则降级为 CPU 整数运算。必须在 nv12ToRgb32() 前调用一次。
 */
void init();

/**
 * @brief 释放RGA资源
 */
void deinit();

/**
 * @brief 查询RGA是否可用
 */
bool isAvailable();

/**
 * @brief NV12转RGB32
 *
 * 优先使用RGA硬件加速（im2d imcvtcolor_t / imcopy），失败则降级CPU。
 *
 * @param src      NV12数据指针（Y+UV连续布局）
 * @param width    图像宽度
 * @param height   图像高度
 * @param dst      输出RGB32缓冲区，调用者保证 >= width*height*4 字节
 * @return true=转换成功，false=失败
 */
bool nv12ToRgb32(const uint8_t *src, int width, int height, uint8_t *dst);

} // namespace rga

#endif // RGA_CONVERTER_H
