/**
 * @file voice_config.cpp
 * @brief VoiceConfig parsing implementation (Qt-free, unit-testable).
 */

#include "voice_config.h"
#include "logmanager.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace voice {

namespace {

const char *const MODULE = "VoiceConfig";

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

void trim(std::string &s)
{
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    if (s.empty()) {
        return;
    }
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
}

} // namespace

bool parseBool(const std::string &value, bool defaultValue)
{
    std::string v = toLower(value);
    trim(v);
    if (v == "true" || v == "1" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off") {
        return false;
    }
    VLOG_WARN(MODULE, "illegal boolean value '%s', using safe default %s",
              value.c_str(), defaultValue ? "true" : "false");
    return defaultValue;
}

VoiceConfig loadVoiceConfig(const std::string &path)
{
    VoiceConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        VLOG_WARN(MODULE, "cannot open voice config: %s", path.c_str());
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        trim(key);
        trim(val);

        if (key == "voice_enabled") {
            config.enabled = parseBool(val, false);
        } else if (key == "llm_enabled") {
            config.llmEnabled = parseBool(val, false);
        } else if (key == "tts_enabled") {
            config.ttsEnabled = parseBool(val, false);
        } else if (key == "direct_command_enabled") {
            config.directCommandEnabled = parseBool(val, true);
        } else if (key == "unknown_command_fallback_to_llm") {
            config.unknownCommandFallbackToLlm = parseBool(val, true);
        } else if (key == "tts_feedback_for_command") {
            config.ttsFeedbackForCommand = parseBool(val, false);
        } else if (key == "tts_feedback_mute_ms") {
            try {
                size_t pos = 0;
                int parsed = std::stoi(val, &pos);
                if (pos == val.size() && parsed >= 0) {
                    config.ttsFeedbackMuteMs = parsed;
                } else {
                    VLOG_WARN(MODULE,
                              "tts_feedback_mute_ms must be a non-negative integer: '%s', using default %d",
                              val.c_str(), config.ttsFeedbackMuteMs);
                }
            } catch (const std::exception &e) {
                VLOG_WARN(MODULE, "invalid tts_feedback_mute_ms: '%s', using default %d",
                          val.c_str(), config.ttsFeedbackMuteMs);
            }
        } else if (key == "llm_bin_path") {
            config.llmBinPath = val;
        } else if (key == "llm_model_path") {
            config.llmModelPath = val;
        } else if (key == "tts_bin_path") {
            config.ttsBinPath = val;
        } else if (key == "tts_model_path") {
            config.ttsModelPath = val;
        } else if (key == "asr_config_path") {
            config.asrConfigPath = val;
        } else if (key == "kws_config_path") {
            config.kwsConfigPath = val;
        } else if (key == "llm_address") {
            config.llmAddress = val;
        } else if (key == "tts_address") {
            config.ttsAddress = val;
        } else if (key == "tts_status_address") {
            config.ttsStatusAddress = val;
        } else if (key == "zmq_timeout_ms") {
            try {
                size_t pos = 0;
                int parsed = std::stoi(val, &pos);
                // Reject partial parses like "5000ms" or "abc5000".
                if (pos == val.size() && parsed > 0) {
                    config.zmqTimeoutMs = parsed;
                } else {
                    VLOG_WARN(MODULE,
                              "zmq_timeout_ms must be a positive integer: '%s', using default %d",
                              val.c_str(), config.zmqTimeoutMs);
                }
            } catch (const std::exception &e) {
                VLOG_WARN(MODULE, "invalid zmq_timeout_ms: '%s', using default %d",
                          val.c_str(), config.zmqTimeoutMs);
            }
        }
    }

    VLOG_INFO(MODULE,
              "loaded voice config: enabled=%d llm=%d tts=%d direct=%d fallback=%d tts_feedback=%d tts_mute_ms=%d",
              config.enabled ? 1 : 0,
              config.llmEnabled ? 1 : 0,
              config.ttsEnabled ? 1 : 0,
              config.directCommandEnabled ? 1 : 0,
              config.unknownCommandFallbackToLlm ? 1 : 0,
              config.ttsFeedbackForCommand ? 1 : 0,
              config.ttsFeedbackMuteMs);
    return config;
}

VoiceRoute decideVoiceRoute(bool commandMatched, bool shouldFallback, bool llmAvailable)
{
    // Invariant (section 六): a matched fixed command NEVER routes to LLM,
    // regardless of LLM availability or fallback flag. This guards against
    // accidental double-dispatch (GUI + LLM) and prevents fixed commands
    // from being forwarded to the LLM.
    if (commandMatched) {
        return VoiceRoute::DirectCommand;
    }

    // Unmatched command: only contact LLM when ALL three conditions hold:
    //   1. unknown_command_fallback_to_llm == true (shouldFallback)
    //   2. llm_enabled == true (caller-visible, surfaced via llmAvailable)
    //   3. LLM process running && ZMQ client ready (also via llmAvailable)
    if (shouldFallback && llmAvailable) {
        return VoiceRoute::SendToLlm;
    }

    // LLM not available, disabled, or fallback disabled.
    // Caller logs and updates status; never crashes or blocks the GUI.
    return VoiceRoute::LogUnavailable;
}

} // namespace voice
