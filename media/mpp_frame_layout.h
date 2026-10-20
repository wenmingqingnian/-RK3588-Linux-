#ifndef MPP_FRAME_LAYOUT_H
#define MPP_FRAME_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mpp {
namespace frame_layout {

inline std::size_t alignTo(std::size_t value, std::size_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0)
        return 0;

    const std::size_t padding = alignment - 1U;
    if (value > std::numeric_limits<std::size_t>::max() - padding)
        return 0;

    return (value + padding) & ~padding;
}

struct Layout {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t horizontalStride = 0;
    std::size_t verticalStride = 0;
    std::size_t yOffset = 0;
    std::size_t uOffset = 0;
    std::size_t vOffset = 0;
    std::size_t bufferSize = 0;

    bool valid() const
    {
        return width > 0 && height > 0 &&
               (width % 2U) == 0 && (height % 2U) == 0 &&
               horizontalStride >= width && verticalStride >= height &&
               (horizontalStride % 2U) == 0 &&
               (verticalStride % 2U) == 0 &&
               uOffset > yOffset && vOffset > uOffset &&
               bufferSize > vOffset;
    }

    static Layout forI420(std::size_t frameWidth,
                          std::size_t frameHeight,
                          std::size_t alignment)
    {
        Layout layout;
        if (frameWidth == 0 || frameHeight == 0 ||
            (frameWidth % 2U) != 0 || (frameHeight % 2U) != 0) {
            return layout;
        }

        const std::size_t horizontal = alignTo(frameWidth, alignment);
        const std::size_t vertical = alignTo(frameHeight, alignment);
        if (horizontal == 0 || vertical == 0 ||
            horizontal > std::numeric_limits<std::size_t>::max() / vertical) {
            return layout;
        }

        const std::size_t lumaSize = horizontal * vertical;
        const std::size_t chromaSize = (horizontal / 2U) * (vertical / 2U);
        if (chromaSize > (std::numeric_limits<std::size_t>::max() - lumaSize) / 2U)
            return layout;

        layout.width = frameWidth;
        layout.height = frameHeight;
        layout.horizontalStride = horizontal;
        layout.verticalStride = vertical;
        layout.yOffset = 0;
        layout.uOffset = lumaSize;
        layout.vOffset = lumaSize + chromaSize;
        layout.bufferSize = lumaSize + chromaSize * 2U;
        return layout;
    }
};

inline bool copyI420ToAligned(const std::uint8_t *source,
                              std::size_t sourceSize,
                              std::uint8_t *destination,
                              std::size_t destinationSize,
                              const Layout &layout)
{
    if (!source || !destination || !layout.valid())
        return false;

    if (layout.width > std::numeric_limits<std::size_t>::max() / layout.height)
        return false;

    const std::size_t sourceYSize = layout.width * layout.height;
    const std::size_t sourceChromaSize =
        (layout.width / 2U) * (layout.height / 2U);
    if (sourceChromaSize >
        (std::numeric_limits<std::size_t>::max() - sourceYSize) / 2U) {
        return false;
    }

    const std::size_t requiredSourceSize =
        sourceYSize + sourceChromaSize * 2U;
    if (sourceSize < requiredSourceSize ||
        destinationSize < layout.bufferSize) {
        return false;
    }

    std::memset(destination, 0, layout.bufferSize);

    for (std::size_t row = 0; row < layout.height; ++row) {
        std::memcpy(destination + layout.yOffset +
                        row * layout.horizontalStride,
                    source + row * layout.width,
                    layout.width);
    }

    const std::uint8_t *sourceU = source + sourceYSize;
    const std::uint8_t *sourceV = sourceU + sourceChromaSize;
    const std::size_t chromaWidth = layout.width / 2U;
    const std::size_t chromaHeight = layout.height / 2U;
    const std::size_t chromaStride = layout.horizontalStride / 2U;

    for (std::size_t row = 0; row < chromaHeight; ++row) {
        std::memcpy(destination + layout.uOffset + row * chromaStride,
                    sourceU + row * chromaWidth, chromaWidth);
        std::memcpy(destination + layout.vOffset + row * chromaStride,
                    sourceV + row * chromaWidth, chromaWidth);
    }

    return true;
}

} // namespace frame_layout
} // namespace mpp

#endif // MPP_FRAME_LAYOUT_H
