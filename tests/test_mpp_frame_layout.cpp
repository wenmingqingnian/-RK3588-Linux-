#include "../media/mpp_frame_layout.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void testAlignedLayout()
{
    const auto layout = mpp::frame_layout::Layout::forI420(18, 18, 16);

    assert(layout.width == 18);
    assert(layout.height == 18);
    assert(layout.horizontalStride == 32);
    assert(layout.verticalStride == 32);
    assert(layout.yOffset == 0);
    assert(layout.uOffset == 32U * 32U);
    assert(layout.vOffset == layout.uOffset + (32U * 32U) / 4U);
    assert(layout.bufferSize == 32U * 32U * 3U / 2U);
}

void testPlaneCopyAndPadding()
{
    constexpr std::size_t width = 18;
    constexpr std::size_t height = 18;
    const auto layout = mpp::frame_layout::Layout::forI420(width, height, 16);

    const std::size_t ySize = width * height;
    const std::size_t uvSize = (width / 2U) * (height / 2U);
    std::vector<std::uint8_t> source(ySize + uvSize * 2U);

    std::fill(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(ySize), 0x11);
    std::fill(source.begin() + static_cast<std::ptrdiff_t>(ySize),
              source.begin() + static_cast<std::ptrdiff_t>(ySize + uvSize), 0x22);
    std::fill(source.begin() + static_cast<std::ptrdiff_t>(ySize + uvSize),
              source.end(), 0x33);

    std::vector<std::uint8_t> destination(layout.bufferSize, 0xEE);
    const bool copied = mpp::frame_layout::copyI420ToAligned(
        source.data(), source.size(), destination.data(), destination.size(), layout);

    assert(copied);

    for (std::size_t row = 0; row < height; ++row) {
        const auto *line = destination.data() + row * layout.horizontalStride;
        assert(std::all_of(line, line + width,
                           [](std::uint8_t value) { return value == 0x11; }));
        assert(std::all_of(line + width, line + layout.horizontalStride,
                           [](std::uint8_t value) { return value == 0; }));
    }

    const std::size_t chromaWidth = width / 2U;
    const std::size_t chromaHeight = height / 2U;
    const std::size_t chromaStride = layout.horizontalStride / 2U;
    for (std::size_t row = 0; row < chromaHeight; ++row) {
        const auto *uLine = destination.data() + layout.uOffset + row * chromaStride;
        const auto *vLine = destination.data() + layout.vOffset + row * chromaStride;
        assert(std::all_of(uLine, uLine + chromaWidth,
                           [](std::uint8_t value) { return value == 0x22; }));
        assert(std::all_of(vLine, vLine + chromaWidth,
                           [](std::uint8_t value) { return value == 0x33; }));
        assert(std::all_of(uLine + chromaWidth, uLine + chromaStride,
                           [](std::uint8_t value) { return value == 0; }));
        assert(std::all_of(vLine + chromaWidth, vLine + chromaStride,
                           [](std::uint8_t value) { return value == 0; }));
    }
}

void testRejectsInvalidInput()
{
    const auto invalid = mpp::frame_layout::Layout::forI420(17, 18, 16);
    assert(!invalid.valid());

    const auto layout = mpp::frame_layout::Layout::forI420(18, 18, 16);
    std::vector<std::uint8_t> source(18U * 18U * 3U / 2U);
    std::vector<std::uint8_t> destination(layout.bufferSize);

    assert(!mpp::frame_layout::copyI420ToAligned(
        source.data(), source.size() - 1U,
        destination.data(), destination.size(), layout));
    assert(!mpp::frame_layout::copyI420ToAligned(
        source.data(), source.size(),
        destination.data(), destination.size() - 1U, layout));
}

} // namespace

int main()
{
    testAlignedLayout();
    testPlaneCopyAndPadding();
    testRejectsInvalidInput();
    std::puts("mpp_frame_layout tests passed");
    return 0;
}
