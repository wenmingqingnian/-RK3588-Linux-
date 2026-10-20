// Minimal embedded RTSP server implementation.
//
// Provides RTSP control protocol (TCP) + RTP H.264/H.265 data (UDP).
// Uses MPP hardware encoder (primary) or FFmpeg libx264 (fallback).
// Uses FFmpeg libswscale for RGB32→YUV420P color conversion (both paths).
// Compatible with VLC, FFplay, and other standard RTSP clients.
//
// Lifecycle: init() -> start() -> pushFrame() -> stop() -> deinit()

#include "rtsp_server.h"
#include "mpp_encoder.h"
#include "logmanager.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>
#include <algorithm>

static const char *MODULE = "rtsp";

// Maximum RTP payload: 1500 MTU - 20 IP - 8 UDP - 12 RTP = 1460.
static constexpr size_t RTP_MAX_PAYLOAD = 1400;

// H.264 NAL unit types (RFC 6184 Section 5.2).
static constexpr uint8_t NAL_TYPE_SPS  = 7;
static constexpr uint8_t NAL_TYPE_PPS  = 8;
static constexpr uint8_t NAL_TYPE_IDR  = 5;
static constexpr uint8_t NAL_TYPE_FU_A = 28;

// H.265 NAL unit types (HEVC spec Section 7.4.2.2).
// NAL header is 2 bytes; type = (byte0 >> 1) & 0x3F.
static constexpr uint8_t HEVC_NAL_VPS   = 32;
static constexpr uint8_t HEVC_NAL_SPS   = 33;
static constexpr uint8_t HEVC_NAL_PPS   = 34;
static constexpr uint8_t HEVC_NAL_IDR_W = 19;  // IDR with RADL
static constexpr uint8_t HEVC_NAL_IDR_N = 20;  // IDR without RADL
static constexpr uint8_t HEVC_NAL_CRA   = 21;  // CRA (clean random access)

// RTP payload type for H.264 (dynamic range).
static constexpr uint8_t RTP_PT_H264 = 96;
// RTP payload type for H.265 (dynamic range, distinct from H.264).
static constexpr uint8_t RTP_PT_H265 = 97;
// H.265 FU payload type (RFC 7798 Section 4.4.3).
static constexpr uint8_t HEVC_NAL_FU = 49;

// RTP clock rate for H.264.
static constexpr uint32_t RTP_CLOCK_RATE = 90000;

// Maximum concurrent client threads to prevent resource exhaustion.
static constexpr int MAX_CLIENT_THREADS = 8;

namespace media {

// ---------------------------------------------------------------------------
// rtspStatusToString: convert RtspStatus to human-readable string
// ---------------------------------------------------------------------------

const char *rtspStatusToString(RtspStatus status) {
    switch (status) {
    case RtspStatus::Success:           return "success";
    case RtspStatus::Disabled:          return "disabled in config";
    case RtspStatus::EncoderNotFound:   return "libx264 encoder not found";
    case RtspStatus::CodecInitFailed:   return "codec context alloc failed";
    case RtspStatus::FrameAllocFailed:  return "frame/packet alloc failed";
    case RtspStatus::SwscaleInitFailed: return "swscale context init failed";
    case RtspStatus::SocketCreateFailed:return "socket creation failed";
    case RtspStatus::BindFailed:        return "bind failed";
    case RtspStatus::ListenFailed:      return "listen failed";
    case RtspStatus::AlreadyRunning:    return "already running";
    case RtspStatus::NotInitialized:    return "not initialized";
    case RtspStatus::InvalidConfig:     return "invalid config";
    default:                            return "unknown error";
    }
}

// ---------------------------------------------------------------------------
// Base64 encoding helper (for SDP sprop-parameter-sets)
// ---------------------------------------------------------------------------

static const char s_base64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t *data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t offset = 0; offset < len; offset += 3) {
        uint32_t triplet = static_cast<uint32_t>(data[offset]) << 16;
        if (offset + 1 < len) {
            triplet |= static_cast<uint32_t>(data[offset + 1]) << 8;
        }
        if (offset + 2 < len) {
            triplet |= static_cast<uint32_t>(data[offset + 2]);
        }
        result += s_base64Table[(triplet >> 18) & 0x3F];
        result += s_base64Table[(triplet >> 12) & 0x3F];
        result += (offset + 1 < len) ? s_base64Table[(triplet >> 6) & 0x3F] : '=';
        result += (offset + 2 < len) ? s_base64Table[triplet & 0x3F] : '=';
    }
    return result;
}

// ---------------------------------------------------------------------------
// Detect server IP (first non-loopback IPv4 address)
// ---------------------------------------------------------------------------

static std::string detectServerIp() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return "0.0.0.0";
    }

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addrResult = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &addrResult) != 0 ||
        !addrResult) {
        return "0.0.0.0";
    }

    char ipText[INET_ADDRSTRLEN];
    auto *sockAddr = reinterpret_cast<struct sockaddr_in *>(
        addrResult->ai_addr);
    inet_ntop(AF_INET, &sockAddr->sin_addr, ipText, sizeof(ipText));
    freeaddrinfo(addrResult);

    std::string detectedIp(ipText);
    if (detectedIp == "127.0.0.1" || detectedIp == "127.0.1.1") {
        return "0.0.0.0";
    }
    return detectedIp;
}

// ---------------------------------------------------------------------------
// Thread-safe random number generation (replaces non-thread-safe rand/srand)
// ---------------------------------------------------------------------------

static std::mt19937 &threadRng() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return rng;
}

// ---------------------------------------------------------------------------
// Generate a random RTSP session ID
// ---------------------------------------------------------------------------

static std::string generateSessionId() {
    std::uniform_int_distribution<unsigned int> dist(0, 0xFFFFFFFFu);
    char sessionIdBuf[16];
    snprintf(sessionIdBuf, sizeof(sessionIdBuf), "%08x", dist(threadRng()));
    return std::string(sessionIdBuf);
}

// ---------------------------------------------------------------------------
// Parse RTSP CSeq header value
// ---------------------------------------------------------------------------

static int parseCSeqValue(const std::string &request) {
    auto pos = request.find("CSeq:");
    if (pos == std::string::npos) {
        pos = request.find("cseq:");
    }
    if (pos == std::string::npos) {
        return 0;
    }
    pos += 5;
    while (pos < request.size() && request[pos] == ' ') {
        pos++;
    }
    return std::atoi(request.c_str() + pos);
}

// ---------------------------------------------------------------------------
// Parse RTSP Transport header: extract client_port value
// ---------------------------------------------------------------------------

static void parseClientPort(const std::string &transport,
                            int &rtpPort, int &rtcpPort) {
    rtpPort = 0;
    rtcpPort = 0;

    auto portPos = transport.find("client_port=");
    if (portPos == std::string::npos) {
        return;
    }
    portPos += 12;
    auto endPos = transport.find(';', portPos);
    if (endPos == std::string::npos) {
        endPos = transport.size();
    }
    std::string portStr = transport.substr(portPos, endPos - portPos);
    auto dashPos = portStr.find('-');
    if (dashPos != std::string::npos) {
        rtpPort = std::atoi(portStr.substr(0, dashPos).c_str());
        rtcpPort = std::atoi(portStr.substr(dashPos + 1).c_str());
    } else {
        rtpPort = std::atoi(portStr.c_str());
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

RtspServer::RtspServer() = default;

RtspServer::~RtspServer() {
    deinit();
}

// ---------------------------------------------------------------------------
// init: allocate encoder, swscale, sockets (does NOT start server thread)
// ---------------------------------------------------------------------------

RtspStatus RtspServer::init(const RtspServerConfig &config) {
    if (m_initialized) {
        VLOG_WARN(MODULE, "RTSP server already initialized");
        return RtspStatus::Success;
    }

    if (!config.enabled) {
        VLOG_INFO(MODULE, "RTSP server disabled in config");
        return RtspStatus::Disabled;
    }

    std::string configError;
    if (!validateRtspServerConfig(config, &configError)) {
        VLOG_ERROR(MODULE, "RTSP config rejected: %s", configError.c_str());
        return RtspStatus::InvalidConfig;
    }

    m_config = config;
    m_serverIp = detectServerIp();

    // Step 1: Initialize encoder (MPP hardware primary, libx264 fallback).
    RtspStatus status = initEncoder(config);
    if (status != RtspStatus::Success) {
        return status;
    }

    // Pre-allocate MPP encode output buffer (1MB per frame).
    if (m_useMpp) {
        m_mppEncBuffer.resize(1024 * 1024);
    }

    // Step 2: Extract SPS/PPS for SDP generation.
    // For MPP path, this is deferred until after the first frame is encoded
    // because MPP_ENC_GET_EXTRA_INFO requires at least one encoded frame.
    // For FFmpeg path, extract immediately (libx264 generates headers at init).
    if (!m_useMpp) {
        extractSpsPps();
        if (m_sps.empty() || m_pps.empty()) {
            VLOG_WARN(MODULE, "SPS/PPS not extracted; VLC may not connect");
        } else {
            VLOG_INFO(MODULE,
                      "SPS/PPS extracted: SPS=%llu bytes, PPS=%llu bytes",
                      static_cast<unsigned long long>(m_sps.size()),
                      static_cast<unsigned long long>(m_pps.size()));
        }
    } else {
        VLOG_INFO(MODULE, "MPP mode: SPS/PPS will be extracted after first frame");
    }

    // Step 3: Allocate reusable frame and packet.
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        VLOG_ERROR(MODULE, "failed to allocate AVFrame/AVPacket");
        deinit();
        return RtspStatus::FrameAllocFailed;
    }

    // Step 4: Initialize swscale for RGB32 -> YUV420P conversion.
    status = initSwscale(config);
    if (status != RtspStatus::Success) {
        return status;
    }

    // Step 5: Create sockets.
    status = initSockets(config);
    if (status != RtspStatus::Success) {
        return status;
    }

    // Step 6: Initialize MP4 recorder (if enabled in recording sub-config).
    // Inherit width/height/fps/codec from the parent RTSP config so the MP4
    // muxer exactly matches the encoded byte stream. Recorder init creates
    // the output directory but does not open any file until startRecording()
    // is called. Recorder init failure is non-fatal — RTSP continues.
    {
        RecordingConfig recCfg = config.recording;
        recCfg.width = config.width;
        recCfg.height = config.height;
        recCfg.fps = config.fps;
        recCfg.codec = config.codec;
        m_config.recording = recCfg;  // persist inherited values
        RecorderStatus recStatus = m_recorder.init(recCfg);
        if (recStatus == RecorderStatus::Success) {
            VLOG_INFO(MODULE, "MP4 recorder initialized (idle until startRecording)");
        } else if (recStatus == RecorderStatus::Disabled) {
            VLOG_INFO(MODULE, "MP4 recorder disabled in config");
        } else {
            VLOG_WARN(MODULE, "MP4 recorder init failed: %s (non-fatal)",
                      recorderStatusToString(recStatus));
        }
    }

    m_initialized = true;
    VLOG_INFO(MODULE, "RTSP server initialized: rtsp://%s:%d%s %dx%d@%dfps",
              m_serverIp.c_str(), config.port, config.streamPath.c_str(),
              config.width, config.height, config.fps);
    return RtspStatus::Success;
}

// ---------------------------------------------------------------------------
// initEncoder: initialize MPP hardware encoder (primary) or libx264 (fallback)
// ---------------------------------------------------------------------------

RtspStatus RtspServer::initEncoder(const RtspServerConfig &config) {
    // Try MPP hardware encoder first (RK3588S VPU).
    if (mpp::isAvailable()) {
        mpp::EncoderConfig encCfg;
        encCfg.codec = (config.codec == "265") ?
                       mpp::CodecType::H265 : mpp::CodecType::H264;
        encCfg.width = config.width;
        encCfg.height = config.height;
        encCfg.fps = config.fps;
        encCfg.bitrate = config.bitrate;
        encCfg.gop = config.gopSize;

        if (mpp::initEncoder(encCfg)) {
            m_useMpp = true;
            VLOG_INFO(MODULE, "using MPP hardware %s encoder",
                      (config.codec == "265") ? "H.265" : "H.264");
            return RtspStatus::Success;
        }
        VLOG_WARN(MODULE, "MPP encoder init failed, falling back to libx264");
    } else {
        VLOG_INFO(MODULE, "MPP not available, using libx264 software encoder");
    }

    // Fallback: FFmpeg libx264 software encoder.
    m_useMpp = false;
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        VLOG_ERROR(MODULE, "libx264 not found; "
                   "install libavcodec-extra or recompile FFmpeg");
        return RtspStatus::EncoderNotFound;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        VLOG_ERROR(MODULE, "failed to allocate AVCodecContext");
        return RtspStatus::CodecInitFailed;
    }

    m_codecCtx->codec_id = AV_CODEC_ID_H264;
    m_codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_codecCtx->width = config.width;
    m_codecCtx->height = config.height;
    m_codecCtx->bit_rate = config.bitrate;
    m_codecCtx->time_base = {1, config.fps};
    m_codecCtx->framerate = {config.fps, 1};
    m_codecCtx->gop_size = config.gopSize;
    m_codecCtx->max_b_frames = 0;

    // Low-latency preset for real-time streaming.
    av_opt_set(m_codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_codecCtx->priv_data, "profile", "baseline", 0);

    // Global header: SPS/PPS stored in extradata for SDP extraction.
    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        VLOG_ERROR(MODULE, "avcodec_open2 failed: %d", ret);
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        return RtspStatus::CodecInitFailed;
    }

    return RtspStatus::Success;
}

// ---------------------------------------------------------------------------
// initSwscale: create RGB32 -> YUV420P conversion context
// ---------------------------------------------------------------------------

RtspStatus RtspServer::initSwscale(const RtspServerConfig &config) {
    m_swsCtx = sws_getContext(
        config.width, config.height, AV_PIX_FMT_RGB32,
        config.width, config.height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        VLOG_ERROR(MODULE, "sws_getContext failed");
        deinit();
        return RtspStatus::SwscaleInitFailed;
    }

    // Pre-allocate YUV420P buffer.
    int yuvSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                           config.width, config.height, 1);
    m_yuvBuffer.resize(yuvSize);
    return RtspStatus::Success;
}

// ---------------------------------------------------------------------------
// initSockets: create UDP (RTP) and TCP (RTSP) sockets
// ---------------------------------------------------------------------------

RtspStatus RtspServer::initSockets(const RtspServerConfig &config) {
    // UDP socket for RTP data packets.
    m_rtpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_rtpFd < 0) {
        VLOG_ERROR(MODULE, "RTP UDP socket creation failed: %s",
                   strerror(errno));
        deinit();
        return RtspStatus::SocketCreateFailed;
    }

    // TCP socket for RTSP control protocol.
    m_tcpFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_tcpFd < 0) {
        VLOG_ERROR(MODULE, "RTSP TCP socket creation failed: %s",
                   strerror(errno));
        deinit();
        return RtspStatus::SocketCreateFailed;
    }

    int reuseAddr = 1;
    if (setsockopt(m_tcpFd, SOL_SOCKET, SO_REUSEADDR,
                   &reuseAddr, sizeof(reuseAddr)) < 0) {
        VLOG_WARN(MODULE, "SO_REUSEADDR failed: %s", strerror(errno));
    }

    struct sockaddr_in bindAddr {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(static_cast<uint16_t>(config.port));

    if (bind(m_tcpFd, reinterpret_cast<struct sockaddr *>(&bindAddr),
             sizeof(bindAddr)) < 0) {
        VLOG_ERROR(MODULE, "bind port %d failed: %s",
                   config.port, strerror(errno));
        deinit();
        return RtspStatus::BindFailed;
    }

    if (listen(m_tcpFd, config.maxClients) < 0) {
        VLOG_ERROR(MODULE, "listen failed: %s", strerror(errno));
        deinit();
        return RtspStatus::ListenFailed;
    }

    return RtspStatus::Success;
}

// ---------------------------------------------------------------------------
// extractSpsPps: dispatch to MPP header or AVCC/Annex-B parser
// ---------------------------------------------------------------------------

void RtspServer::extractSpsPps() {
    // MPP path: get SPS/PPS via MPP_ENC_GET_EXTRA_INFO.
    if (m_useMpp) {
        uint8_t headerBuf[256];
        size_t headerSize = 0;
        if (mpp::getHeader(headerBuf, headerSize, sizeof(headerBuf)) &&
            headerSize > 0) {
            extractSpsPpsFromMppHeader(headerBuf, headerSize);
        }
        return;
    }

    // FFmpeg path: parse extradata.
    if (!m_codecCtx || !m_codecCtx->extradata ||
        m_codecCtx->extradata_size <= 0) {
        return;
    }
    extractSpsPpsFromAvcc();
    if (m_sps.empty()) {
        extractSpsPpsFromAnnexB();
    }
}

// ---------------------------------------------------------------------------
// extractSpsPpsFromMppHeader: parse Annex-B SPS/PPS from MPP getHeader()
// ---------------------------------------------------------------------------

void RtspServer::extractSpsPpsFromMppHeader(const uint8_t *headerData,
                                             size_t headerSize) {
    // MPP header is Annex-B format:
    //   H.264: [00 00 00 01 SPS] [00 00 00 01 PPS]
    //   H.265: [00 00 00 01 VPS] [00 00 00 01 SPS] [00 00 00 01 PPS]
    bool isH265 = mpp::isH265();

    // H.265 NAL header is 2 bytes, so we need i+5 to be valid.
    // H.264 NAL header is 1 byte, so we need i+4 to be valid.
    size_t minLen = isH265 ? 5 : 4;
    for (size_t i = 0; i + minLen < headerSize; i++) {
        if (headerData[i] != 0 || headerData[i + 1] != 0 ||
            headerData[i + 2] != 0 || headerData[i + 3] != 1) {
            continue;
        }

        // Extract NAL type: H.264 = byte & 0x1F, H.265 = (byte >> 1) & 0x3F.
        uint8_t nalType;
        size_t nalStart;
        if (isH265) {
            nalType = (headerData[i + 4] >> 1) & 0x3F;
            nalStart = i + 5;  // H.265 NAL header is 2 bytes.
        } else {
            nalType = headerData[i + 4] & 0x1F;
            nalStart = i + 4;  // H.264 NAL header is 1 byte.
        }

        // Find end of this NAL (next start code or end of data).
        size_t nalEnd = nalStart;
        for (size_t j = nalStart + 1; j + 3 < headerSize; j++) {
            if (headerData[j] == 0 && headerData[j + 1] == 0 &&
                headerData[j + 2] == 0 && headerData[j + 3] == 1) {
                nalEnd = j;
                break;
            }
        }
        if (nalEnd <= nalStart) {
            nalEnd = headerSize;
        }

        if (!isH265) {
            // H.264: extract SPS (type 7) and PPS (type 8).
            if (nalType == NAL_TYPE_SPS) {
                m_sps.assign(headerData + nalStart, headerData + nalEnd);
                m_spsBase64 = base64Encode(m_sps.data(), m_sps.size());
            } else if (nalType == NAL_TYPE_PPS) {
                m_pps.assign(headerData + nalStart, headerData + nalEnd);
                m_ppsBase64 = base64Encode(m_pps.data(), m_pps.size());
            }
        } else {
            // H.265: extract VPS (type 32), SPS (type 33), PPS (type 34).
            // VPS 是 HEVC 解码器初始化必需的(RFC 7798 SDP sprop-vps)。
            if (nalType == HEVC_NAL_VPS) {
                m_vps.assign(headerData + nalStart, headerData + nalEnd);
                m_vpsBase64 = base64Encode(m_vps.data(), m_vps.size());
            } else if (nalType == HEVC_NAL_SPS) {
                m_sps.assign(headerData + nalStart, headerData + nalEnd);
                m_spsBase64 = base64Encode(m_sps.data(), m_sps.size());
            } else if (nalType == HEVC_NAL_PPS) {
                m_pps.assign(headerData + nalStart, headerData + nalEnd);
                m_ppsBase64 = base64Encode(m_pps.data(), m_pps.size());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// extractSpsPpsFromAvcc: parse AVCC-format extradata
// ---------------------------------------------------------------------------

void RtspServer::extractSpsPpsFromAvcc() {
    const uint8_t *extra = m_codecCtx->extradata;
    int extraLen = m_codecCtx->extradata_size;

    // AVCC format: skip 4 bytes header, then read NALU length size.
    int pos = 4;
    if (pos >= extraLen) {
        return;
    }
    pos++; // Skip nalu_length_size_minus1 byte.

    // Read SPS.
    // Byte at pos: 0b111(numSPS) — reserved bits=0b111, low 5 bits = numSPS.
    // libx264 outputs 0xE1 → numSPS=1. Must check > 0, not == 0.
    if (pos < extraLen && (extra[pos] & 0x1F) > 0) {
        pos++; // Skip num_sps byte.
        if (pos + 2 <= extraLen) {
            int spsLen = (extra[pos] << 8) | extra[pos + 1];
            pos += 2;
            if (pos + spsLen <= extraLen) {
                m_sps.assign(extra + pos, extra + pos + spsLen);
                m_spsBase64 = base64Encode(m_sps.data(), m_sps.size());
                pos += spsLen;
            }
        }
    }

    // Read PPS.
    if (pos < extraLen) {
        pos++; // Skip num_pps byte.
        if (pos + 2 <= extraLen) {
            int ppsLen = (extra[pos] << 8) | extra[pos + 1];
            pos += 2;
            if (pos + ppsLen <= extraLen) {
                m_pps.assign(extra + pos, extra + pos + ppsLen);
                m_ppsBase64 = base64Encode(m_pps.data(), m_pps.size());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// extractSpsPpsFromAnnexB: scan for start-code-delimited SPS/PPS NAL units
// ---------------------------------------------------------------------------

void RtspServer::extractSpsPpsFromAnnexB() {
    const uint8_t *extra = m_codecCtx->extradata;
    int extraLen = m_codecCtx->extradata_size;

    for (int i = 0; i + 4 < extraLen; i++) {
        // Look for 4-byte start code: 00 00 00 01.
        if (extra[i] != 0 || extra[i + 1] != 0 ||
            extra[i + 2] != 0 || extra[i + 3] != 1) {
            continue;
        }

        uint8_t nalType = extra[i + 4] & 0x1F;
        int nalStart = i + 4;

        // Find end of this NAL (next start code or end of data).
        int nalEnd = nalStart;
        for (int j = nalStart + 1; j + 3 < extraLen; j++) {
            if (extra[j] == 0 && extra[j + 1] == 0 &&
                extra[j + 2] == 0 && extra[j + 3] == 1) {
                nalEnd = j;
                break;
            }
        }
        if (nalEnd <= nalStart) {
            nalEnd = extraLen;
        }

        if (nalType == NAL_TYPE_SPS) {
            m_sps.assign(extra + nalStart, extra + nalEnd);
            m_spsBase64 = base64Encode(m_sps.data(), m_sps.size());
        } else if (nalType == NAL_TYPE_PPS) {
            m_pps.assign(extra + nalStart, extra + nalEnd);
            m_ppsBase64 = base64Encode(m_pps.data(), m_pps.size());
        }
    }
}

// ---------------------------------------------------------------------------
// start: launch the RTSP TCP server thread
// ---------------------------------------------------------------------------

RtspStatus RtspServer::start() {
    if (!m_initialized) {
        VLOG_ERROR(MODULE, "cannot start: not initialized");
        return RtspStatus::NotInitialized;
    }
    if (m_running) {
        VLOG_WARN(MODULE, "RTSP server already running");
        return RtspStatus::AlreadyRunning;
    }

    m_running = true;
    m_startTime = std::chrono::steady_clock::now();
    m_serverThread = std::thread(&RtspServer::serverLoop, this);
    m_encodeThread = std::thread([this]() {
        while (m_running) {
            std::unique_lock<std::mutex> lock(m_frameMutex);
            m_frameCv.wait(lock, [this]() {
                return m_hasNewFrame || !m_running;
            });
            if (!m_running) {
                break;
            }
            // Swap out the latest frame to minimize lock hold time.
            std::vector<uint8_t> frameData;
            frameData.swap(m_latestFrame);
            int w = m_latestFrameWidth;
            int h = m_latestFrameHeight;
            m_hasNewFrame = false;
            lock.unlock();

            // Encode when there are RTSP clients OR when recording is active
            // (recorder consumes the Annex-B output of encodeAndSend).
            if (!frameData.empty() && (clientCount() > 0 || m_recorder.isRunning())) {
                encodeAndSend(frameData.data(), w, h);
            }
        }
    });

    VLOG_INFO(MODULE, "RTSP server started on port %d", m_config.port);
    return RtspStatus::Success;
}

// ---------------------------------------------------------------------------
// stop: signal server thread to exit (does NOT release resources)
// ---------------------------------------------------------------------------

void RtspServer::stop() {
    if (!m_running) {
        // Even when not running, there may be leftover client threads if
        // stop() was never called after a prior start() (defensive cleanup).
        joinClientThreads();
        return;
    }

    m_running = false;

    // Wake up encode thread so it can exit.
    m_frameCv.notify_all();
    if (m_encodeThread.joinable()) {
        m_encodeThread.join();
    }

    // Shutdown all client TCP fds to unblock recv() in handleClient.
    // Without this, client threads would block up to 5s (SO_RCVTIMEO)
    // before checking m_running and exiting.
    {
        std::lock_guard<std::mutex> fdsLock(m_clientFdsMutex);
        for (int fd : m_clientFds) {
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
            }
        }
    }

    // Shutdown listen socket so serverLoop's select() returns immediately.
    if (m_tcpFd >= 0) {
        shutdown(m_tcpFd, SHUT_RDWR);
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    // Join all client threads after they exit handleClient.
    joinClientThreads();

    VLOG_DEBUG(MODULE, "RTSP server stopped");
}

// ---------------------------------------------------------------------------
// joinClientThreads: join all client threads (helper for stop/deinit)
// ---------------------------------------------------------------------------

void RtspServer::joinClientThreads() {
    std::list<ClientThread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
        threadsToJoin.swap(m_clientThreads);
    }
    for (auto &ct : threadsToJoin) {
        if (ct.thread.joinable()) {
            ct.thread.join();
        }
    }
}

// ---------------------------------------------------------------------------
// deinit: release all resources (calls stop() if still running)
// ---------------------------------------------------------------------------

void RtspServer::deinit() {
    stop();

    // Close all client TCP connections.
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto &client : m_clients) {
            if (client.tcpFd >= 0) {
                close(client.tcpFd);
                client.tcpFd = -1;
            }
        }
        m_clients.clear();
    }

    if (m_tcpFd >= 0) {
        close(m_tcpFd);
        m_tcpFd = -1;
    }
    if (m_rtpFd >= 0) {
        close(m_rtpFd);
        m_rtpFd = -1;
    }

    // Release MP4 recorder first: it holds FFmpeg muxer state and may have
    // an open file. Doing this before releasing the encoder keeps ordering
    // symmetric with init() (encoder init before recorder init; recorder
    // deinit before encoder deinit).
    m_recorder.deinit();

    // Release MPP hardware encoder (before FFmpeg cleanup).
    if (m_useMpp) {
        mpp::deinitEncoder();
        m_useMpp = false;
    }
    m_mppEncBuffer.clear();
    m_mppEncBuffer.shrink_to_fit();

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }

    m_frameCount = 0;
    m_sps.clear();
    m_pps.clear();
    m_vps.clear();
    m_spsBase64.clear();
    m_ppsBase64.clear();
    m_vpsBase64.clear();
    m_latestFrame.clear();
    m_latestFrameWidth = 0;
    m_latestFrameHeight = 0;
    m_hasNewFrame = false;
    m_initialized = false;

    // Defensive: clear client fd tracking (fds themselves are closed by
    // handleClient on exit; do NOT close again here to avoid double-close).
    {
        std::lock_guard<std::mutex> fdsLock(m_clientFdsMutex);
        m_clientFds.clear();
    }

    VLOG_DEBUG(MODULE, "RTSP server resources released");
}

// ---------------------------------------------------------------------------
// clientCount: number of clients in PLAY state
// ---------------------------------------------------------------------------

int RtspServer::clientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    int playingCount = 0;
    for (const auto &client : m_clients) {
        if (client.isPlaying) {
            playingCount++;
        }
    }
    return playingCount;
}

// ---------------------------------------------------------------------------
// getStatus: human-readable diagnostics
// ---------------------------------------------------------------------------

std::string RtspServer::getStatus() const {
    std::ostringstream oss;
    oss << "RtspServer[initialized=" << (m_initialized ? "yes" : "no")
        << ", running=" << (m_running ? "yes" : "no")
        << ", clients=" << clientCount()
        << ", frames=" << m_frameCount.load();
    if (m_initialized) {
        oss << ", url=rtsp://" << m_serverIp << ":" << m_config.port
            << m_config.streamPath;
    }
    oss << "]";
    return oss.str();
}

// ---------------------------------------------------------------------------
// extractHeaderValue: extract a header value from RTSP request text
// ---------------------------------------------------------------------------

std::string RtspServer::extractHeaderValue(
    const std::string &request, const std::string &headerName) const {
    auto pos = request.find(headerName);
    if (pos == std::string::npos) {
        // Try lowercase.
        std::string lowerName = headerName;
        for (auto &ch : lowerName) {
            ch = static_cast<char>(std::tolower(ch));
        }
        pos = request.find(lowerName);
    }
    if (pos == std::string::npos) {
        return "";
    }
    pos += headerName.size();
    auto lineEnd = request.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
        lineEnd = request.size();
    }
    std::string value = request.substr(pos, lineEnd - pos);
    // Trim leading spaces.
    auto firstNonSpace = value.find_first_not_of(' ');
    if (firstNonSpace != std::string::npos) {
        value = value.substr(firstNonSpace);
    }
    return value;
}

// ---------------------------------------------------------------------------
// serverLoop: TCP accept loop (runs in background thread)
// ---------------------------------------------------------------------------

void RtspServer::serverLoop() {
    VLOG_INFO(MODULE, "RTSP server thread started on port %d", m_config.port);

    while (m_running) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(m_tcpFd, &readFds);

        struct timeval selectTimeout {};
        selectTimeout.tv_sec = 1;
        selectTimeout.tv_usec = 0;

        int selectRet = select(m_tcpFd + 1, &readFds, nullptr,
                               nullptr, &selectTimeout);
        if (selectRet <= 0) {
            continue;
        }

        struct sockaddr_in clientAddr {};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(m_tcpFd,
                              reinterpret_cast<struct sockaddr *>(&clientAddr),
                              &addrLen);
        if (clientFd < 0) {
            if (m_running) {
                VLOG_WARN(MODULE, "accept failed: %s", strerror(errno));
            }
            continue;
        }

        // Limit concurrent client threads to prevent resource exhaustion.
        if (m_activeClientCount >= MAX_CLIENT_THREADS) {
            VLOG_WARN(MODULE, "max client threads reached (%d), rejecting",
                      MAX_CLIENT_THREADS);
            close(clientFd);
            continue;
        }

        // Disable Nagle algorithm for low-latency RTSP responses.
        int noDelay = 1;
        if (setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY,
                       &noDelay, sizeof(noDelay)) < 0) {
            VLOG_WARN(MODULE, "TCP_NODELAY failed: %s", strerror(errno));
        }

        // Get client IP for logging and RTP addressing.
        char ipText[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipText, sizeof(ipText));
        std::string clientIp(ipText);

        VLOG_INFO(MODULE, "RTSP client connected: %s", clientIp.c_str());

        // 清理已完成的客户端线程,避免 m_clientThreads 长期增长。
        // finished 标志由线程 lambda 在退出时设置为 true。
        {
            std::lock_guard<std::mutex> threadsLock(m_clientThreadsMutex);
            for (auto it = m_clientThreads.begin();
                 it != m_clientThreads.end(); ) {
                if (it->finished->load(std::memory_order_acquire)) {
                    if (it->thread.joinable()) {
                        it->thread.join();
                    }
                    it = m_clientThreads.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Spawn a thread per client (limited by m_activeClientCount).
        // Thread is stored in m_clientThreads (not detached) so stop() can
        // join it before deinit destroys shared members (m_sps/m_codecCtx),
        // preventing use-after-free during ~RtspServer.
        m_activeClientCount++;
        {
            std::lock_guard<std::mutex> fdsLock(m_clientFdsMutex);
            m_clientFds.push_back(clientFd);
        }
        auto finishedFlag = std::make_shared<std::atomic<bool>>(false);
        std::thread clientThread([this, clientFd, clientIp, finishedFlag]() {
            handleClient(clientFd, clientIp);
            // Remove fd from tracking list on exit.
            {
                std::lock_guard<std::mutex> fdsLock(m_clientFdsMutex);
                auto it = std::find(m_clientFds.begin(), m_clientFds.end(),
                                    clientFd);
                if (it != m_clientFds.end()) {
                    m_clientFds.erase(it);
                }
            }
            m_activeClientCount--;
            finishedFlag->store(true, std::memory_order_release);
        });
        {
            std::lock_guard<std::mutex> threadsLock(m_clientThreadsMutex);
            m_clientThreads.push_back(
                ClientThread{std::move(clientThread), finishedFlag});
        }
    }

    VLOG_DEBUG(MODULE, "RTSP server thread exited");
}

// ---------------------------------------------------------------------------
// handleClient: process RTSP requests from a single client
// ---------------------------------------------------------------------------

void RtspServer::handleClient(int clientFd, const std::string &clientIp) {
    // Set receive timeout to avoid blocking forever on dead connections.
    struct timeval recvTimeout {};
    recvTimeout.tv_sec = 5;
    recvTimeout.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                   &recvTimeout, sizeof(recvTimeout)) < 0) {
        VLOG_WARN(MODULE, "SO_RCVTIMEO failed for %s: %s",
                  clientIp.c_str(), strerror(errno));
    }

    char recvBuf[4096];
    std::string requestBuffer;
    bool tcpClosed = false;
    int tcpClosedSeconds = 0;
    std::string localSessionId; // Track session for safe cleanup.

    while (m_running) {
        // If TCP connection is already closed, sleep but enforce a 60-second
        // session timeout to prevent resource leaks from stale clients.
        if (tcpClosed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            tcpClosedSeconds++;
            if (tcpClosedSeconds >= 60) {
                VLOG_INFO(MODULE, "session timeout for %s after %ds, "
                          "cleaning up", clientIp.c_str(), tcpClosedSeconds);
                break;
            }
            continue;
        }

        ssize_t bytesRead = recv(clientFd, recvBuf, sizeof(recvBuf) - 1, 0);
        if (bytesRead > 0) {
            recvBuf[bytesRead] = '\0';
            requestBuffer += recvBuf;

            // RTSP requests are terminated by \r\n\r\n.
            auto headerEnd = requestBuffer.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                continue;
            }

            std::string request = requestBuffer.substr(0, headerEnd + 4);
            requestBuffer.clear();

            VLOG_DEBUG(MODULE, "RTSP request from %s:\n%s",
                       clientIp.c_str(), request.c_str());

            std::string response = processRequest(request, clientIp);
            if (!response.empty()) {
                ssize_t sent = send(clientFd, response.c_str(),
                                    response.size(), MSG_NOSIGNAL);
                if (sent < 0) {
                    VLOG_WARN(MODULE, "send response to %s failed: %s",
                              clientIp.c_str(), strerror(errno));
                }
                // Track session ID for safe cleanup on disconnect.
                auto sessPos = response.find("Session:");
                if (sessPos != std::string::npos) {
                    auto vStart = sessPos + 8;
                    while (vStart < response.size() &&
                           response[vStart] == ' ') {
                        vStart++;
                    }
                    auto vEnd = response.find("\r\n", vStart);
                    if (vEnd == std::string::npos) {
                        vEnd = response.size();
                    }
                    std::string extracted = response.substr(
                        vStart, vEnd - vStart);
                    if (!extracted.empty()) {
                        localSessionId = extracted;
                    }
                }
            }
        } else if (bytesRead == 0) {
            // Client closed TCP connection (normal after PLAY).
            // Keep streaming: encode thread continues via pushFrame().
            VLOG_DEBUG(MODULE, "TCP connection closed by %s, "
                       "streaming continues", clientIp.c_str());
            tcpClosed = true;
        } else {
            // bytesRead < 0: error or timeout.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Receive timeout - no RTSP commands, continue.
                VLOG_DEBUG(MODULE, "recv timeout for %s, continuing",
                           clientIp.c_str());
            } else {
                VLOG_WARN(MODULE, "recv error from %s: %s",
                          clientIp.c_str(), strerror(errno));
                break;
            }
        }
    }

    // Clean up: remove client from list on disconnect.
    // Use sessionId for precise matching (IP alone is ambiguous with
    // multiple clients from the same host).
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            bool match = !localSessionId.empty()
                             ? (it->sessionId == localSessionId)
                             : (it->ip == clientIp);
            if (match) {
                VLOG_INFO(MODULE, "client session ended: %s (session=%s)",
                          clientIp.c_str(), it->sessionId.c_str());
                m_clients.erase(it);
                break;
            }
        }
    }

    close(clientFd);
}

// ---------------------------------------------------------------------------
// processRequest: dispatch RTSP method to appropriate handler
// ---------------------------------------------------------------------------

std::string RtspServer::processRequest(const std::string &request,
                                       const std::string &clientIp) {
    std::istringstream iss(request);
    std::string method, url, version;
    iss >> method >> url >> version;

    int cseq = parseCSeqValue(request);

    if (method == "OPTIONS") {
        return buildOptionsResponse(cseq);
    }
    if (method == "DESCRIBE") {
        return buildDescribeResponse(cseq);
    }
    if (method == "SETUP") {
        std::string transport = extractHeaderValue(request, "Transport:");
        return buildSetupResponse(cseq, transport, clientIp);
    }
    if (method == "PLAY") {
        std::string session = extractHeaderValue(request, "Session:");
        return buildPlayResponse(cseq, session);
    }
    if (method == "TEARDOWN") {
        std::string session = extractHeaderValue(request, "Session:");
        return buildTeardownResponse(cseq, session);
    }
    if (method == "GET_PARAMETER") {
        // VLC sends GET_PARAMETER as keepalive.
        return "RTSP/1.0 200 OK\r\nCSeq: " + std::to_string(cseq) +
               "\r\n\r\n";
    }

    VLOG_WARN(MODULE, "unsupported RTSP method: %s", method.c_str());
    return "RTSP/1.0 405 Method Not Allowed\r\nCSeq: " +
           std::to_string(cseq) + "\r\n\r\n";
}

// ---------------------------------------------------------------------------
// RTSP response builders
// ---------------------------------------------------------------------------

std::string RtspServer::buildOptionsResponse(int cseq) {
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + std::to_string(cseq) + "\r\n"
           "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, "
           "GET_PARAMETER\r\n\r\n";
}

std::string RtspServer::buildDescribeResponse(int cseq) {
    std::string sdp = buildSdp();
    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + std::to_string(cseq) + "\r\n"
           "Content-Type: application/sdp\r\n"
           "Content-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" +
           sdp;
}

std::string RtspServer::buildSetupResponse(int cseq,
                                           const std::string &transport,
                                           const std::string &clientIp) {
    int rtpPort = 0;
    int rtcpPort = 0;
    parseClientPort(transport, rtpPort, rtcpPort);

    if (rtpPort == 0) {
        VLOG_WARN(MODULE, "SETUP: no client_port in Transport header");
        return "RTSP/1.0 461 Unsupported Transport\r\nCSeq: " +
               std::to_string(cseq) + "\r\n\r\n";
    }

    std::string sessionId = generateSessionId();

    ClientInfo client;
    client.tcpFd = -1;
    client.ip = clientIp;
    client.rtpPort = rtpPort;
    client.rtcpPort = (rtcpPort > 0) ? rtcpPort : rtpPort + 1;
    client.isPlaying = false;
    client.sessionId = sessionId;
    client.seqNum = static_cast<uint16_t>(threadRng()() & 0xFFFF);
    client.ssrc = static_cast<uint32_t>(threadRng()());

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.push_back(client);
    }

    VLOG_INFO(MODULE, "SETUP: session=%s client=%s:%d",
              sessionId.c_str(), clientIp.c_str(), rtpPort);

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + std::to_string(cseq) + "\r\n"
           "Session: " + sessionId + "\r\n"
           "Transport: RTP/AVP;unicast;"
           "client_port=" + std::to_string(rtpPort) + "-" +
           std::to_string(rtcpPort) + "\r\n\r\n";
}

std::string RtspServer::buildPlayResponse(int cseq,
                                          const std::string &session) {
    bool clientFound = false;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto &client : m_clients) {
            if (client.sessionId == session) {
                client.isPlaying = true;
                clientFound = true;
                VLOG_INFO(MODULE, "PLAY: session=%s", session.c_str());
                break;
            }
        }
    }

    if (!clientFound) {
        VLOG_WARN(MODULE, "PLAY: unknown session %s", session.c_str());
        return "RTSP/1.0 454 Session Not Found\r\nCSeq: " +
               std::to_string(cseq) + "\r\n\r\n";
    }

    // Push the cached IDR frame to the new client immediately so it can
    // start decoding without waiting for the next GOP boundary (up to 1s
    // at 30fps with GOP=30). This is the application-layer fallback that
    // compensates for the libmpp MPP_ENC_SET_IDR_FRAME_REQ API defect
    // (control call disabled due to nullptr→-3 / uint32_t*=1→segfault).
    //
    // Note: m_lastIDR is updated by the encode thread on every IDR frame;
    // if no IDR has been encoded yet (server just started), this is a no-op
    // and the client falls back to the natural GOP cycle.
    //
    // Must be called AFTER releasing m_clientsMutex: sendCachedIDRToClient
    // internally calls parseAndSendNalUnits → sendNaluAsRtp, which
    // re-acquires m_clientsMutex to iterate clients.
    sendCachedIDRToClient(session);

    return "RTSP/1.0 200 OK\r\n"
           "CSeq: " + std::to_string(cseq) + "\r\n"
           "Session: " + session + "\r\n\r\n";
}

std::string RtspServer::buildTeardownResponse(int cseq,
                                              const std::string &session) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto &client : m_clients) {
        if (client.sessionId == session) {
            client.isPlaying = false;
            VLOG_INFO(MODULE, "TEARDOWN: session=%s (stopped streaming)",
                      session.c_str());
            return "RTSP/1.0 200 OK\r\n"
                   "CSeq: " + std::to_string(cseq) + "\r\n\r\n";
        }
    }
    return "RTSP/1.0 200 OK\r\nCSeq: " +
           std::to_string(cseq) + "\r\n\r\n";
}

// ---------------------------------------------------------------------------
// buildSdp: generate SDP description for H.264 video
// ---------------------------------------------------------------------------

std::string RtspServer::buildSdp() const {
    std::string clockStr = std::to_string(RTP_CLOCK_RATE);
    bool isH265 = (m_config.codec == "265");

    if (isH265) {
        // H.265 SDP (RFC 7798): uses sprop-vps/sprop-sps/sprop-pps。
        // VPS/SPS/PPS 分别从 m_vpsBase64/m_spsBase64/m_ppsBase64 取值。
        std::string ptStr = std::to_string(RTP_PT_H265);
        std::string fmtp;
        if (!m_vpsBase64.empty()) {
            fmtp += "sprop-vps=" + m_vpsBase64;
        }
        if (!m_spsBase64.empty()) {
            if (!fmtp.empty()) fmtp += ";";
            fmtp += "sprop-sps=" + m_spsBase64;
        }
        if (!m_ppsBase64.empty()) {
            if (!fmtp.empty()) fmtp += ";";
            fmtp += "sprop-pps=" + m_ppsBase64;
        }

        std::string sdp = "v=0\r\n"
            "o=- 0 0 IN IP4 " + m_serverIp + "\r\n"
            "s=CarTerminal\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP " + ptStr + "\r\n"
            "c=IN IP4 " + m_serverIp + "\r\n"
            "a=rtpmap:" + ptStr + " H265/" + clockStr + "\r\n";
        if (!fmtp.empty()) {
            sdp += "a=fmtp:" + ptStr + " " + fmtp + "\r\n";
        }
        sdp += "a=control:trackID=0\r\n";
        return sdp;
    }

    // H.264 SDP (RFC 6184): uses sprop-parameter-sets.
    std::string fmtp = "packetization-mode=1";
    if (!m_spsBase64.empty() && !m_ppsBase64.empty()) {
        fmtp += ";sprop-parameter-sets=" + m_spsBase64 + "," + m_ppsBase64;
    }

    std::string ptStr = std::to_string(RTP_PT_H264);

    return "v=0\r\n"
           "o=- 0 0 IN IP4 " + m_serverIp + "\r\n"
           "s=CarTerminal\r\n"
           "t=0 0\r\n"
           "m=video 0 RTP/AVP " + ptStr + "\r\n"
           "c=IN IP4 " + m_serverIp + "\r\n"
           "a=rtpmap:" + ptStr + " H264/" + clockStr + "\r\n"
           "a=fmtp:" + ptStr + " " + fmtp + "\r\n"
           "a=control:trackID=0\r\n";
}

// ---------------------------------------------------------------------------
// pushFrame: entry point from Monitor::showImage (any thread)
// ---------------------------------------------------------------------------

bool RtspServer::pushFrame(const uint8_t *rgb32Data, int width, int height) {
    if (!m_initialized || !m_running || !rgb32Data) {
        return false;
    }

    // Skip encoding when no clients are watching and recording is inactive
    // (saves CPU). When MP4 recording is running, frames must continue to
    // flow through the encoder so the recorder receives Annex-B data.
    if (clientCount() == 0 && !m_recorder.isRunning()) {
        return true;
    }

    // Copy frame into the latest-frame buffer for the encode thread.
    // This keeps the GUI thread non-blocking.
    size_t frameBytes = static_cast<size_t>(width) * height * 4;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestFrame.assign(rgb32Data, rgb32Data + frameBytes);
        m_latestFrameWidth = width;
        m_latestFrameHeight = height;
        m_hasNewFrame = true;
    }
    m_frameCv.notify_one();

    // Diagnostic: log every 100 frames (~3s at 30fps).
    // pushFrame() 可从任意线程调用（见头文档），static 变量必须原子以避免数据竞争。
    static std::atomic<int> s_pushCount{0};
    int pushCount = ++s_pushCount;
    if (pushCount % 100 == 1) {
        VLOG_DEBUG(MODULE, "pushFrame #%d: %dx%d, clients=%d",
                   pushCount, width, height, clientCount());
    }

    return true;
}

// ---------------------------------------------------------------------------
// encodeAndSend: RGB32 -> YUV420P -> H.264 -> parse NAL -> RTP
// ---------------------------------------------------------------------------

void RtspServer::encodeAndSend(const uint8_t *rgb32Data,
                               int width, int height) {
    if (!m_swsCtx) {
        return;
    }

    // Convert RGB32 -> YUV420P via swscale (shared by both MPP and FFmpeg paths).
    const uint8_t *srcSlice[1] = { rgb32Data };
    int srcStride[1] = { width * 4 };

    uint8_t *yPlane = m_yuvBuffer.data();
    uint8_t *uPlane = yPlane + width * height;
    uint8_t *vPlane = uPlane + (width / 2) * (height / 2);
    uint8_t *dstData[3] = { yPlane, uPlane, vPlane };
    int dstStride[3] = { width, width / 2, width / 2 };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, height,
              dstData, dstStride);

    // Use wall-clock time for RTP timestamps to avoid drift from fps mismatch
    // between camera (30fps) and encoder (configured fps).
    auto elapsed = std::chrono::steady_clock::now() - m_startTime;
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        elapsed).count();
    uint32_t timestamp = static_cast<uint32_t>(
        elapsedUs * RTP_CLOCK_RATE / 1000000);

    if (m_useMpp) {
        // --- MPP hardware encoding path (RK3588S VPU) ---
        size_t encSize = 0;
        if (!mpp::encode(m_yuvBuffer.data(), m_mppEncBuffer.data(),
                         encSize, m_mppEncBuffer.size())) {
            VLOG_ERROR(MODULE, "MPP encode failed at frame #%u", m_frameCount.load());
            m_frameCount++;
            return;
        }

        m_frameCount++;

        // Lazy SPS/PPS extraction: MPP needs at least one encoded frame
        // before MPP_ENC_GET_EXTRA_INFO returns valid data.
        if (!m_spsPpsExtracted && encSize > 0) {
            extractSpsPps();
            m_spsPpsExtracted = true;
            if (!m_sps.empty() && !m_pps.empty()) {
                VLOG_INFO(MODULE, "MPP SPS/PPS extracted after first frame: "
                          "SPS=%llu bytes, PPS=%llu bytes",
                          static_cast<unsigned long long>(m_sps.size()),
                          static_cast<unsigned long long>(m_pps.size()));
            } else {
                VLOG_WARN(MODULE, "MPP SPS/PPS extraction failed after first frame");
            }
        }

        if (encSize > 0) {
            // Detect IDR frame by scanning ALL Annex-B NAL units.
            // H.264: NAL type = byte & 0x1F (SPS=7, IDR=5).
            // H.265: NAL type = (byte >> 1) & 0x3F (SPS=33, IDR=19/20, CRA=21).
            bool isIdr = false;
            bool hasSps = false;
            bool isH265 = mpp::isH265();

            for (size_t i = 0; i + 4 < encSize; i++) {
                if (m_mppEncBuffer[i] == 0 && m_mppEncBuffer[i + 1] == 0 &&
                    m_mppEncBuffer[i + 2] == 0 && m_mppEncBuffer[i + 3] == 1) {
                    if (isH265) {
                        // H.265: NAL type in second byte of 2-byte header.
                        uint8_t nalType = (m_mppEncBuffer[i + 4] >> 1) & 0x3F;
                        if (nalType == HEVC_NAL_SPS) {
                            hasSps = true;
                            isIdr = true;
                        } else if (nalType == HEVC_NAL_IDR_W ||
                                   nalType == HEVC_NAL_IDR_N ||
                                   nalType == HEVC_NAL_CRA) {
                            isIdr = true;
                        }
                    } else {
                        // H.264: NAL type in lower 5 bits.
                        uint8_t nalType = m_mppEncBuffer[i + 4] & 0x1F;
                        if (nalType == NAL_TYPE_SPS) {
                            hasSps = true;
                            isIdr = true;
                        } else if (nalType == NAL_TYPE_IDR) {
                            isIdr = true;
                        }
                    }
                    // Continue scanning — MPP may output [SEI][SPS][PPS][IDR].
                    // We need to find SPS even if it's not the first NAL.
                    if (hasSps)
                        break;  // Found SPS, no need to prepend.
                }
            }

            if (isIdr && !hasSps && !m_sps.empty() && !m_pps.empty()) {
                // MPP IDR without embedded SPS/PPS — prepend them.
                // H.265 还需前置 VPS(解码器初始化必需)。
                bool isH265 = (m_config.codec == "265");
                VLOG_DEBUG(MODULE,
                           "IDR frame #%u: prepending %s%sSPS(%llu) + PPS(%llu)",
                           m_frameCount.load(),
                           isH265 ? "VPS+ " : "",
                           isH265 ? "" : "",
                           static_cast<unsigned long long>(m_sps.size()),
                           static_cast<unsigned long long>(m_pps.size()));
                std::vector<uint8_t> annexB;
                const uint8_t sc4[] = {0, 0, 0, 1};
                size_t reserveSize = m_sps.size() + m_pps.size() + encSize + 8;
                if (isH265 && !m_vps.empty()) {
                    reserveSize += m_vps.size() + 4;
                }
                annexB.reserve(reserveSize);
                if (isH265 && !m_vps.empty()) {
                    annexB.insert(annexB.end(), sc4, sc4 + 4);
                    annexB.insert(annexB.end(), m_vps.begin(), m_vps.end());
                }
                annexB.insert(annexB.end(), sc4, sc4 + 4);
                annexB.insert(annexB.end(), m_sps.begin(), m_sps.end());
                annexB.insert(annexB.end(), sc4, sc4 + 4);
                annexB.insert(annexB.end(), m_pps.begin(), m_pps.end());
                annexB.insert(annexB.end(),
                              m_mppEncBuffer.begin(),
                              m_mppEncBuffer.begin() + encSize);
                // Cache the IDR frame (with prepended SPS/PPS/VPS) so that
                // future PLAY requests can push it immediately.
                updateCachedIDR(annexB.data(), annexB.size(), timestamp);
                parseAndSendNalUnits(annexB.data(), annexB.size(), timestamp);
                // Feed Annex-B (with SPS/PPS prepended) to MP4 recorder.
                // isKeyFrame=true: recorder extracts extradata / switches segment.
                feedRecorder(annexB.data(), annexB.size(), true, timestamp);
            } else {
                // For IDR frames with embedded SPS/PPS, also cache the raw
                // encoder output (it already contains SPS/PPS/IDR).
                if (isIdr) {
                    updateCachedIDR(m_mppEncBuffer.data(), encSize, timestamp);
                }
                parseAndSendNalUnits(m_mppEncBuffer.data(), encSize, timestamp);
                // Feed raw encoder output to MP4 recorder. isIdr flag is
                // determined by Annex-B NAL scan above (SPS/IDR/CRA detection).
                feedRecorder(m_mppEncBuffer.data(), encSize, isIdr, timestamp);
            }
        }
    } else {
        // --- FFmpeg libx264 software encoding path ---
        if (!m_codecCtx || !m_frame || !m_packet) {
            return;
        }

        // Set up AVFrame with external buffer (no copy).
        av_frame_unref(m_frame);
        m_frame->format = AV_PIX_FMT_YUV420P;
        m_frame->width = width;
        m_frame->height = height;
        m_frame->data[0] = dstData[0];
        m_frame->data[1] = dstData[1];
        m_frame->data[2] = dstData[2];
        m_frame->linesize[0] = dstStride[0];
        m_frame->linesize[1] = dstStride[1];
        m_frame->linesize[2] = dstStride[2];
        m_frame->pts = m_frameCount.load();

        // Send frame to encoder; retrieve compressed packets in the loop below.
        int sendRet = avcodec_send_frame(m_codecCtx, m_frame);
        if (sendRet < 0) {
            VLOG_ERROR(MODULE, "avcodec_send_frame failed: %d", sendRet);
            m_frameCount++;
            return;
        }

        int recvRet = 0;
        while (recvRet >= 0) {
            recvRet = avcodec_receive_packet(m_codecCtx, m_packet);
            if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF) {
                break;
            }
            if (recvRet < 0) {
                VLOG_ERROR(MODULE, "avcodec_receive_packet failed: %d", recvRet);
                break;
            }

            // For IDR frames, prepend SPS/PPS so the decoder can initialize.
            bool isKeyFrame = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (isKeyFrame && !m_sps.empty() && !m_pps.empty()) {
                VLOG_DEBUG(MODULE,
                           "IDR frame #%u: prepending SPS(%llu) + PPS(%llu)",
                           m_frameCount.load(),
                           static_cast<unsigned long long>(m_sps.size()),
                           static_cast<unsigned long long>(m_pps.size()));
                std::vector<uint8_t> annexB;
                const uint8_t sc4[] = {0, 0, 0, 1};
                annexB.reserve(m_sps.size() + m_pps.size() + m_packet->size + 8);
                annexB.insert(annexB.end(), sc4, sc4 + 4);
                annexB.insert(annexB.end(), m_sps.begin(), m_sps.end());
                annexB.insert(annexB.end(), sc4, sc4 + 4);
                annexB.insert(annexB.end(), m_pps.begin(), m_pps.end());
                annexB.insert(annexB.end(),
                              m_packet->data,
                              m_packet->data + m_packet->size);
                // Cache the IDR frame (with prepended SPS/PPS) for PLAY.
                updateCachedIDR(annexB.data(), annexB.size(), timestamp);
                parseAndSendNalUnits(annexB.data(), annexB.size(), timestamp);
                // Feed Annex-B (with SPS/PPS prepended) to MP4 recorder.
                feedRecorder(annexB.data(), annexB.size(), true, timestamp);
            } else {
                parseAndSendNalUnits(m_packet->data,
                                     static_cast<size_t>(m_packet->size),
                                     timestamp);
                // Feed raw encoder output to MP4 recorder. isKeyFrame flag
                // comes from libx264's AV_PKT_FLAG_KEY.
                feedRecorder(m_packet->data,
                             static_cast<size_t>(m_packet->size),
                             isKeyFrame, timestamp);
            }
            av_packet_unref(m_packet);
        }
    }

    m_frameCount++;
}

// ---------------------------------------------------------------------------
// parseAndSendNalUnits: split Annex-B byte stream into individual NAL units
//
// Extracted to eliminate the duplicated start-code scanning that was
// previously copy-pasted 3 times in encodeAndSend().
// ---------------------------------------------------------------------------

void RtspServer::parseAndSendNalUnits(const uint8_t *annexBData,
                                      size_t dataSize,
                                      uint32_t timestamp,
                                      const std::string *onlySession) {
    // Phase 1: Collect all NAL unit boundaries by scanning Annex-B start codes.
    struct NalInfo {
        size_t offset;
        size_t size;
    };
    std::vector<NalInfo> nalUnits;

    size_t nalStart = 0;
    bool insideNal = false;

    for (size_t i = 0; i < dataSize; ) {
        bool isFourByteCode = (i + 4 <= dataSize &&
            annexBData[i] == 0 && annexBData[i + 1] == 0 &&
            annexBData[i + 2] == 0 && annexBData[i + 3] == 1);
        bool isThreeByteCode = !isFourByteCode && (i + 3 <= dataSize &&
            annexBData[i] == 0 && annexBData[i + 1] == 0 &&
            annexBData[i + 2] == 1);

        if (isFourByteCode || isThreeByteCode) {
            size_t codeLen = isFourByteCode ? 4 : 3;
            if (insideNal && nalStart < i) {
                nalUnits.push_back({nalStart, i - nalStart});
            }
            nalStart = i + codeLen;
            insideNal = true;
            i += codeLen;
        } else {
            i++;
        }
    }

    // Last NAL in the buffer.
    if (insideNal && nalStart < dataSize) {
        nalUnits.push_back({nalStart, dataSize - nalStart});
    }

    // Phase 2: Send all NAL units, marking the last one for RTP marker bit.
    // If onlySession is non-null, only that single client receives the NALs
    // (used by sendCachedIDRToClient to push a cached IDR to one new client).
    for (size_t idx = 0; idx < nalUnits.size(); idx++) {
        const auto &nal = nalUnits[idx];
        bool isLastNal = (idx == nalUnits.size() - 1);
        sendNaluAsRtp(annexBData + nal.offset, nal.size, timestamp,
                      isLastNal, onlySession);
    }
}

// ---------------------------------------------------------------------------
// updateCachedIDR: cache the latest IDR frame (Annex-B with SPS/PPS/VPS prepended)
//
// Called by encodeAndSend() when an IDR frame is detected. The cached copy
// is later pushed to new clients on PLAY request via sendCachedIDRToClient(),
// eliminating the 1-2s "black screen" wait for the next GOP boundary.
//
// Thread safety: m_idrMutex protects m_lastIDR. Updated only by the encode
// thread (encodeAndSend), read by the server thread (buildPlayResponse).
// ---------------------------------------------------------------------------

void RtspServer::updateCachedIDR(const uint8_t *annexBData, size_t dataSize,
                                 uint32_t timestamp) {
    if (!annexBData || dataSize == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_idrMutex);
    m_lastIDR.annexB.assign(annexBData, annexBData + dataSize);
    m_lastIDR.timestamp = timestamp;
    m_lastIDR.valid = true;
}

// ---------------------------------------------------------------------------
// sendCachedIDRToClient: push the cached IDR frame to a single client.
//
// Called by buildPlayResponse() after marking the client as isPlaying=true.
// This allows the client to start decoding immediately without waiting for
// the next GOP boundary. The cached IDR is sent with its original RTP
// timestamp (which may be slightly stale but is acceptable — decoder PTS
// only needs monotonic increase, and the next live frame will have a newer PTS).
//
// If no IDR has been cached yet (e.g. server just started before the first
// IDR was encoded), this is a no-op and the client falls back to the
// natural GOP cycle (~1s at gop=30).
// ---------------------------------------------------------------------------

void RtspServer::sendCachedIDRToClient(const std::string &sessionId) {
    std::vector<uint8_t> annexBCopy;
    uint32_t timestamp = 0;
    {
        std::lock_guard<std::mutex> lock(m_idrMutex);
        if (!m_lastIDR.valid) {
            return;
        }
        annexBCopy = m_lastIDR.annexB;  // copy under lock
        timestamp = m_lastIDR.timestamp;
    }
    // Lock released — parseAndSendNalUnits may block on m_clientsMutex.
    VLOG_INFO(MODULE,
              "PLAY: pushing cached IDR (%zu bytes, ts=%u) to session=%s",
              annexBCopy.size(), timestamp, sessionId.c_str());
    parseAndSendNalUnits(annexBCopy.data(), annexBCopy.size(),
                         timestamp, &sessionId);
}

// ---------------------------------------------------------------------------
// feedRecorder: forward encoded Annex-B frame to MP4 recorder
//
// Called from encodeAndSend() right after parseAndSendNalUnits(). When the
// recorder is not running (m_recorder.isRunning() == false) this is a cheap
// no-op (single atomic load). The RTP timestamp (90 kHz clock) is converted
// to microseconds so the recorder can produce wall-clock-aligned PTS.
// ---------------------------------------------------------------------------

void RtspServer::feedRecorder(const uint8_t *annexBData, size_t dataSize,
                                bool isKeyFrame, uint32_t timestamp) {
    if (!m_recorder.isRunning()) {
        return;
    }
    // RTP clock is 90 kHz; convert to microseconds (1/1e6 s).
    const int64_t timestampUs =
        static_cast<int64_t>(timestamp) * 1000000LL / 90000LL;
    m_recorder.writeFrame(annexBData, dataSize, isKeyFrame, timestampUs);
}

// ---------------------------------------------------------------------------
// Recording control: thin wrappers around Mp4Recorder.
// ---------------------------------------------------------------------------

bool RtspServer::startRecording() {
    if (!m_initialized) {
        VLOG_WARN(MODULE, "cannot start recording: RTSP not initialized");
        return false;
    }
    const RecorderStatus status = m_recorder.start();
    if (status == RecorderStatus::Success) {
        VLOG_INFO(MODULE, "recording started: %s",
                  m_recorder.currentSegmentPath().c_str());
        return true;
    }
    if (status == RecorderStatus::Disabled) {
        // Recording disabled in config — not an error, stay silent.
        return false;
    }
    VLOG_WARN(MODULE, "recording start failed: %s",
              recorderStatusToString(status));
    return false;
}

void RtspServer::stopRecording() {
    m_recorder.stop();
}

bool RtspServer::isRecording() const {
    return m_recorder.isRunning();
}

std::string RtspServer::recordingStatus() const {
    return m_recorder.getStatus();
}

// ---------------------------------------------------------------------------
// sendNaluAsRtp: dispatch to single-NAL or FU-A packetization
// ---------------------------------------------------------------------------

void RtspServer::sendNaluAsRtp(const uint8_t *nalu, size_t size,
                               uint32_t timestamp, bool isLastNal,
                               const std::string *onlySession) {
    if (size == 0) {
        return;
    }

    // Build send context list under a single lock hold.
    // Use direct pointers to client seqNum to avoid read/writeback race.
    // If onlySession is non-null, only that session is included (used by
    // sendCachedIDRToClient to push a cached IDR to one specific client).
    struct SendEntry {
        RtpSendContext ctx;
        std::string sessionId;
    };
    std::vector<SendEntry> entries;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto &client : m_clients) {
            if (!client.isPlaying || client.rtpPort == 0) {
                continue;
            }
            if (onlySession != nullptr && client.sessionId != *onlySession) {
                continue;
            }
            SendEntry entry;
            entry.ctx.destIp = client.ip;
            entry.ctx.destPort = client.rtpPort;
            entry.ctx.seqNum = &client.seqNum;
            entry.ctx.ssrc = client.ssrc;
            entry.sessionId = client.sessionId;
            entries.push_back(std::move(entry));
        }
    }

    // Send without holding the lock (sendto is fast for small UDP packets).
    for (auto &entry : entries) {
        if (size <= RTP_MAX_PAYLOAD) {
            sendSingleNalu(nalu, size, timestamp, isLastNal, entry.ctx);
        } else {
            sendFuANalu(nalu, size, timestamp, isLastNal, entry.ctx);
        }
    }
}

// ---------------------------------------------------------------------------
// sendSingleNalu: send a small NAL unit in a single RTP packet
// ---------------------------------------------------------------------------

void RtspServer::sendSingleNalu(const uint8_t *nalu, size_t size,
                                uint32_t timestamp, bool isLastNal,
                                const RtpSendContext &ctx) {
    // Marker bit: set on the last NAL of the access unit (frame).
    // Per RFC 6184 Section 5.1, the marker bit indicates the last packet
    // of an access unit, enabling the decoder to immediately process the frame.
    sendRtpPacket(nalu, size, timestamp, isLastNal, ctx);
}

// ---------------------------------------------------------------------------
// sendFuANalu: fragment a large NAL unit into FU-A RTP packets (RFC 6184)
// ---------------------------------------------------------------------------

void RtspServer::sendFuANalu(const uint8_t *nalu, size_t size,
                             uint32_t timestamp, bool isLastNal,
                             const RtpSendContext &ctx) {
    bool isH265 = (m_config.codec == "265");

    if (isH265) {
        // H.265 FU 分片(RFC 7798 Section 4.4.3)。
        // H.265 NAL header 是 2 字节:type = (byte0 >> 1) & 0x3F。
        if (size < 2) return;  // H.265 NAL header 至少 2 字节。
        uint8_t nalType = (nalu[0] >> 1) & 0x3F;

        // PayloadHdr(2 字节): type=49(FU), layerid=0, tid=1。
        // byte0 = (49 << 1) | (layerid >> 5) = 0x62
        // byte1 = ((layerid & 0x1F) << 3) | tid = 0x01
        uint8_t payloadHdr0 = static_cast<uint8_t>(HEVC_NAL_FU << 1);
        uint8_t payloadHdr1 = 0x01;  // layerid=0, tid=1

        size_t payloadOffset = 2;  // 跳过原 2 字节 NAL header。
        bool isFirstFragment = true;

        while (payloadOffset < size) {
            // FU payload: [PayloadHdr(2)][FuHeader(1)][fragment data]。
            size_t chunkSize = std::min(RTP_MAX_PAYLOAD - 3,
                                        size - payloadOffset);
            bool isLastFragment = (payloadOffset + chunkSize >= size);

            // FuHeader(1 字节): [S:1][E:1][FuType:6]。
            uint8_t fuHeader = static_cast<uint8_t>(nalType);
            if (isFirstFragment) {
                fuHeader |= 0x80;  // S=1 (Start)。
            }
            if (isLastFragment) {
                fuHeader |= 0x40;  // E=1 (End)。
            }

            uint8_t fuPayload[RTP_MAX_PAYLOAD];
            fuPayload[0] = payloadHdr0;
            fuPayload[1] = payloadHdr1;
            fuPayload[2] = fuHeader;
            memcpy(fuPayload + 3, nalu + payloadOffset, chunkSize);

            sendRtpPacket(fuPayload, chunkSize + 3, timestamp,
                          isLastFragment && isLastNal, ctx);

            payloadOffset += chunkSize;
            isFirstFragment = false;
        }
        return;
    }

    // H.264 FU-A 分片(RFC 6184 Section 5.8)。
    uint8_t nalHeader = nalu[0];
    uint8_t nalNri = nalHeader & 0x60;
    uint8_t nalType = nalHeader & 0x1F;

    // FU-A indicator: [F=0 | NRI | Type=28].
    uint8_t fuIndicator = nalNri | NAL_TYPE_FU_A;

    size_t payloadOffset = 1; // Skip original NAL header.
    bool isFirstFragment = true;

    while (payloadOffset < size) {
        size_t chunkSize = std::min(RTP_MAX_PAYLOAD - 2,
                                    size - payloadOffset);
        bool isLastFragment = (payloadOffset + chunkSize >= size);

        // FU header: [S | E | R=0 | Type].
        uint8_t fuHeader = nalType;
        if (isFirstFragment) {
            fuHeader |= 0x80; // Start bit.
        }
        if (isLastFragment) {
            fuHeader |= 0x40; // End bit.
        }

        // Build FU-A payload: [indicator][header][fragment data].
        uint8_t fuPayload[RTP_MAX_PAYLOAD];
        fuPayload[0] = fuIndicator;
        fuPayload[1] = fuHeader;
        memcpy(fuPayload + 2, nalu + payloadOffset, chunkSize);

        // Marker bit: only on last fragment of last NAL of the frame.
        sendRtpPacket(fuPayload, chunkSize + 2, timestamp,
                      isLastFragment && isLastNal, ctx);

        payloadOffset += chunkSize;
        isFirstFragment = false;
    }
}

// ---------------------------------------------------------------------------
// sendRtpPacket: build RTP header and send via UDP
// ---------------------------------------------------------------------------

void RtspServer::sendRtpPacket(const uint8_t *payload, size_t size,
                               uint32_t timestamp, bool marker,
                               const RtpSendContext &ctx) {
    if (m_rtpFd < 0 || size == 0 || !ctx.seqNum) {
        return;
    }

    // Build 12-byte RTP header.
    uint8_t rtpHeader[12];
    rtpHeader[0] = 0x80; // V=2, P=0, X=0, CC=0.
    // 选择 RTP PT: H.264=96, H.265=97(根据 config.codec)。
    uint8_t pt = (m_config.codec == "265") ? RTP_PT_H265 : RTP_PT_H264;
    rtpHeader[1] = (marker ? 0x80 : 0x00) | pt;
    rtpHeader[2] = (*ctx.seqNum >> 8) & 0xFF;
    rtpHeader[3] = *ctx.seqNum & 0xFF;
    rtpHeader[4] = (timestamp >> 24) & 0xFF;
    rtpHeader[5] = (timestamp >> 16) & 0xFF;
    rtpHeader[6] = (timestamp >> 8) & 0xFF;
    rtpHeader[7] = timestamp & 0xFF;
    rtpHeader[8] = (ctx.ssrc >> 24) & 0xFF;
    rtpHeader[9] = (ctx.ssrc >> 16) & 0xFF;
    rtpHeader[10] = (ctx.ssrc >> 8) & 0xFF;
    rtpHeader[11] = ctx.ssrc & 0xFF;

    (*ctx.seqNum)++;

    // Combine header + payload in one buffer for a single sendto call.
    uint8_t rtpPacket[12 + RTP_MAX_PAYLOAD + 2];
    size_t packetLen = 12 + size;
    memcpy(rtpPacket, rtpHeader, 12);
    memcpy(rtpPacket + 12, payload, size);

    struct sockaddr_in destAddr {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(static_cast<uint16_t>(ctx.destPort));
    if (inet_pton(AF_INET, ctx.destIp.c_str(), &destAddr.sin_addr) <= 0) {
        VLOG_WARN(MODULE, "inet_pton failed for %s", ctx.destIp.c_str());
        return;
    }

    ssize_t sent = sendto(m_rtpFd, rtpPacket, packetLen, 0,
                          reinterpret_cast<struct sockaddr *>(&destAddr),
                          sizeof(destAddr));
    if (sent < 0) {
        VLOG_WARN(MODULE, "RTP sendto %s:%d failed: %s",
                  ctx.destIp.c_str(), ctx.destPort, strerror(errno));
    }
}

} // namespace media
