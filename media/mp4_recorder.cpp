// MP4 video recorder implementation for RK3588S CarTerminal.
//
// Wraps FFmpeg libavformat MP4 muxer to record the Annex-B H.264/H.265 byte
// stream already produced by RtspServer's MPP encoder. Re-encoding is
// intentionally avoided because the MPP encoder is a process-wide singleton
// with static state and cannot be called concurrently.
//
// Per-frame flow (writeFrame):
//   1. Lock mutex.
//   2. First keyframe: extract SPS/PPS/VPS → avcC/hvcC extradata.
//   3. Segment switch check (only at keyframe boundary for clean random
//      access in every segment).
//   4. Annex-B → AVCC conversion (length-prefixed NAL units, no SPS/PPS).
//   5. Build AVPacket (data + PTS in stream time_base) and av_write_frame.
//
// Lifecycle: init() -> start() -> writeFrame()* -> stop() -> deinit()

#include "mp4_recorder.h"
#include "logmanager.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <utility>   // std::pair
#include <vector>

static const char *MODULE = "recorder";

// H.264 NAL unit types (RFC 6184 Section 5.2).
static constexpr uint8_t NAL_TYPE_SPS = 7;
static constexpr uint8_t NAL_TYPE_PPS = 8;

// H.265 NAL unit types (HEVC spec Section 7.4.2.2).
static constexpr uint8_t HEVC_NAL_VPS = 32;
static constexpr uint8_t HEVC_NAL_SPS = 33;
static constexpr uint8_t HEVC_NAL_PPS = 34;

namespace media {

// ---------------------------------------------------------------------------
// recorderStatusToString
// ---------------------------------------------------------------------------

const char *recorderStatusToString(RecorderStatus status) {
    switch (status) {
    case RecorderStatus::Success:           return "success";
    case RecorderStatus::Disabled:          return "disabled in config";
    case RecorderStatus::InvalidConfig:     return "invalid config";
    case RecorderStatus::OutputDirFailed:    return "output dir create failed";
    case RecorderStatus::MuxerAllocFailed:   return "muxer alloc failed";
    case RecorderStatus::StreamAddFailed:    return "stream add failed";
    case RecorderStatus::HeaderWriteFailed:  return "header write failed";
    case RecorderStatus::NotInitialized:     return "not initialized";
    case RecorderStatus::AlreadyRunning:      return "already running";
    case RecorderStatus::SegmentOpenFailed:   return "segment open failed";
    case RecorderStatus::NotRunning:         return "not running";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Annex-B scanner (file-local helper)
//
// Splits an Annex-B byte stream into individual NAL units by scanning for
// 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes. Returns a list
// of (naluPtr, naluSize) pairs pointing into the input buffer (no copy).
// ---------------------------------------------------------------------------

using NaluView = std::pair<const uint8_t *, size_t>;

static void scanAnnexBNalus(const uint8_t *data, size_t size,
                            std::vector<NaluView> &nalus) {
    nalus.clear();
    size_t i = 0;
    while (i + 3 <= size) {
        // Detect 4-byte start code (00 00 00 01).
        bool sc4 = (i + 4 <= size) &&
                   data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1;
        // Detect 3-byte start code (00 00 01).
        bool sc3 = data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1;

        if (!sc4 && !sc3) {
            i++;
            continue;
        }

        const size_t hdrLen = sc4 ? 4 : 3;
        const size_t naluStart = i + hdrLen;

        // Find the next start code (or end of buffer).
        size_t j = naluStart;
        while (j + 3 <= size) {
            const bool next4 = (j + 4 <= size) &&
                               data[j] == 0 && data[j + 1] == 0 &&
                               data[j + 2] == 0 && data[j + 3] == 1;
            const bool next3 = data[j] == 0 && data[j + 1] == 0 &&
                               data[j + 2] == 1;
            if (next4 || next3) {
                break;
            }
            j++;
        }

        // If no following start code was found, the scan loop stops when
        // fewer than three bytes remain. Those remaining bytes still belong
        // to the current NAL unit and must not be discarded.
        if (size - j < 3) {
            j = size;
        }

        // NAL unit ends at j (exclusive).
        const size_t naluSize = j - naluStart;
        if (naluSize > 0) {
            nalus.emplace_back(data + naluStart, naluSize);
        }
        i = naluStart + naluSize;
        if (i < j) i = j;  // skip to next start code
    }
}

// ---------------------------------------------------------------------------
// Mp4Recorder: ctor / dtor
// ---------------------------------------------------------------------------

Mp4Recorder::Mp4Recorder() = default;

Mp4Recorder::~Mp4Recorder() {
    deinit();
}

// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------

RecorderStatus Mp4Recorder::init(const RecordingConfig &config) {
    if (m_initialized) {
        VLOG_WARN(MODULE, "init called twice; ignoring");
        return RecorderStatus::Success;
    }

    std::string errorMessage;
    if (!validateRecordingConfig(config, &errorMessage)) {
        VLOG_ERROR(MODULE, "config invalid: %s", errorMessage.c_str());
        return RecorderStatus::InvalidConfig;
    }

    m_config = config;

    if (!m_config.enabled) {
        VLOG_INFO(MODULE, "recording disabled in config");
        m_initialized = true;
        return RecorderStatus::Disabled;
    }

    // Ensure output directory exists (single-level mkdir, like rtsp_server).
    // For nested paths the user must pre-create parent directories.
    std::lock_guard<std::mutex> lock(m_mutex);
    struct stat st;
    if (::stat(m_config.outputDir.c_str(), &st) != 0) {
        if (::mkdir(m_config.outputDir.c_str(), 0755) != 0) {
            VLOG_ERROR(MODULE, "mkdir %s failed: %s",
                       m_config.outputDir.c_str(), std::strerror(errno));
            return RecorderStatus::OutputDirFailed;
        }
        VLOG_INFO(MODULE, "created output dir: %s", m_config.outputDir.c_str());
    }

    m_initialized = true;
    VLOG_INFO(MODULE,
              "initialized: dir=%s seg=%ds max=%d %dx%d@%dfps codec=%s",
              m_config.outputDir.c_str(),
              m_config.segmentDurationSec, m_config.maxSegments,
              m_config.width, m_config.height, m_config.fps,
              m_config.codec.c_str());
    return RecorderStatus::Success;
}

void Mp4Recorder::deinit() {
    if (m_running.load()) {
        stop();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    closeCurrentSegment();

    if (m_packet) {
        av_packet_free(&m_packet);
    }

    m_extradata.clear();
    m_extradataReady = false;
    m_initialized = false;
    VLOG_INFO(MODULE, "deinitialized");
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

RecorderStatus Mp4Recorder::start() {
    if (!m_initialized) {
        VLOG_ERROR(MODULE, "start called before init");
        return RecorderStatus::NotInitialized;
    }
    if (!m_config.enabled) {
        return RecorderStatus::Disabled;
    }
    if (m_running.load()) {
        VLOG_WARN(MODULE, "already running");
        return RecorderStatus::AlreadyRunning;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // Allocate reusable AVPacket (av_write_frame does not retain it).
    if (!m_packet) {
        m_packet = av_packet_alloc();
        if (!m_packet) {
            VLOG_ERROR(MODULE, "av_packet_alloc failed");
            return RecorderStatus::MuxerAllocFailed;
        }
    }

    // Reset per-session bookkeeping.
    m_firstFrameTimestampUs = -1;
    m_lastFrameTimestampUs = 0;
    m_lastPacketPts = -1;
    m_segmentIndex = 0;

    // Open the first segment.
    if (!openNewSegment()) {
        return RecorderStatus::SegmentOpenFailed;
    }

    m_running.store(true);
    VLOG_INFO(MODULE, "recording started: %s",
              m_currentSegmentPath.c_str());
    return RecorderStatus::Success;
}

void Mp4Recorder::stop() {
    if (!m_running.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    closeCurrentSegment();
    m_running.store(false);
    VLOG_INFO(MODULE, "recording stopped; total segments=%zu",
              m_segmentFiles.size());
}

// ---------------------------------------------------------------------------
// writeFrame: main entry — Annex-B → AVCC → MP4 muxer
// ---------------------------------------------------------------------------

bool Mp4Recorder::writeFrame(const uint8_t *annexBData, size_t size,
                              bool isKeyFrame, int64_t timestampUs) {
    if (!m_running.load()) {
        return false;
    }
    if (!annexBData || size == 0) {
        return false;
    }

    // Auto-generate timestamp from steady_clock when caller passes 0.
    if (timestampUs == 0) {
        auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
        timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
            sinceEpoch).count();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_fmtCtx || !m_videoStream) {
        return false;
    }

    // First-frame PTS anchor (used for relative PTS within a segment).
    if (m_firstFrameTimestampUs < 0) {
        m_firstFrameTimestampUs = timestampUs;
    }
    m_lastFrameTimestampUs = timestampUs;

    // Extract SPS/PPS/VPS from the first keyframe → extradata (avcC/hvcC).
    // Required before avformat_write_header(); runs once per session.
    if (!m_extradataReady && isKeyFrame) {
        if (!extractExtradata(annexBData, size)) {
            VLOG_WARN(MODULE,
                      "extradata extraction failed; deferring to next keyframe");
            return false;
        }
        m_extradataReady = true;
        VLOG_INFO(MODULE, "extradata extracted: %zu bytes",
                  m_extradata.size());

        // Extradata was not yet available when openNewSegment ran for the
        // first segment. Patch it in now so the MP4 moov atom is valid.
        // For simplicity we close and reopen the first segment here.
        // (Alternative: defer openNewSegment until first keyframe.)
        // — reopen path chosen for clarity over a deferred-open state.
        if (m_headerWritten && m_videoStream) {
            AVCodecParameters *codecpar = m_videoStream->codecpar;
            if (codecpar->extradata) {
                av_freep(&codecpar->extradata);
                codecpar->extradata_size = 0;
            }
            codecpar->extradata = static_cast<uint8_t *>(
                av_mallocz(m_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (codecpar->extradata) {
                std::memcpy(codecpar->extradata, m_extradata.data(),
                            m_extradata.size());
                codecpar->extradata_size = static_cast<int>(m_extradata.size());
            }
        }
    }

    // Segment switch — only at keyframe boundary so every segment starts
    // with an IDR for clean random access (VLC seek/scrub works correctly).
    if (isKeyFrame && shouldSwitchSegment()) {
        VLOG_DEBUG(MODULE, "switching segment at ts=%lldus",
                   static_cast<long long>(timestampUs));
        closeCurrentSegment();
        if (!openNewSegment()) {
            VLOG_ERROR(MODULE, "segment open failed; stopping recorder");
            m_running.store(false);
            return false;
        }
        // Reset per-segment PTS state so the new file starts at PTS=0.
        m_firstFrameTimestampUs = timestampUs;
        m_lastPacketPts = -1;
    }

    // Convert Annex-B → AVCC (length-prefixed NAL units; SPS/PPS/VPS dropped
    // because they live in extradata, not in per-frame packets).
    std::vector<uint8_t> avccData;
    const bool isH265 = (m_config.codec == "265");
    if (!annexBToAvcc(annexBData, size, isH265, avccData)) {
        VLOG_WARN(MODULE, "annexB→AVCC conversion failed; dropping frame");
        return false;
    }

    // Build AVPacket and write it.
    av_packet_unref(m_packet);
    m_packet->data = avccData.data();
    m_packet->size = static_cast<int>(avccData.size());
    m_packet->stream_index = m_videoStream->index;
    m_packet->flags = isKeyFrame ? AV_PKT_FLAG_KEY : 0;

    // Convert microsecond timestamp → AVStream time_base (1/fps).
    // Use av_rescale_q for safe rational scaling without overflow.
    const AVRational us_tb = {1, 1000000};
    const int64_t relUs = timestampUs - m_firstFrameTimestampUs;
    int64_t packetPts =
        av_rescale_q(relUs, us_tb, m_videoStream->time_base);

    // Timestamp quantization at a 1/fps time base can map adjacent camera
    // frames to the same integer PTS. MP4 packets require strictly increasing
    // DTS for this no-B-frame encoder path.
    if (packetPts <= m_lastPacketPts) {
        packetPts = m_lastPacketPts + 1;
    }

    m_packet->pts = packetPts;
    m_packet->dts = packetPts;
    m_packet->duration = 1;
    m_lastPacketPts = packetPts;

    int writeRet = av_write_frame(m_fmtCtx, m_packet);
    if (writeRet < 0) {
        VLOG_WARN(MODULE, "av_write_frame failed: ret=%d size=%d",
                  writeRet, m_packet->size);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Segment management (private)
// ---------------------------------------------------------------------------

bool Mp4Recorder::openNewSegment() {
    m_segmentIndex++;
    m_currentSegmentPath = buildSegmentPath();

    // Allocate output format context for MP4 container.
    AVFormatContext *fmtCtx = nullptr;
    int ret = avformat_alloc_output_context2(&fmtCtx, nullptr, "mp4",
                                              m_currentSegmentPath.c_str());
    if (ret < 0 || !fmtCtx) {
        VLOG_ERROR(MODULE,
                   "avformat_alloc_output_context2 failed: ret=%d path=%s",
                   ret, m_currentSegmentPath.c_str());
        return false;
    }

    // Create video stream.
    AVStream *videoStream = avformat_new_stream(fmtCtx, nullptr);
    if (!videoStream) {
        VLOG_ERROR(MODULE, "avformat_new_stream failed");
        avformat_free_context(fmtCtx);
        return false;
    }

    // Configure codec parameters.
    AVCodecParameters *codecpar = videoStream->codecpar;
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = (m_config.codec == "265")
                          ? AV_CODEC_ID_H265
                          : AV_CODEC_ID_H264;
    codecpar->width = m_config.width;
    codecpar->height = m_config.height;
    codecpar->format = AV_PIX_FMT_YUV420P;

    // time_base = 1/fps. MP4 muxer will store as a fraction.
    videoStream->time_base.num = 1;
    videoStream->time_base.den = m_config.fps;

    // Attach extradata (avcC / hvcC) if already extracted from a keyframe.
    // On the very first segment this may be empty; writeFrame() patches it
    // into codecpar before the first keyframe is written.
    if (m_extradataReady && !m_extradata.empty()) {
        codecpar->extradata = static_cast<uint8_t *>(
            av_mallocz(m_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (codecpar->extradata) {
            std::memcpy(codecpar->extradata, m_extradata.data(),
                        m_extradata.size());
            codecpar->extradata_size = static_cast<int>(m_extradata.size());
        }
    }

    // Open the output file (unless muxer is AVFMT_NOFILE).
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmtCtx->pb, m_currentSegmentPath.c_str(),
                        AVIO_FLAG_WRITE);
        if (ret < 0) {
            VLOG_ERROR(MODULE, "avio_open(%s) failed: ret=%d",
                       m_currentSegmentPath.c_str(), ret);
            avformat_free_context(fmtCtx);
            return false;
        }
    }

    // Write MP4 header (ftyp + moov with codecpar).
    ret = avformat_write_header(fmtCtx, nullptr);
    if (ret < 0) {
        VLOG_ERROR(MODULE, "avformat_write_header failed: ret=%d", ret);
        avio_closep(&fmtCtx->pb);
        avformat_free_context(fmtCtx);
        return false;
    }

    m_fmtCtx = fmtCtx;
    m_videoStream = videoStream;
    m_headerWritten = true;
    m_segmentStartTime = std::chrono::steady_clock::now();

    VLOG_INFO(MODULE, "segment opened: %s (idx=%d)",
              m_currentSegmentPath.c_str(), m_segmentIndex);
    return true;
}

void Mp4Recorder::closeCurrentSegment() {
    if (!m_fmtCtx) {
        return;
    }

    if (m_headerWritten) {
        if (av_write_trailer(m_fmtCtx) < 0) {
            VLOG_WARN(MODULE, "av_write_trailer failed for %s",
                      m_currentSegmentPath.c_str());
        }
        m_headerWritten = false;
    }

    if (m_fmtCtx->pb) {
        avio_closep(&m_fmtCtx->pb);
    }

    // Track segment file for old-segment cleanup.
    if (!m_currentSegmentPath.empty()) {
        m_segmentFiles.push_back(m_currentSegmentPath);
        cleanupOldSegments();
    }

    avformat_free_context(m_fmtCtx);
    m_fmtCtx = nullptr;
    m_videoStream = nullptr;

    VLOG_INFO(MODULE, "segment closed: %s",
              m_currentSegmentPath.empty() ? "(none)"
                                          : m_currentSegmentPath.c_str());
    m_currentSegmentPath.clear();
}

void Mp4Recorder::cleanupOldSegments() {
    while (static_cast<int>(m_segmentFiles.size()) > m_config.maxSegments) {
        const std::string oldest = m_segmentFiles.front();
        m_segmentFiles.erase(m_segmentFiles.begin());
        if (::unlink(oldest.c_str()) != 0) {
            VLOG_WARN(MODULE, "unlink(%s) failed: %s",
                      oldest.c_str(), std::strerror(errno));
        } else {
            VLOG_INFO(MODULE, "old segment deleted: %s", oldest.c_str());
        }
    }
}

bool Mp4Recorder::shouldSwitchSegment() const {
    if (m_config.segmentDurationSec <= 0) {
        return false;
    }
    const auto elapsed = std::chrono::steady_clock::now() - m_segmentStartTime;
    const auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
        elapsed).count();
    return elapsedSec >= m_config.segmentDurationSec;
}

std::string Mp4Recorder::buildSegmentPath() const {
    char filename[256];
    std::snprintf(filename, sizeof(filename), "%s_%04d.mp4",
                  m_config.filenamePrefix.c_str(), m_segmentIndex);
    std::string path = m_config.outputDir;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += filename;
    return path;
}

// ---------------------------------------------------------------------------
// Annex-B → AVCC conversion (private)
// ---------------------------------------------------------------------------

bool Mp4Recorder::annexBToAvcc(const uint8_t *annexBData, size_t size,
                                bool isH265, std::vector<uint8_t> &avccOut) {
    avccOut.clear();

    std::vector<NaluView> nalus;
    scanAnnexBNalus(annexBData, size, nalus);
    if (nalus.empty()) {
        return false;
    }

    // Each NAL becomes [4-byte big-endian length][nalu bytes].
    // Skip SPS/PPS/VPS — they are stored in extradata, not per-frame.
    for (const auto &nv : nalus) {
        const uint8_t *nalu = nv.first;
        const size_t naluSize = nv.second;
        if (naluSize == 0) {
            continue;
        }

        const uint8_t firstByte = nalu[0];
        const uint8_t h264Type = firstByte & 0x1F;
        const uint8_t h265Type = (firstByte >> 1) & 0x3F;

        const bool isParamSet = isH265
            ? (h265Type == HEVC_NAL_VPS ||
               h265Type == HEVC_NAL_SPS ||
               h265Type == HEVC_NAL_PPS)
            : (h264Type == NAL_TYPE_SPS || h264Type == NAL_TYPE_PPS);
        if (isParamSet) {
            continue;
        }

        // 4-byte big-endian length prefix.
        const uint32_t lenN = static_cast<uint32_t>(naluSize);
        avccOut.push_back(static_cast<uint8_t>((lenN >> 24) & 0xFF));
        avccOut.push_back(static_cast<uint8_t>((lenN >> 16) & 0xFF));
        avccOut.push_back(static_cast<uint8_t>((lenN >> 8) & 0xFF));
        avccOut.push_back(static_cast<uint8_t>(lenN & 0xFF));
        avccOut.insert(avccOut.end(), nalu, nalu + naluSize);
    }
    return !avccOut.empty();
}

bool Mp4Recorder::extractExtradata(const uint8_t *annexBData, size_t size) {
    std::vector<NaluView> nalus;
    scanAnnexBNalus(annexBData, size, nalus);

    const uint8_t *sps = nullptr; size_t spsSize = 0;
    const uint8_t *pps = nullptr; size_t ppsSize = 0;
    const uint8_t *vps = nullptr; size_t vpsSize = 0;

    const bool isH265 = (m_config.codec == "265");

    for (const auto &nv : nalus) {
        const uint8_t *nalu = nv.first;
        const size_t naluSize = nv.second;
        if (naluSize == 0) {
            continue;
        }

        if (isH265) {
            const uint8_t nalType = (nalu[0] >> 1) & 0x3F;
            if (nalType == HEVC_NAL_VPS)      { vps = nalu; vpsSize = naluSize; }
            else if (nalType == HEVC_NAL_SPS) { sps = nalu; spsSize = naluSize; }
            else if (nalType == HEVC_NAL_PPS) { pps = nalu; ppsSize = naluSize; }
        } else {
            const uint8_t nalType = nalu[0] & 0x1F;
            if (nalType == NAL_TYPE_SPS)      { sps = nalu; spsSize = naluSize; }
            else if (nalType == NAL_TYPE_PPS) { pps = nalu; ppsSize = naluSize; }
        }
    }

    if (!sps || !pps) {
        return false;
    }

    if (isH265) {
        // hvcC (HEVCDecoderConfigurationRecord) — ISO/IEC 14496-15 §8.3.3.1.
        // We build a minimal valid record; muxer accepts and stores as-is.
        std::vector<uint8_t> hvcc;
        hvcc.resize(23, 0);  // base header is 23 bytes (incl. numOfArrays byte)

        hvcc[0] = 1;  // configurationVersion
        // Copy profile_tier_level fields from SPS (offset by 2-byte NALU hdr).
        // SPS bytes: [0..1]=NALU header, [2..]=profile_tier_level.
        if (spsSize >= 12) {
            hvcc[1]  = sps[2];  // general_profile_space+tier+profile_idc
            hvcc[2]  = sps[3];  // general_profile_compatibility_flags[0..7]
            hvcc[3]  = sps[4];  // [8..15]
            hvcc[4]  = sps[5];  // [16..23]
            hvcc[5]  = sps[6];  // [24..31]
            hvcc[6]  = sps[7];  // general_constraint_indicator_flags[0..7]
            hvcc[7]  = sps[8];  // [8..15]
            hvcc[8]  = sps[9];  // [16..23]
            hvcc[9]  = sps[10]; // [24..31]
            hvcc[10] = sps[11]; // [32..39]
            hvcc[11] = (spsSize > 12) ? sps[12] : 0;  // general_level_idc
        }
        hvcc[12] = 0xF0;  // min_spatial_segmentation = 0 (reserved bits = 0xF)
        hvcc[13] = 0xFC;  // parallelismType = 0 (reserved = 0xFC)
        hvcc[14] = 0xFC | 1;  // chroma_format_idc = 1 (4:2:0)
        hvcc[15] = 0xF8 | 0;  // bit_depth_luma_minus8 = 0 (8-bit)
        hvcc[16] = 0xF8 | 0;  // bit_depth_chroma_minus8 = 0
        hvcc[17] = 0;          // avg_frame_rate high byte
        hvcc[18] = 0;          // avg_frame_rate low byte
        // constantFrameRate=0, numTemporalLayers=1, temporalIdNested=1,
        // lengthSizeMinusOne=3 (4-byte length prefix).
        hvcc[19] = (0 << 6) | (1 << 3) | (1 << 2) | 3;
        hvcc[20] = 0x80;  // reserved = 0x80, numOfArrays follows at byte 22
        hvcc[21] = 0;     // reserved
        hvcc[22] = vps ? 3 : 2;  // numOfArrays (VPS+SPS+PPS or SPS+PPS)

        // Append each parameter-set array.
        // Format: [array_completeness:1 | nal_type:7][numNalus:2B]
        //         for each nalu: [naluLength:2B][nalu bytes]
        const auto appendArray = [&hvcc](uint8_t nalType,
                                          const uint8_t *data, size_t len) {
            hvcc.push_back(0x80 | (nalType & 0x7F));  // completeness=1
            hvcc.push_back(0);  // numNalus high
            hvcc.push_back(1);  // numNalus low
            hvcc.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            hvcc.push_back(static_cast<uint8_t>(len & 0xFF));
            hvcc.insert(hvcc.end(), data, data + len);
        };

        if (vps) appendArray(HEVC_NAL_VPS, vps, vpsSize);
        appendArray(HEVC_NAL_SPS, sps, spsSize);
        appendArray(HEVC_NAL_PPS, pps, ppsSize);

        m_extradata = std::move(hvcc);
    } else {
        // avcC (AVCDecoderConfigurationRecord) — ISO/IEC 14496-15 §5.2.4.1.
        // SPS/PPS NALU data includes the 1-byte NALU header (no start code).
        //   byte 0: configurationVersion = 1
        //   byte 1: AVCProfileIndication  = SPS[1] (profile_idc)
        //   byte 2: profile_compatibility = SPS[2]
        //   byte 3: AVCLevelIndication    = SPS[3] (level_idc)
        //   byte 4: 0xFF (reserved=0x3F, lengthSizeMinusOne=3)
        //   byte 5: 0xE1 (reserved=0x07, numSPS=1)
        //   bytes 6..7: SPS length (big-endian)
        //   bytes 8..8+spsSize-1: SPS NALU data
        //   byte 8+spsSize: numPPS = 1
        //   bytes 9+spsSize..10+spsSize: PPS length
        //   bytes 11+spsSize..: PPS NALU data
        std::vector<uint8_t> avcc;
        avcc.reserve(8 + spsSize + 2 + ppsSize + 2);
        avcc.push_back(1);  // configurationVersion
        avcc.push_back(spsSize >= 2 ? sps[1] : 0);  // profile_idc
        avcc.push_back(spsSize >= 3 ? sps[2] : 0);  // profile_compatibility
        avcc.push_back(spsSize >= 4 ? sps[3] : 0);  // level_idc
        avcc.push_back(0xFF);  // lengthSizeMinusOne = 3
        avcc.push_back(0xE1);  // numSPS = 1
        avcc.push_back(static_cast<uint8_t>((spsSize >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(spsSize & 0xFF));
        avcc.insert(avcc.end(), sps, sps + spsSize);
        avcc.push_back(1);  // numPPS = 1
        avcc.push_back(static_cast<uint8_t>((ppsSize >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(ppsSize & 0xFF));
        avcc.insert(avcc.end(), pps, pps + ppsSize);

        m_extradata = std::move(avcc);
    }

    return !m_extradata.empty();
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::string Mp4Recorder::currentSegmentPath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentSegmentPath;
}

std::string Mp4Recorder::getStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string status = "recorder: ";
    if (!m_initialized) {
        status += "not_initialized";
    } else if (!m_config.enabled) {
        status += "disabled";
    } else if (!m_running.load()) {
        status += "stopped segments=" + std::to_string(m_segmentFiles.size());
    } else {
        status += "running ";
        status += m_currentSegmentPath;
        status += " total=" + std::to_string(m_segmentFiles.size());
    }
    return status;
}

} // namespace media
