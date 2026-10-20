// dlfcn-backed Rockchip MPP decoder mock.
//
// This mock behaves like a tiny librockchip_mpp.so for host tests. It models
// the important ownership rule: mpp_frame_get_buffer(frame) returns an
// MppBuffer handle, not a raw pixel pointer. The caller must pass that handle
// to mpp_buffer_get_ptr_with_caller() before copying frame bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../media/mpp_api_types.h"

extern "C" {

namespace {

struct FakeCtx {
    int marker;
};

struct FakePacket {
    const void *data;
    size_t size;
};

struct FakeFrame {
    int marker;
};

FakeCtx g_ctx{0x1234};
FakePacket g_packet{nullptr, 0};
FakeFrame g_frame{0x5678};

// 2x2 NV12 with width=2, height=2, horizontal stride=4, vertical stride=2.
// Y rows:
//   10 20 pad pad
//   30 40 pad pad
// UV row at offset hstride*vstride=8:
//   128 128 pad pad
uint8_t g_frameData[] = {
    10, 20, 0xA0, 0xA1,
    30, 40, 0xA2, 0xA3,
    128, 128, 0xB0, 0xB1
};

// This is deliberately not pixel data. Old decoder code that treats the
// MppBuffer handle as a data pointer will copy these bytes and fail the test.
uint8_t g_bufferHandleStorage[sizeof(g_frameData)] = {
    0xEE, 0xEE, 0xEE, 0xEE,
    0xEE, 0xEE, 0xEE, 0xEE,
    0xEE, 0xEE, 0xEE, 0xEE
};

int g_syncBeginCount = 0;
int g_syncEndCount = 0;

MppRet fake_decode(MppCtx, MppPacket, MppFrame *frame)
{
    *frame = &g_frame;
    return 0;
}

MppRet fake_decode_put_packet(MppCtx, MppPacket)
{
    return 0;
}

MppRet fake_decode_get_frame(MppCtx, MppFrame *frame)
{
    *frame = &g_frame;
    return 0;
}

MppApi g_api = {
    sizeof(MppApi),
    1,
    fake_decode,
    fake_decode_put_packet,
    fake_decode_get_frame,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    {0}
};

void *symbolFor(const char *symbol)
{
    if (std::strcmp(symbol, "mpp_create") == 0) {
        return reinterpret_cast<void *>(
            +[](MppCtx *ctx, MppApi **api) -> MppRet {
                *ctx = &g_ctx;
                *api = &g_api;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_init") == 0) {
        return reinterpret_cast<void *>(
            +[](MppCtx, int, int) -> MppRet { return 0; });
    }
    if (std::strcmp(symbol, "mpp_destroy") == 0) {
        return reinterpret_cast<void *>(
            +[](MppCtx) -> MppRet { return 0; });
    }
    if (std::strcmp(symbol, "mpp_packet_init") == 0) {
        return reinterpret_cast<void *>(
            +[](MppPacket *packet, const void *data, size_t size) -> MppRet {
                g_packet.data = data;
                g_packet.size = size;
                *packet = &g_packet;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_packet_deinit") == 0) {
        return reinterpret_cast<void *>(
            +[](MppPacket *packet) -> MppRet {
                *packet = nullptr;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_frame_init") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame *frame) -> MppRet {
                *frame = &g_frame;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_frame_deinit") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame *frame) -> MppRet {
                *frame = nullptr;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_frame_get_width") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> uint32_t { return 2; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_height") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> uint32_t { return 2; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_hor_stride") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> uint32_t { return 4; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_ver_stride") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> uint32_t { return 2; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_fmt") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> int { return 0; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_errinfo") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> int { return 0; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_discard") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> int { return 0; });
    }
    if (std::strcmp(symbol, "mpp_frame_get_buffer") == 0) {
        return reinterpret_cast<void *>(
            +[](MppFrame) -> MppBuffer {
                return static_cast<MppBuffer>(g_bufferHandleStorage);
            });
    }
    if (std::strcmp(symbol, "mpp_buffer_get_ptr_with_caller") == 0) {
        return reinterpret_cast<void *>(
            +[](MppBuffer buffer, const char *) -> void * {
                return buffer == static_cast<MppBuffer>(g_bufferHandleStorage)
                    ? static_cast<void *>(g_frameData)
                    : nullptr;
            });
    }
    if (std::strcmp(symbol, "mpp_buffer_sync_begin_f") == 0) {
        return reinterpret_cast<void *>(
            +[](MppBuffer, int, const char *) -> MppRet {
                ++g_syncBeginCount;
                return 0;
            });
    }
    if (std::strcmp(symbol, "mpp_buffer_sync_end_f") == 0) {
        return reinterpret_cast<void *>(
            +[](MppBuffer, int, const char *) -> MppRet {
                ++g_syncEndCount;
                return 0;
            });
    }
    return nullptr;
}

} // namespace

void *dlopen(const char *, int)
{
    return &g_ctx;
}

void *dlsym(void *, const char *symbol)
{
    return symbolFor(symbol);
}

int dlclose(void *)
{
    return 0;
}

char *dlerror()
{
    static char message[] = "mock symbol not found";
    return message;
}

void mpp_mock_reset_sync_counts()
{
    g_syncBeginCount = 0;
    g_syncEndCount = 0;
}

int mpp_mock_sync_begin_count()
{
    return g_syncBeginCount;
}

int mpp_mock_sync_end_count()
{
    return g_syncEndCount;
}

} // extern "C"
