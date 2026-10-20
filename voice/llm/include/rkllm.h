#ifndef RKLLM_H
#define RKLLM_H
#include <cstdint>
#ifdef __cplusplus
extern "C" {
#endif

#define CPU0 (1 << 0)  // 0x01
#define CPU1 (1 << 1)  // 0x02
#define CPU2 (1 << 2)  // 0x04
#define CPU3 (1 << 3)  // 0x08
#define CPU4 (1 << 4)  // 0x10
#define CPU5 (1 << 5)  // 0x20
#define CPU6 (1 << 6)  // 0x40
#define CPU7 (1 << 7)  // 0x80

/**
 * @typedef LLMHandle
 * @brief A handle used to manage and interact with the large language model.
 */
typedef void* LLMHandle;

/**
 * @enum LLMCallState
 * @brief Describes the possible states of an LLM call.
 */
typedef enum {
    RKLLM_RUN_NORMAL  = 0, /**< The LLM call is in a normal running state. */
    RKLLM_RUN_WAITING = 1, /**< The LLM call is waiting for complete UTF-8 encoded character. */
    RKLLM_RUN_FINISH  = 2, /**< The LLM call has finished execution. */
    RKLLM_RUN_ERROR   = 3, /**< An error occurred during the LLM call. */
} LLMCallState;

/**
 * @enum RKLLMInputType
 * @brief Defines the types of inputs that can be fed into the LLM.
 */
typedef enum {
    RKLLM_INPUT_PROMPT      = 0, /**< Input is a text prompt. */
    RKLLM_INPUT_TOKEN       = 1, /**< Input is a sequence of tokens. */
    RKLLM_INPUT_EMBED       = 2, /**< Input is an embedding vector. */
    RKLLM_INPUT_MULTIMODAL  = 3, /**< Input is multimodal (e.g., text and image). */
} RKLLMInputType;

/**
 * @enum RKLLMInferMode
 * @brief Specifies the inference modes of the LLM.
 */
typedef enum {
    RKLLM_INFER_GENERATE                    = 0, /**< The LLM generates text based on input. */
    RKLLM_INFER_GET_LAST_HIDDEN_LAYER       = 1, /**< The LLM retrieves the last hidden layer for further processing. */
    RKLLM_INFER_GET_LOGITS                  = 2, /**< The LLM retrieves logits for further processing. */
} RKLLMInferMode;

/**
 * @struct RKLLMExtendParam
 * @brief The extend parameters for configuring an LLM instance.
 */
typedef struct {
    int32_t      base_domain_id;        /**< base_domain_id */
    int8_t       embed_flash;           /**< Indicates whether to query word embedding vectors from flash memory (1) or not (0). */
    int8_t       enabled_cpus_num;      /**< Number of CPUs enabled for inference. */
    uint32_t     enabled_cpus_mask;     /**< Bitmask indicating which CPUs to enable for inference. */
    uint8_t      n_batch;               /**< Number of input samples processed concurrently in one forward pass. */
    int8_t       use_cross_attn;        /**< Whether to enable cross attention (non-zero to enable, 0 to disable). */
    uint8_t      reserved[104];         /**< reserved */
} RKLLMExtendParam;

/**
 * @struct RKLLMParam
 * @brief Defines the parameters for configuring an LLM instance.
 */
typedef struct {
    const char* model_path;         /**< Path to the model file. */
    int32_t max_context_len;        /**< Maximum number of tokens in the context window. */
    int32_t max_new_tokens;         /**< Maximum number of new tokens to generate. */
    int32_t top_k;                  /**< Top-K sampling parameter for token generation. */
    int32_t n_keep;                 /** number of kv cache to keep at the beginning when shifting context window */
    float top_p;                    /**< Top-P (nucleus) sampling parameter. */
    float temperature;              /**< Sampling temperature, affecting the randomness of token selection. */
    float repeat_penalty;           /**< Penalty for repeating tokens in generation. */
    float frequency_penalty;        /**< Penalizes frequent tokens during generation. */
    float presence_penalty;         /**< Penalizes frequent tokens based on their presence in the input. */
    int32_t mirostat;               /**< Mirostat sampling strategy flag (0 to disable). */
    float mirostat_tau;             /**< Tau parameter for Mirostat sampling. */
    float mirostat_eta;             /**< Eta parameter for Mirostat sampling. */
    bool skip_special_token;        /**< Whether to skip special tokens during generation. */
    bool is_async;                  /**< Whether to run inference asynchronously. */
    const char* img_start;          /**< Starting position of an image in multimodal input. */
    const char* img_end;            /**< Ending position of an image in multimodal input. */
    const char* img_content;        /**< Pointer to the image content. */
    RKLLMExtendParam extend_param; /**< Extend parameters. */
} RKLLMParam;

/**
 * @struct RKLLMLoraAdapter
 * @brief Defines parameters for a Lora adapter used in model fine-tuning.
 */
typedef struct {
    const char* lora_adapter_path; /**< Path to the Lora adapter file. */
    const char* lora_adapter_name; /**< Name of the Lora adapter. */
    float scale;                   /**< Scaling factor for applying the Lora adapter. */
} RKLLMLoraAdapter;

/**
 * @struct RKLLMEmbedInput
 * @brief Represents an embedding input to the LLM.
 */
typedef struct {
    float* embed;      /**< Pointer to the embedding vector. */
    size_t n_tokens;   /**< Number of tokens represented in the embedding. */
} RKLLMEmbedInput;

/**
 * @struct RKLLMTokenInput
 * @brief Represents token input to the LLM.
 */
typedef struct {
    int32_t* input_ids; /**< Array of token IDs. */
    size_t n_tokens;    /**< Number of tokens in the input. */
} RKLLMTokenInput;

/**
 * @struct RKLLMMultiModalInput
 * @brief Represents multimodal input (e.g., text and image).
 */
typedef struct {
    char* prompt;           /**< Text prompt input. */
    float* image_embed;     /**< Embedding of the images. */
    size_t n_image_tokens;  /**< Number of image_token. */
    size_t n_image;         /**< Number of image. */
    size_t image_width;     /**< Width of image. */
    size_t image_height;    /**< Height of image. */
} RKLLMMultiModalInput;

/**
 * @struct RKLLMInput
 * @brief Represents different types of input to the LLM via a union.
 */
typedef struct {
    const char* role;          /**< Message role: "user" (user input), "tool" (function result) */
    bool enable_thinking;      /**< Controls whether "thinking mode" is enabled for the Qwen3 model. */
    RKLLMInputType input_type; /**< Specifies the type of input provided. */
    union {
        const char* prompt_input;               /**< Text prompt input if input_type is RKLLM_INPUT_PROMPT. */
        RKLLMEmbedInput embed_input;            /**< Embedding input if input_type is RKLLM_INPUT_EMBED. */
        RKLLMTokenInput token_input;            /**< Token input if input_type is RKLLM_INPUT_TOKEN. */
        RKLLMMultiModalInput multimodal_input;  /**< Multimodal input if input_type is RKLLM_INPUT_MULTIMODAL. */
    };
} RKLLMInput;

/**
 * @struct RKLLMLoraParam
 * @brief Structure defining parameters for Lora adapters.
 */
typedef struct {
    const char* lora_adapter_name; /**< Name of the Lora adapter. */
} RKLLMLoraParam;

/**
 * @struct RKLLMPromptCacheParam
 * @brief Structure to define parameters for caching prompts.
 */
typedef struct {
    int save_prompt_cache;          /**< Flag to indicate whether to save the prompt cache. */
    const char* prompt_cache_path;  /**< Path to the prompt cache file. */
} RKLLMPromptCacheParam;

/**
 * @struct RKLLMCrossAttnParam
 * @brief Structure holding parameters for cross-attention inference.
 */
typedef struct {
    float* encoder_k_cache;   /**< Pointer to encoder key cache. */
    float* encoder_v_cache;   /**< Pointer to encoder value cache. */
    float* encoder_mask;      /**< Pointer to encoder attention mask. */
    int32_t* encoder_pos;     /**< Pointer to encoder token positions. */
    int num_tokens;           /**< Number of tokens in the encoder sequence. */
} RKLLMCrossAttnParam;

/**
 * @struct RKLLMInferParam
 * @brief Structure for defining parameters during inference.
 */
typedef struct {
    RKLLMInferMode mode;                        /**< Inference mode. */
    RKLLMLoraParam* lora_params;                /**< Pointer to Lora adapter parameters. */
    RKLLMPromptCacheParam* prompt_cache_params; /**< Pointer to prompt cache parameters. */
    int keep_history;                           /**< Flag to determine history retention. */
} RKLLMInferParam;

/**
 * @struct RKLLMResultLastHiddenLayer
 * @brief Structure to hold the hidden states from the last layer.
 */
typedef struct {
    const float* hidden_states; /**< Pointer to the hidden states. */
    int embd_size;              /**< Size of the embedding vector. */
    int num_tokens;             /**< Number of tokens for which hidden states are stored. */
} RKLLMResultLastHiddenLayer;

/**
 * @struct RKLLMResultLogits
 * @brief Structure to hold the logits.
 */
typedef struct {
    const float* logits;        /**< Pointer to the logits. */
    int vocab_size;             /**< Size of the vocab. */
    int num_tokens;             /**< Number of tokens for which logits are stored. */
} RKLLMResultLogits;

/**
 * @struct RKLLMPerfStat
 * @brief Structure to hold performance statistics.
 */
typedef struct {
    float prefill_time_ms;      /**< Total time taken for the prefill stage in milliseconds. */
    int prefill_tokens;         /**< Number of tokens processed during the prefill stage. */
    float generate_time_ms;     /**< Total time taken for the generate stage in milliseconds. */
    int generate_tokens;        /**< Number of tokens processed during the generate stage. */
    float memory_usage_mb;      /**< VmHWM resident memory usage during inference, in megabytes. */
} RKLLMPerfStat;

/**
 * @struct RKLLMResult
 * @brief Structure to represent the result of LLM inference.
 */
typedef struct {
    const char* text;                             /**< Generated text result. */
    int32_t token_id;                             /**< ID of the generated token. */
    RKLLMResultLastHiddenLayer last_hidden_layer; /**< Hidden states of the last layer. */
    RKLLMResultLogits logits;                     /**< Model output logits. */
    RKLLMPerfStat perf;                           /**< Performance statistics. */
} RKLLMResult;

/**
 * @typedef LLMResultCallback
 * @brief Callback function to handle LLM results.
 * @return 0: Continue inference normally. 1: Pause inference.
 */
typedef int(*LLMResultCallback)(RKLLMResult* result, void* userdata, LLMCallState state);

RKLLMParam rkllm_createDefaultParam();
int rkllm_init(LLMHandle* handle, RKLLMParam* param, LLMResultCallback callback);
int rkllm_load_lora(LLMHandle handle, RKLLMLoraAdapter* lora_adapter);
int rkllm_load_prompt_cache(LLMHandle handle, const char* prompt_cache_path);
int rkllm_release_prompt_cache(LLMHandle handle);
int rkllm_destroy(LLMHandle handle);
int rkllm_run(LLMHandle handle, RKLLMInput* rkllm_input, RKLLMInferParam* rkllm_infer_params, void* userdata);
int rkllm_run_async(LLMHandle handle, RKLLMInput* rkllm_input, RKLLMInferParam* rkllm_infer_params, void* userdata);
int rkllm_abort(LLMHandle handle);
int rkllm_is_running(LLMHandle handle);
int rkllm_clear_kv_cache(LLMHandle handle, int keep_system_prompt, int* start_pos, int* end_pos);
int rkllm_get_kv_cache_size(LLMHandle handle, int* cache_sizes);
int rkllm_set_chat_template(LLMHandle handle, const char* system_prompt, const char* prompt_prefix, const char* prompt_postfix);

#ifdef __cplusplus
}
#endif

#endif
