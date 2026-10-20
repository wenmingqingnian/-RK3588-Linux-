#ifndef MPP_ABI_CONSTANTS_H
#define MPP_ABI_CONSTANTS_H

namespace mpp {
namespace abi {

// Values from the Rockchip MPP headers shipped with project1 and the RK3588S
// board SDK. Keep the host test in sync when the board MPP ABI is upgraded.
constexpr int kContextDecoder = 0;
constexpr int kContextEncoder = 1;
constexpr int kErrorTimeout = -8;
constexpr int kCodingAvc = 7;
constexpr int kCodingHevc = 0x01000004;
constexpr int kCodingMjpeg = 5;
constexpr int kFormatYuv420Sp = 0;
constexpr int kFormatYuv420P = 4;
constexpr int kRateControlVbr = 0;

constexpr int kEncoderSetConfig = 0x00320001;
constexpr int kEncoderGetConfig = 0x00320002;
// MPP_ENC_SET_IDR_FRAME_REQ: force the next encoded frame to be an IDR frame.
// Used by the RTSP server when a PLAY request arrives so the new client can
// decode immediately without waiting for the next GOP boundary.
constexpr int kEncoderSetIdrFrameReq = 0x00320003;
constexpr int kEncoderGetExtraInfo = 0x0032000E;

// Decoder control command (from rk_mpi_cmd.h, CMD_MODULE_CODEC=0x00300000 |
// CMD_CTX_ID_DEC=0x00010000, base 0x00310000). Used to push frame info into
// the decoder before the first decode call.
constexpr int kDecoderSetFrameInfo = 0x02000002;

constexpr int kBufferTypeIon = 1;
constexpr int kBufferModeInternal = 0;

} // namespace abi
} // namespace mpp

#endif // MPP_ABI_CONSTANTS_H
