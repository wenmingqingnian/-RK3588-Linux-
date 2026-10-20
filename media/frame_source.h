#ifndef MEDIA_FRAME_SOURCE_H
#define MEDIA_FRAME_SOURCE_H

#include "frame_packet.h"

#include <string>

// Abstract frame source contract for the media pipeline (task §四).
//
// A source produces FramePacket frames on demand via readFrame(). The pipeline
// owns a single source thread that loops on readFrame() and dispatches each
// frame to the broker. Sources must be interruptible: requestStop() must
// unblock a pending readFrame() within a bounded time so the pipeline can shut
// down safely without QThread::terminate().
//
// Error reporting uses std::string (not QString) so sources are Qt-free and
// unit-testable with plain g++, consistent with the project's existing gps/
// alarm/buzzer test pattern.
//
// Lifecycle:
//   open() -> [readFrame() loop] -> requestStop() -> close()
//   open() may be called again after close() for repeated start/stop cycles.

namespace media {

enum class FrameReadResult {
    FrameReady,       // a frame was written to *frame
    Timeout,          // no frame within timeoutMs; safe to retry
    EndOfStream,      // source exhausted (e.g. file EOF with loop disabled)
    Stopped,          // requestStop() was called; readFrame returned promptly
    RecoverableError, // transient error (e.g. EAGAIN on V4L2); may retry
    FatalError,       // unrecoverable; source must be closed and reopened
};

inline const char *frameReadResultToString(FrameReadResult result)
{
    switch (result) {
    case FrameReadResult::FrameReady:      return "FrameReady";
    case FrameReadResult::Timeout:         return "Timeout";
    case FrameReadResult::EndOfStream:     return "EndOfStream";
    case FrameReadResult::Stopped:         return "Stopped";
    case FrameReadResult::RecoverableError:return "RecoverableError";
    case FrameReadResult::FatalError:      return "FatalError";
    }
    return "Unknown";
}

class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    // Open the source and verify it can produce frames. Must return false on
    // any failure (missing file, device open error, unsupported format). On
    // success the source is ready for readFrame(). On failure *errorMessage
    // (if non-null) is set to a human-readable cause.
    //
    // Must be idempotent against repeated open after close.
    virtual bool open(std::string *errorMessage) = 0;

    // Read one frame with a bounded timeout. Returns FrameReady on success
    // with the frame written to *frame. Must respond to requestStop() by
    // returning Stopped promptly (within a small bounded time, not the full
    // timeoutMs).
    virtual FrameReadResult readFrame(FramePacket *frame,
                                      int timeoutMs,
                                      std::string *errorMessage) = 0;

    // Request the source to stop. Must be safe to call from a different
    // thread than readFrame(). Must unblock a pending readFrame().
    // Idempotent: multiple calls are safe.
    virtual void requestStop() = 0;

    // Release all resources (file descriptors, mmap, buffers). The source
    // may be reopened with open() after close(). Idempotent.
    virtual void close() = 0;
};

} // namespace media

#endif // MEDIA_FRAME_SOURCE_H
