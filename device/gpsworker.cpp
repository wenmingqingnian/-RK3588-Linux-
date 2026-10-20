#include "gpsworker.h"

#include "logmanager.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

[[maybe_unused]] const char *GPS_WORKER_MODULE = "gps-worker";

// Read buffer for one serial readChunk() call. Larger than a single NMEA
// sentence (max 256) so a burst of sentences is collected in one read.
constexpr std::size_t kReadChunkSize = 256;

// Simulation sleep step. The simulation loop sleeps in small increments so a
// stop request is noticed within this interval rather than waiting for the
// full simulation interval.
constexpr int kSimSleepStepMs = 50;

// Back-off applied after a serial read error. Without this, an unplugged or
// detached GPS device would cause a tight error loop that floods the log and
// burns CPU. The sleep is performed in kSimSleepStepMs increments so stop()
// is still noticed within that interval.
constexpr int kReadErrorBackoffMs = 200;

// Status strings emitted to the GUI. Defined once to avoid typos.
const char *const kStatusGpsOff       = "GPS已关闭";
const char *const kStatusDeviceFailed = "GPS设备不可用";
const char *const kStatusNoFix        = "GPS未定位";
const char *const kStatusFixed        = "GPS已定位";
const char *const kStatusSimulating   = "GPS模拟定位";
const char *const kStatusDataError    = "GPS数据异常";

} // namespace

GpsWorker::GpsWorker(const vehicledevice::GpsSerialConfig &serialConfig,
                     const vehicledevice::GpsSimulationConfig &simulationConfig,
                     QObject *parent)
    : QObject(parent),
      serialConfig_(serialConfig),
      simulationConfig_(simulationConfig),
      device_(serialConfig),
      running_(false)
{
    // Register the GpsFix type for queued cross-thread signal/slot delivery.
    // qRegisterMetaType must be called before the first signal emission with
    // a queued connection. The id is intentionally unused.
    [[maybe_unused]] const int metaTypeId =
        qRegisterMetaType<vehiclegps::GpsFix>("vehiclegps::GpsFix");
}

GpsWorker::~GpsWorker()
{
    stop();
}

bool GpsWorker::isRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void GpsWorker::setRunning(bool value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = value;
}

void GpsWorker::start()
{
    if (isRunning()) {
        return;
    }

    // Both modes disabled — nothing to do. The GUI will show "GPS已关闭".
    if (!serialConfig_.enabled && !simulationConfig_.enabled) {
        emit statusChanged(QString::fromUtf8(kStatusGpsOff));
        VLOG_INFO(GPS_WORKER_MODULE, "start: both gps and simulation disabled");
        return;
    }

    setRunning(true);

    // Pick the mode. The config parser already enforces mutual exclusion, but
    // we guard against both being enabled here as a defense-in-depth.
    if (simulationConfig_.enabled) {
        thread_ = std::thread(&GpsWorker::runSimulationLoop, this);
    } else {
        thread_ = std::thread(&GpsWorker::runRealGpsLoop, this);
    }

    VLOG_INFO(GPS_WORKER_MODULE, "start: worker thread launched (mode=%s)",
              simulationConfig_.enabled ? "simulation" : "real-gps");
}

void GpsWorker::stop()
{
    // Phase 9 stop sequence:
    //   1. Set running_=false under lock.
    //   2. Release lock (lock_guard scope ends).
    //   3. The thread loop notices running_ is false within a bounded interval
    //      (poll timeout ≤ readTimeoutMs, or sim sleep step ≤ 50ms).
    //   4. Join the thread — NO lock held.
    //   5. Close the device fd — NO lock held, thread already joined.
    setRunning(false);

    if (thread_.joinable()) {
        thread_.join();
    }

    // Device close happens here, after the thread has fully exited, so there is
    // no lock contention with readChunk(). For simulation mode this is a no-op
    // (device_ was never opened).
    device_.closeDevice();

    emit availabilityChanged(false);
    VLOG_INFO(GPS_WORKER_MODULE, "stop: worker thread joined, device released");
}

void GpsWorker::runRealGpsLoop()
{
    std::string errorMessage;
    if (!device_.openDevice(&errorMessage)) {
        VLOG_ERROR(GPS_WORKER_MODULE, "open device failed: %s",
                   errorMessage.c_str());
        emit warning(QString::fromUtf8("GPS设备打开失败: ") +
                     QString::fromStdString(errorMessage));
        emit statusChanged(QString::fromUtf8(kStatusDeviceFailed));
        setRunning(false);
        emit availabilityChanged(false);
        return;
    }

    // When gps_enabled is false, openDevice succeeds as a no-op but fd stays
    // -1. This branch is unreachable when serialConfig_.enabled is true
    // (checked in start()), but guard anyway.
    if (!device_.isAvailable()) {
        emit statusChanged(QString::fromUtf8(kStatusGpsOff));
        setRunning(false);
        emit availabilityChanged(false);
        return;
    }

    emit availabilityChanged(true);
    emit statusChanged(QString::fromUtf8(kStatusNoFix));

    VLOG_INFO(GPS_WORKER_MODULE, "real-gps loop started: device=%s",
              serialConfig_.devicePath.c_str());

    vehiclegps::NmeaStreamBuffer stream;
    char buffer[kReadChunkSize];
    bool hasFix = false;

    // Persistent fix accumulator. RMC and GGA sentences provide complementary
    // fields (RMC: speed/course/date; GGA: satellites/altitude/fixQuality).
    // parseNmeaSentence only fills the fields present in the parsed sentence
    // and leaves other fields unchanged, so feeding the same GpsFix across
    // sentences accumulates a complete fix without merge logic.
    vehiclegps::GpsFix aggregatedFix;

    while (isRunning()) {
        std::string readError;
        const int bytesRead = device_.readChunk(buffer, sizeof(buffer),
                                                &readError);
        if (bytesRead < 0) {
            VLOG_WARN(GPS_WORKER_MODULE, "read failed: %s",
                      readError.c_str());
            emit warning(QString::fromUtf8("GPS读取错误: ") +
                         QString::fromStdString(readError));
            // Back off before retrying. Without this, an unplugged or detached
            // device would cause a tight error loop flooding the log and
            // burning CPU. The sleep is interruptible in kSimSleepStepMs
            // increments so stop() is still noticed quickly.
            int waited = 0;
            while (isRunning() && waited < kReadErrorBackoffMs) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kSimSleepStepMs));
                waited += kSimSleepStepMs;
            }
            continue;
        }

        if (bytesRead == 0) {
            // Timeout, no data. Loop and check running_ again.
            continue;
        }

        std::vector<std::string> sentences;
        stream.feed(buffer, static_cast<std::size_t>(bytesRead), &sentences);

        for (std::size_t i = 0; i < sentences.size(); ++i) {
            const std::string &sentence = sentences[i];

            // Only RMC/GGA sentences carry position data; other NMEA types
            // (GSA/GSV/VTG/ZDA/TXT) parse successfully but leave outFix
            // unchanged. Emitting on them would re-broadcast the stale last
            // fix once per sentence. Quick type-prefix check: sentence is
            // "$xxXYZ,..." where xx is a 2-char talker ID and XYZ is the
            // 3-char sentence type. Take chars [3..5] and require RMC/GGA.
            if (sentence.size() < 6) {
                continue;
            }
            const std::string type3 = sentence.substr(3, 3);
            if (type3 != "RMC" && type3 != "GGA") {
                continue;
            }

            // Parse into aggregatedFix to accumulate fields across RMC/GGA.
            if (!vehiclegps::parseNmeaSentence(sentence, &aggregatedFix)) {
                // Parse error on one sentence does not affect the stream.
                // Invalid data never overwrites the last valid fix.
                VLOG_DEBUG(GPS_WORKER_MODULE, "parse failed: %s",
                           sentence.c_str());
                continue;
            }

            if (!aggregatedFix.valid) {
                // Recognized RMC/GGA sentence but status indicates no fix
                // (RMC status V or GGA fixQuality 0). Roll the worker state
                // back to "no fix" so the GUI stops showing the stale
                // position as if it were current.
                if (hasFix) {
                    hasFix = false;
                    emit statusChanged(QString::fromUtf8(kStatusNoFix));
                    VLOG_INFO(GPS_WORKER_MODULE, "fix lost");
                }
                continue;
            }

            // Valid fix from real device. Emit and update status.
            if (!hasFix) {
                hasFix = true;
                emit statusChanged(QString::fromUtf8(kStatusFixed));
                VLOG_INFO(GPS_WORKER_MODULE,
                          "first valid fix: lat=%.5f lon=%.5f speed=%.1f",
                          aggregatedFix.latitude, aggregatedFix.longitude,
                          aggregatedFix.speedKmh);
            }
            emit fixUpdated(aggregatedFix);
        }
    }

    device_.closeDevice();
    emit availabilityChanged(false);
    VLOG_INFO(GPS_WORKER_MODULE, "real-gps loop exited");
}

void GpsWorker::runSimulationLoop()
{
    if (!simulator_.loadFile(simulationConfig_.file)) {
        VLOG_ERROR(GPS_WORKER_MODULE, "simulation file load failed: %s",
                   simulationConfig_.file.c_str());
        emit warning(QString::fromUtf8("GPS模拟文件加载失败: ") +
                     QString::fromStdString(simulationConfig_.file));
        emit statusChanged(QString::fromUtf8(kStatusDataError));
        setRunning(false);
        emit availabilityChanged(false);
        return;
    }

    emit availabilityChanged(true);
    emit statusChanged(QString::fromUtf8(kStatusSimulating));

    VLOG_INFO(GPS_WORKER_MODULE, "simulation loop started: file=%s count=%u",
              simulationConfig_.file.c_str(),
              static_cast<unsigned int>(simulator_.sentenceCount()));

    // Persistent fix accumulator. The simulation file alternates RMC and GGA
    // sentences; nextFix parses directly into this struct, so RMC fields
    // (speed/course/date) and GGA fields (satellites/altitude/fixQuality)
    // accumulate across consecutive calls without merge logic.
    vehiclegps::GpsFix aggregatedFix;

    while (isRunning()) {
        // nextFix parses into aggregatedFix, preserving complementary fields
        // from the prior sentence. simulated is forced true by the simulator.
        if (simulator_.nextFix(&aggregatedFix)) {
            emit fixUpdated(aggregatedFix);
        }

        // Sleep for the simulation interval in small steps so stop() is
        // noticed within kSimSleepStepMs rather than the full interval.
        int waited = 0;
        while (isRunning() && waited < simulationConfig_.intervalMs) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSimSleepStepMs));
            waited += kSimSleepStepMs;
        }
    }

    emit availabilityChanged(false);
    VLOG_INFO(GPS_WORKER_MODULE, "simulation loop exited");
}
