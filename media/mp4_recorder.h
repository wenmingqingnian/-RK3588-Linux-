// MP4 video recorder for RK3588S CarTerminal.
//
// Records the Annex-B H.264/H.265 byte stream produced by RtspServer's
// MPP encoder into segmented MP4 files. Reuses the already-encoded stream
// to avoid calling the MPP encoder a second time (the encoder is a process
// -wide singleton with static state, so concurrent calls are forbidden).
//
// Architecture:
//   RtspServer::encodeAndSend()
//     └── produces Annex-B [SPS][PPS][IDR] / [P] byte stream
//         └── Mp4Recorder::writeFrame(annexB, size, isKeyFrame, ts)
//               ├── Annex-B → AVCC conversion (length-prefixed NAL units)
//               ├── av_write_frame() into MP4 container
//               └── segment switch on time boundary
//
// Dependencies:
//   - FFmpeg libavformat (MP4 muxer, AVFormatContext)
//   - FFmpeg libavcodec  (AV_CODEC_ID_H264 / H265 parameters)
//   - FFmpeg libavutil   (AVRational, av_packet_alloc)
//
// Thread safety: writeFrame() is safe from any thread (typically called
// from RtspServer's encode thread). init/start/stop/deinit are not
// thread-safe and must be called from the same owning thread (GUI main).
//
// Lifecycle: init() -> start() -> writeFrame()* -> stop() -> deinit()
//            Repeated start()/stop() cycles are allowed within one init().

#ifndef MP4_RECORDER_H
#define MP4_RECORDER_H

#include <cstdint>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <mutex>

// Forward declarations for FFmpeg types (avoid pulling <libavformat/...> into
// every translation unit that includes this header).
struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace media {

// Recorder operation status codes (mirrors RtspStatus convention:
// >= 1 success, <= 0 failure).
enum class RecorderStatus : int {
    Success               =  1,
    Disabled              =  0,
    InvalidConfig         = -1,
    OutputDirFailed       = -2,  // mkdir -p output directory failed
    MuxerAllocFailed      = -3,  // avformat_alloc_output_context2 failed
    StreamAddFailed       = -4,  // avformat_new_stream failed
    HeaderWriteFailed     = -5,  // avformat_write_header failed
    NotInitialized        = -6,
    AlreadyRunning        = -7,
    SegmentOpenFailed     = -8,  // avio_open / write_header on new segment
    NotRunning            = -9,
};

const char *recorderStatusToString(RecorderStatus status);

// Recording configuration. Loaded from config/recording_settings.conf by
// MainWindow::loadAllConfigs(). The codec/width/height/fps fields should
// match the RTSP/camera configuration to avoid rescaling or re-encoding.
struct RecordingConfig {
    bool enabled = false;
    std::string outputDir = "recordings";     // directory for .mp4 files
    std::string filenamePrefix = "record";    // e.g. record_0001.mp4
    int segmentDurationSec = 60;               // switch file every N seconds
    int maxSegments = 10;                      // keep at most N segments
    int width = 800;
    int height = 600;
    int fps = 30;
    std::string codec = "264";                 // "264" or "265"
};

// Validate config fields; returns false and sets *errorMessage on failure.
inline bool validateRecordingConfig(const RecordingConfig &config,
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
    if (config.outputDir.empty()) {
        return fail("recording_output_dir must not be empty");
    }
    if (config.filenamePrefix.empty()) {
        return fail("recording_filename_prefix must not be empty");
    }
    if (config.segmentDurationSec < 5 || config.segmentDurationSec > 3600) {
        return fail("recording_segment_duration_sec must be between 5 and 3600");
    }
    if (config.maxSegments < 1 || config.maxSegments > 1000) {
        return fail("recording_max_segments must be between 1 and 1000");
    }
    if (config.width < 32 || config.width > 3840 ||
        config.height < 32 || config.height > 2160 ||
        (config.width % 2) != 0 || (config.height % 2) != 0) {
        return fail("recording dimensions must be bounded positive even values");
    }
    if (config.fps < 1 || config.fps > 120) {
        return fail("recording_fps must be between 1 and 120");
    }
    if (config.codec != "264" && config.codec != "265") {
        return fail("recording_codec must be 264 or 265");
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

// Mp4Recorder: segmented MP4 muxer consuming Annex-B H.264/H.265 byte stream.
//
// Design notes:
//   1. Reuses the Annex-B stream already produced by RtspServer (no second
//      MPP encode call — the encoder is a process-wide singleton).
//   2. Converts Annex-B (start-code prefixed) to AVCC (length-prefixed)
//      per writeFrame() call, as required by the MP4 container.
//   3. Extracts SPS/PPS (and VPS for H.265) from the first keyframe to
//      populate AVCodecParameters::extradata (avcC box) before writeHeader().
//   4. Switches to a new MP4 segment when wall-clock duration exceeds
//      segmentDurationSec; deletes oldest segments beyond maxSegments.
class Mp4Recorder {
public:
    Mp4Recorder();
    ~Mp4Recorder();

    // Module lifecycle (follows project module interface convention).
    RecorderStatus init(const RecordingConfig &config);
    void deinit();

    // Start / stop recording. start() opens the first segment; stop() closes
    // the current segment. Repeated start()/stop() cycles are allowed.
    // Returns RecorderStatus::Success on success.
    RecorderStatus start();
    void stop();

    bool isInitialized() const { return m_initialized; }
    bool isRunning() const { return m_running.load(); }

    // Write one frame of Annex-B byte stream to the current segment.
    //
    // annexBData:   pointer to Annex-B bytes ([00 00 00 01 NAL]+ ...).
    //               May contain [SPS][PPS][IDR] for keyframes, or just [P].
    // size:         byte count of annexBData.
    // isKeyFrame:   true if this frame is IDR/H.265-IDR/CRA. On the first
    //               keyframe, extradata is extracted and writeHeader() runs.
    // timestampUs:  frame timestamp in microseconds since recorder start.
    //               Pass 0 to use internal steady_clock (auto PTS).
    //
    // Thread-safe: may be called from RtspServer's encode thread.
    // Returns false if not running or if muxer write fails (logged).
    bool writeFrame(const uint8_t *annexBData, size_t size,
                    bool isKeyFrame, int64_t timestampUs = 0);

    // Path of the currently open segment file (empty if not running).
    std::string currentSegmentPath() const;

    // Human-readable status for diagnostics (used in logs / UI status line).
    std::string getStatus() const;

private:
    // --- Segment management ---
    // Open a new .mp4 file: allocate AVFormatContext, add video stream,
    // set codec parameters, write header. Sets m_currentSegmentPath on success.
    bool openNewSegment();

    // Close the current segment: write trailer, free context, close file.
    // Resets m_fmtCtx/m_videoStream to nullptr. Safe to call when no segment
    // is open (no-op).
    void closeCurrentSegment();

    // Delete oldest segment files when total count exceeds maxSegments.
    void cleanupOldSegments();

    // --- Annex-B / AVCC conversion helpers ---
    // Extract SPS/PPS/VPS from Annex-B keyframe and populate m_extradata
    // (avcC format for H.264, hvcC format for H.265).
    // Returns false if no SPS/PPS found.
    bool extractExtradata(const uint8_t *annexBData, size_t size);

    // Convert Annex-B byte stream to AVCC (length-prefixed NAL units).
    // Used for both AVPacket data and extradata construction.
    // isH265: true for H.265 (skip VPS/SPS/PPS), false for H.264 (skip SPS/PPS).
    static bool annexBToAvcc(const uint8_t *annexBData, size_t size,
                             bool isH265, std::vector<uint8_t> &avccOut);

    // Return true if wall-clock time since segment open exceeds configured
    // segment duration. Uses steady_clock so wall-clock skew is not an issue.
    bool shouldSwitchSegment() const;

    // Build the next segment file path: <outputDir>/<prefix>_<NNNN>.mp4
    std::string buildSegmentPath() const;

    // --- FFmpeg muxer state (all protected by m_mutex) ---
    AVFormatContext *m_fmtCtx = nullptr;
    AVStream *m_videoStream = nullptr;
    AVPacket *m_packet = nullptr;
    bool m_headerWritten = false;

    // avcC/hvcC extradata, extracted from the first keyframe.
    std::vector<uint8_t> m_extradata;
    bool m_extradataReady = false;

    // --- Recording state ---
    bool m_initialized = false;
    std::atomic<bool> m_running{false};
    RecordingConfig m_config;

    // Wall-clock anchor for the current segment (segment switch decision).
    std::chrono::steady_clock::time_point m_segmentStartTime;

    // PTS bookkeeping: microsecond timestamps of the first and last frame.
    // Used to compute relative PTS within a segment and to detect time jumps.
    int64_t m_firstFrameTimestampUs = -1;
    int64_t m_lastFrameTimestampUs = 0;
    // Last DTS/PTS written in the current segment time base.
    int64_t m_lastPacketPts = -1;

    // Monotonic segment counter used in filenames (record_0001.mp4, ...).
    int m_segmentIndex = 0;
    std::string m_currentSegmentPath;

    // List of segment files produced in this session, oldest first.
    // Used by cleanupOldSegments() to delete from the front.
    std::vector<std::string> m_segmentFiles;

    // Protects m_fmtCtx / m_videoStream / m_packet / m_headerWritten /
    // m_extradata* / m_segment* / m_currentSegmentPath / m_segmentFiles.
    // writeFrame() runs on the encode thread; start/stop/deinit run on the
    // owning (GUI) thread, so all shared state must be guarded.
    mutable std::mutex m_mutex;
};

} // namespace media

#endif // MP4_RECORDER_H
