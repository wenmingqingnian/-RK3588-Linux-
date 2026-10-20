// Migrated from LLM_Voice_Flow-master/llm/rknn-llm/examples/DeepSeek-R1-Distill-Qwen-1.5B_Demo/deploy/src/llm_demo.cpp
// Original: Rockchip Electronics Co., Ltd. (Apache License 2.0)
//
// Refactored: eliminated global variables, added graceful shutdown,
// removed debug code, fixed signal safety, replaced deprecated APIs.

#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include <locale>
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <csignal>
#include <stdexcept>

#include "rkllm.h"
#include "ZmqServer.h"
#include "ZmqClient.h"

static const char *MODULE = "llm_service";

// --- Signal-safe shutdown flag ---
static volatile std::sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int /*sig*/)
{
    g_shutdown_requested = 1;
}

// --- UTF-8 / wchar conversion (replaces deprecated wstring_convert) ---

static std::wstring utf8_to_wstring(const std::string &str)
{
    std::wstring result;
    result.reserve(str.size());
    size_t i = 0;
    while (i < str.size()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        wchar_t wc = 0;
        if (c < 0x80) {
            wc = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= str.size()) break;
            wc = ((c & 0x1F) << 6) | (static_cast<unsigned char>(str[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= str.size()) break;
            wc = ((c & 0x0F) << 12) |
                 ((static_cast<unsigned char>(str[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(str[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= str.size()) break;
            wc = ((c & 0x07) << 18) |
                 ((static_cast<unsigned char>(str[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(str[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(str[i + 3]) & 0x3F);
            i += 4;
        } else {
            i += 1;
            continue;
        }
        result += wc;
    }
    return result;
}

static std::string wstring_to_utf8(const std::wstring &str)
{
    std::string result;
    result.reserve(str.size() * 3);
    for (wchar_t wc : str) {
        if (wc < 0x80) {
            result += static_cast<char>(wc);
        } else if (wc < 0x800) {
            result += static_cast<char>(0xC0 | (wc >> 6));
            result += static_cast<char>(0x80 | (wc & 0x3F));
        } else if (wc < 0x10000) {
            result += static_cast<char>(0xE0 | (wc >> 12));
            result += static_cast<char>(0x80 | ((wc >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (wc & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (wc >> 18));
            result += static_cast<char>(0x80 | ((wc >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((wc >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (wc & 0x3F));
        }
    }
    return result;
}

// --- Text processing helpers ---

static std::wstring extract_after_think(const std::wstring &input)
{
    const std::wstring start_tag = L"<think>";
    const std::wstring end_tag = L"</think>";

    size_t start_pos = input.find(start_tag);
    size_t end_pos = input.find(end_tag);

    std::wstring result;

    if (start_pos != std::wstring::npos && end_pos != std::wstring::npos && end_pos > start_pos) {
        result = input.substr(start_pos + start_tag.length(), end_pos - start_pos - start_tag.length());
    } else if (end_pos != std::wstring::npos) {
        result = input.substr(end_pos + end_tag.length());
    } else {
        result = input;
    }

    static const std::wstring punct = L" \t\n\r*#@$%^&，。：、；！？【】（）""''";

    std::wstring filtered;
    for (wchar_t c : result) {
        if (punct.find(c) == std::wstring::npos) {
            filtered += c;
        }
    }

    return filtered;
}

// --- LLM Service class ---

class LlmService {
public:
    // Static instance pointer for callback access (API doesn't support userdata properly).
    // 用 std::atomic 保证主线程构造/析构与 rkllm callback 线程之间的安全访问。
    static std::atomic<LlmService *> s_instance;

    LlmService(const std::string &asr_address,
               const std::string &tts_address)
        : m_asr_server(asr_address)
        , m_tts_client(tts_address)
    {
        s_instance.store(this, std::memory_order_release);
    }

    ~LlmService()
    {
        shutdown();
        s_instance.store(nullptr, std::memory_order_release);
    }

    bool init(const std::string &model_path)
    {
        RKLLMParam param = rkllm_createDefaultParam();
        param.model_path = model_path.c_str();

        param.top_k = 1;
        param.top_p = 0.95;
        param.temperature = 0.8;
        param.repeat_penalty = 1.1;
        param.frequency_penalty = 0.0;
        param.presence_penalty = 0.0;

        param.max_new_tokens = 100;
        param.max_context_len = 256;
        param.skip_special_token = true;
        param.extend_param.base_domain_id = 0;
        param.extend_param.embed_flash = 1;
        param.extend_param.enabled_cpus_num = 2;
        param.extend_param.enabled_cpus_mask = CPU0 | CPU2;

        int ret = rkllm_init(&m_handle, &param, llm_callback);
        if (ret != 0) {
            fprintf(stderr, "[%s] ERROR: rkllm_init failed (ret=%d)\n", MODULE, ret);
            return false;
        }

        m_running = true;
        printf("[%s] rkllm init success\n", MODULE);
        return true;
    }

    void shutdown()
    {
        m_running = false;
        if (m_handle != nullptr) {
            printf("[%s] destroying LLM handle\n", MODULE);
            LLMHandle tmp = m_handle;
            m_handle = nullptr;
            rkllm_destroy(tmp);
        }
    }

    void run()
    {
        RKLLMInferParam infer_params;
        memset(&infer_params, 0, sizeof(RKLLMInferParam));
        infer_params.mode = RKLLM_INFER_GENERATE;
        infer_params.keep_history = 0;

        printf("[%s] entering main loop, waiting for ASR input...\n", MODULE);

        while (m_running && g_shutdown_requested == 0) {
            rkllm_set_chat_template(m_handle, "", "<｜User｜>", "<｜Assistant｜>");

            std::string input_str;
            try {
                input_str = m_asr_server.receive();
            } catch (const std::exception &e) {
                if (g_shutdown_requested) break;
                fprintf(stderr, "[%s] receive error: %s\n", MODULE, e.what());
                continue;
            }

            printf("[%s] received from ASR: %s\n", MODULE, input_str.c_str());

            // Acknowledge receipt to ASR client
            try {
                m_asr_server.send("ok");
            } catch (const std::exception &e) {
                fprintf(stderr, "[%s] send ack error: %s\n", MODULE, e.what());
            }

            // Set callback context for this inference
            m_buffer.clear();

            RKLLMInput rkllm_input;
            memset(&rkllm_input, 0, sizeof(RKLLMInput));
            rkllm_input.role = "user";
            rkllm_input.enable_thinking = false;
            rkllm_input.input_type = RKLLM_INPUT_PROMPT;
            rkllm_input.prompt_input = input_str.c_str();

            printf("[%s] rkllm_run start\n", MODULE);
            int ret = rkllm_run(m_handle, &rkllm_input, &infer_params, nullptr);
            if (ret != 0) {
                fprintf(stderr, "[%s] rkllm_run failed (ret=%d)\n", MODULE, ret);
                // rkllm_run 失败时 RKLLM_RUN_FINISH 回调可能不会触发,
                // 手动 flush 以确保已生成的部分内容发送给 TTS,避免丢字。
                // flush_buffer_to_tts 内部会清空 m_buffer 并发送 END 标记。
                flush_buffer_to_tts();
            }
            printf("[%s] rkllm_run returned: %d\n", MODULE, ret);
        }

        printf("[%s] main loop exited\n", MODULE);
    }

    // Callback invoked by rkllm (static, accesses instance via s_instance)
    static int llm_callback(RKLLMResult *result, void * /*userdata*/, LLMCallState state)
    {
        LlmService *self = s_instance.load(std::memory_order_acquire);
        if (!self || !result) return 0;

        if (state == RKLLM_RUN_FINISH) {
            printf("[%s] callback: RKLLM_RUN_FINISH\n", MODULE);
            self->flush_buffer_to_tts();
        } else if (state == RKLLM_RUN_ERROR) {
            fprintf(stderr, "[%s] callback: RKLLM_RUN_ERROR\n", MODULE);
        } else if (state == RKLLM_RUN_NORMAL) {
            if (!result->text) return 0;

            std::wstring wide_text = utf8_to_wstring(result->text);
            for (wchar_t c : wide_text) {
                self->m_buffer += c;
                // Split on Chinese punctuation for streaming TTS
                static const std::wstring split_chars = L"：，。\n；！？";
                if (split_chars.find(c) != std::wstring::npos) {
                    if (!self->m_buffer.empty()) {
                        self->send_chunk_to_tts(self->m_buffer);
                        self->m_buffer.clear();
                    }
                }
            }
        }
        return 0;
    }

private:
    void send_chunk_to_tts(const std::wstring &text)
    {
        std::string utf8_text = wstring_to_utf8(extract_after_think(text));
        if (utf8_text.empty()) return;

        try {
            std::string reply = m_tts_client.request(utf8_text);
            printf("[%s] TTS reply: %s\n", MODULE, reply.c_str());
        } catch (const std::exception &e) {
            fprintf(stderr, "[%s] TTS request failed: %s\n", MODULE, e.what());
        }
    }

    void flush_buffer_to_tts()
    {
        try {
            if (!m_buffer.empty()) {
                std::string utf8_text = wstring_to_utf8(extract_after_think(m_buffer));
                if (!utf8_text.empty()) {
                    std::string msg = utf8_text + " END";
                    printf("[%s] sending final chunk to TTS\n", MODULE);
                    std::string reply = m_tts_client.request(msg);
                    printf("[%s] TTS reply: %s\n", MODULE, reply.c_str());
                }
                m_buffer.clear();
            } else {
                printf("[%s] sending END to TTS\n", MODULE);
                std::string reply = m_tts_client.request("END");
                printf("[%s] TTS reply: %s\n", MODULE, reply.c_str());
            }
        } catch (const std::exception &e) {
            fprintf(stderr, "[%s] TTS request failed: %s\n", MODULE, e.what());
            m_buffer.clear();
        }
    }

    LLMHandle m_handle = nullptr;
    zmq_component::ZmqServer m_asr_server;       // receives from ASR (port 6666)
    zmq_component::ZmqClient m_tts_client;       // sends to TTS (port 7777)
    std::wstring m_buffer;
    bool m_running = false;
};

std::atomic<LlmService *> LlmService::s_instance{nullptr};

// --- Entry point ---

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "en_US.UTF-8");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model_path> [asr_address] [tts_address]\n", argv[0]);
        fprintf(stderr, "  asr_address: ZMQ address to receive ASR text (default: tcp://*:6666)\n");
        fprintf(stderr, "  tts_address: ZMQ address to send to TTS (default: tcp://localhost:7777)\n");
        return 1;
    }

    // Install signal handler for graceful shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const char *asr_addr = (argc > 2) ? argv[2] : "tcp://*:6666";
    const char *tts_addr = (argc > 3) ? argv[3] : "tcp://localhost:7777";

    printf("[%s] starting LLM service (model=%s, asr=%s, tts=%s)\n",
           MODULE, argv[1], asr_addr, tts_addr);

    LlmService service(asr_addr, tts_addr);

    if (!service.init(argv[1])) {
        fprintf(stderr, "[%s] FATAL: LLM init failed\n", MODULE);
        return 1;
    }

    service.run();

    // service destructor handles cleanup
    printf("[%s] LLM service stopped\n", MODULE);
    return 0;
}
