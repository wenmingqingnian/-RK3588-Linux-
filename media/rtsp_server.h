// Minimal embedded RTSP server for RK3588S CarTerminal.
//
// Provides a self-contained RTSP server that VLC/FFplay can connect to
// via "rtsp://<ip>:<port><path>" (default: rtsp://0.0.0.0:8554/live).
//
// Architecture:
//   pushFrame(rgb32) -> swscale RGB32->YUV420P -> MPP/libx264 -> RTP/UDP
//   Background thread: TCP RTSP control protocol
//     (OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN)
//
// Dependencies:
//   - Primary: MPP hardware encoder (librockchip_mpp.so via dlopen)
//   - Fallback: FFmpeg libx264 software encoding
//   - Always: FFmpeg libswscale for RGB32→YUV420P color conversion
//
// Thread safety: pushFrame() is safe from any thread; init/deinit are not.
//
// Lifecycle: init() -> start() -> pushFrame() -> stop() -> deinit()

#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <limits>

#include "mp4_recorder.h"

// Forward declarations for FFmpeg types (swscale only).
struct SwsContext;

// Forward declarations for FFmpeg AVCodec types (software fallback).
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace media {

// RTSP server operation status codes.
// >= 1 success, <= 0 failure.
enum class RtspStatus : int {
    Success             =  1,
    Disabled            =  0,
    EncoderNotFound     = -1,
    CodecInitFailed     = -2,
    FrameAllocFailed    = -3,
    SwscaleInitFailed   = -4,
    SocketCreateFailed  = -5,
    BindFailed          = -6,
    ListenFailed        = -7,
    AlreadyRunning      = -8,
    NotInitialized      = -9,
    InvalidConfig       = -10,
};

const char *rtspStatusToString(RtspStatus status);

struct RtspServerConfig {
    bool enabled = false;
    int port = 8554;
    std::string streamPath = "/live";
    int width = 800;
    int height = 600;
    int fps = 25;
    int bitrate = 2000000;
    int gopSize = 25;
    int maxClients = 4;
    std::string codec = "264"; // "264" or "265"

    // Recording sub-configuration. The recorder consumes the Annex-B byte
    // stream already produced by the encoder in encodeAndSend(), so enabling
    // recording adds zero encoding overhead. codec/width/height/fps should
    // match the parent RTSP fields; if left at defaults they are inherited
    // automatically in RtspServer::init().
    RecordingConfig recording;
};

inline bool validateRtspServerConfig(const RtspServerConfig &config,
                                     std::string *errorMessage = nullptr)
{
    const auto fail = [errorMessage](const char *message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (!config.enabled) {
        return true;
    }
    if (config.port < 1 || config.port > 65535) {
        return fail("rtsp_port must be between 1 and 65535");
    }
    if (config.streamPath.empty() || config.streamPath[0] != '/') {
        return fail("rtsp_stream_path must start with '/'");
    }
    if (config.width < 32 || config.width > 3840 ||
        config.height < 32 || config.height > 2160 ||
        (config.width % 2) != 0 || (config.height % 2) != 0) {
        return fail("RTSP dimensions must be bounded positive even values");
    }
    if (config.fps < 1 || config.fps > 120) {
        return fail("rtsp_fps must be between 1 and 120");
    }
    if (config.bitrate < 1 ||
        config.bitrate > std::numeric_limits<int>::max() / 2) {
        return fail("rtsp_bitrate is outside the safe MPP range");
    }
    if (config.gopSize < 1 || config.gopSize > 1000) {
        return fail("rtsp_gop_size must be between 1 and 1000");
    }
    if (config.maxClients < 1 || config.maxClients > 8) {
        return fail("rtsp_max_clients must be between 1 and 8");
    }
    if (config.codec != "264" && config.codec != "265") {
        return fail("rtsp_codec must be 264 or 265");
    }

    // Validate embedded recording sub-config (recording.enabled=false is OK).
    std::string recordingError;
    if (!validateRecordingConfig(config.recording, &recordingError)) {
        if (errorMessage) {
            *errorMessage = std::string("recording: ") + recordingError;
        }
        return false;
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

// RTP send destination (replaces 8-parameter function calls).
struct RtpSendContext {
    std::string destIp;
    int destPort = 0;
    uint16_t *seqNum = nullptr;
    uint32_t ssrc = 0;
};

class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    // Module lifecycle (follows project module interface convention).
    RtspStatus init(const RtspServerConfig &config);
    RtspStatus start();
    void stop();
    void deinit();

    bool isInitialized() const { return m_initialized; }
    bool isRunning() const { return m_running; }

    // Push an RGB32 frame for encoding and RTP streaming.
    // Thread-safe: may be called from any thread (e.g. GUI thread).
    // Returns true if frame was encoded and sent to at least one client.
    bool pushFrame(const uint8_t *rgb32Data, int width, int height);

    // Number of clients currently in PLAY state.
    int clientCount() const;

    // Get human-readable module status for diagnostics.
    std::string getStatus() const;

    // --- Recording control ---
    // Recording is opt-in via RtspServerConfig.recording.enabled. When the
    // config has recording disabled, these calls are no-ops returning false.
    // start() / stop() are idempotent and safe to call from the GUI thread.
    //
    // Even when recording is enabled in config, the recorder stays stopped
    // until startRecording() is called. This allows the application to gate
    // recording on user input (e.g. a "Record" button) or runtime policy.
    bool startRecording();
    void stopRecording();
    bool isRecording() const;
    std::string recordingStatus() const;

private:
    // --- Encoder initialization (split from init for readability) ---
    RtspStatus initEncoder(const RtspServerConfig &config);
    RtspStatus initSwscale(const RtspServerConfig &config);
    RtspStatus initSockets(const RtspServerConfig &config);
    void extractSpsPps();
    void extractSpsPpsFromMppHeader(const uint8_t *headerData, size_t headerSize);
    void extractSpsPpsFromAvcc();
    void extractSpsPpsFromAnnexB();

    // --- RTSP TCP server (background thread) ---
    void serverLoop();
    void handleClient(int clientFd, const std::string &clientIp);
    void joinClientThreads();  // join all client threads (called by stop)
    std::string processRequest(const std::string &request,
                               const std::string &clientIp);
    std::string buildOptionsResponse(int cseq);
    std::string buildDescribeResponse(int cseq);
    std::string buildSetupResponse(int cseq, const std::string &transport,
                                   const std::string &clientIp);
    std::string buildPlayResponse(int cseq, const std::string &session);
    std::string buildTeardownResponse(int cseq, const std::string &session);
    std::string buildSdp() const;
    std::string extractHeaderValue(const std::string &request,
                                   const std::string &headerName) const;

    // --- RTP H.264 packetization ---
    void encodeAndSend(const uint8_t *rgb32Data, int width, int height);
    void parseAndSendNalUnits(const uint8_t *annexBData, size_t dataSize,
                              uint32_t timestamp,
                              const std::string *onlySession = nullptr);
    void sendNaluAsRtp(const uint8_t *nalu, size_t size,
                       uint32_t timestamp, bool isLastNal,
                       const std::string *onlySession = nullptr);
    void sendSingleNalu(const uint8_t *nalu, size_t size,
                        uint32_t timestamp, bool isLastNal,
                        const RtpSendContext &ctx);
    void sendFuANalu(const uint8_t *nalu, size_t size,
                     uint32_t timestamp, bool isLastNal,
                     const RtpSendContext &ctx);
    void sendRtpPacket(const uint8_t *payload, size_t size,
                       uint32_t timestamp, bool marker,
                       const RtpSendContext &ctx);

    // --- Cached IDR frame (application-layer IDR forcing) ---
    // On PLAY request, push the cached IDR + SPS/PPS to the new client
    // immediately, avoiding the wait for the next GOP boundary. This
    // eliminates the 1-2s "black screen" delay when VLC connects, even
    // though the MPP_ENC_SET_IDR_FRAME_REQ control call is disabled
    // (due to libmpp ABI defect on the board).
    //
    // Thread safety: m_idrMutex protects m_lastIDR. Updated by encode
    // thread (encodeAndSend), read by server thread (buildPlayResponse).
    void updateCachedIDR(const uint8_t *annexBData, size_t dataSize,
                         uint32_t timestamp);
    void sendCachedIDRToClient(const std::string &sessionId);

    // --- Recorder integration ---
    // Forwards an encoded Annex-B byte stream to Mp4Recorder. Called from
    // encodeAndSend() after MPP/libx264 produces the bytes. The call is
    // a no-op when recording is not running (m_recorder.isRunning()==false).
    // Identified keyframes feed the recorder with isKeyFrame=true so it
    // can extract extradata and switch segments at IDR boundaries.
    void feedRecorder(const uint8_t *annexBData, size_t dataSize,
                      bool isKeyFrame, uint32_t timestamp);

    // --- Client tracking ---
    struct ClientInfo {
        int tcpFd = -1;
        std::string ip;
        int rtpPort = 0;
        int rtcpPort = 0;
        bool isPlaying = false;
        std::string sessionId;
        uint16_t seqNum = 0;
        uint32_t ssrc = 0;
    };

    // Client thread entry: thread handle + finished flag.
    // finished flag is shared with the thread lambda so serverLoop can
    // detect completion and join+erase without blocking.
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    // --- State ---
    bool m_initialized = false;
    RtspServerConfig m_config;

    // TCP server.
    int m_tcpFd = -1;
    std::thread m_serverThread;
    std::atomic<bool> m_running{false};

    // Active client thread count (limits concurrent client threads).
    std::atomic<int> m_activeClientCount{0};

    // Client list (protected by m_clientsMutex).
    // Uses std::list for stable element pointers (sendNaluAsRtp holds
    // pointers to client.seqNum across lock boundaries).
    mutable std::mutex m_clientsMutex;
    std::list<ClientInfo> m_clients;

    // Active client threads (replaces detached threads to avoid UB during
    // ~RtspServer: detached threads could access destroyed members after
    // deinit clears m_sps/m_codecCtx etc.). Joined in stop().
    // Uses std::list for stable iterators (serverLoop erases finished entries
    // during traversal without invalidating other iterators).
    std::list<ClientThread> m_clientThreads;
    std::mutex m_clientThreadsMutex;

    // Active client TCP fds (shutdown on stop() to unblock recv() in
    // handleClient, allowing client threads to exit promptly instead of
    // waiting up to 5s for SO_RCVTIMEO).
    std::vector<int> m_clientFds;
    std::mutex m_clientFdsMutex;

    // Shared UDP socket for RTP data.
    int m_rtpFd = -1;

    // Encoder mode: true = MPP hardware, false = FFmpeg libx264 software.
    bool m_useMpp = false;

    // MPP encoding output buffer (pre-allocated to avoid per-frame allocation).
    std::vector<uint8_t> m_mppEncBuffer;

    // FFmpeg encoding contexts (software fallback).
    AVCodecContext *m_codecCtx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;

    // Swscale for RGB32 -> YUV420P conversion (used by both paths).
    SwsContext *m_swsCtx = nullptr;

    // Pre-allocated YUV420P buffer.
    std::vector<uint8_t> m_yuvBuffer;

    // H.264/H.265 SPS/PPS (extracted from encoder after first frame encoded).
    // H.265 also stores VPS (Video Parameter Set, NAL type 32) — required by
    // RFC 7798 SDP sprop-vps and by HEVC decoders for initialization.
    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
    std::vector<uint8_t> m_vps;  // H.265 only
    std::string m_spsBase64;
    std::string m_ppsBase64;
    std::string m_vpsBase64;     // H.265 only
    bool m_spsPpsExtracted = false;  // MPP: must encode 1 frame before extracting.

    // Cached IDR frame for application-layer IDR forcing on PLAY.
    // Stores the last encoded IDR frame as Annex-B byte stream (including
    // prepended SPS/PPS/VPS). When a new client sends PLAY, this frame is
    // pushed immediately so the decoder can initialize without waiting for
    // the next GOP boundary.
    //
    // Updated by encode thread in encodeAndSend() when isIdr == true.
    // Read by server thread in sendCachedIDRToClient() on PLAY request.
    // Protected by m_idrMutex.
    struct CachedIDRFrame {
        std::vector<uint8_t> annexB;  // [SPS][PPS][IDR] or [VPS][SPS][PPS][IDR]
        uint32_t timestamp = 0;       // RTP timestamp of the IDR frame
        bool valid = false;           // true after first IDR has been cached
    };
    CachedIDRFrame m_lastIDR;
    mutable std::mutex m_idrMutex;

    // Frame counter for encoding PTS.
    // 跨线程访问：encode 线程递增，getStatus() 从其他线程读取 → 必须原子。
    std::atomic<uint32_t> m_frameCount{0};

    // Wall-clock start time for RTP timestamps (avoids drift from fps mismatch).
    std::chrono::steady_clock::time_point m_startTime;

    // --- MP4 recorder ---
    // Reuses the Annex-B byte stream produced by encodeAndSend(); adds zero
    // additional encoding overhead. When recording is disabled in config or
    // not started via startRecording(), writeFrame() returns false and no
    // files are written.
    Mp4Recorder m_recorder;

    // Dedicated encode thread (keeps encoding off the GUI thread).
    std::thread m_encodeThread;
    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    std::vector<uint8_t> m_latestFrame;
    int m_latestFrameWidth = 0;
    int m_latestFrameHeight = 0;
    bool m_hasNewFrame = false;

    // Server IP (auto-detected at init).
    std::string m_serverIp;
};

} // namespace media

#endif // RTSP_SERVER_H
