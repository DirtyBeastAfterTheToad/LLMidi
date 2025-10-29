#pragma once
#include <string>
#include <vector>
#include <memory>

struct LlamaContextParams {
    int   n_ctx = 2048;
    int   n_batch = 512;
    int   n_gpu_layers = 0;     // CPU-only: keep 0
    int   seed = 12345;
    bool  low_vram = false; // reserved for GPU builds
};

struct LlamaInferParams {
    float temperature = 0.3f;
    float top_p = 0.9f;
    int   top_k = 40;
    float repeat_penalty = 1.1f;
    int   max_tokens = 512;
    int   seed = -1;   // -1 = use context seed
    std::vector<std::string> stop;   // optional stop strings
    std::string grammar;            // optional grammar text (empty = off)
};

class LlamaRunner {
public:
    LlamaRunner();
    ~LlamaRunner();

    // Non-copyable
    LlamaRunner(const LlamaRunner&) = delete;
    LlamaRunner& operator=(const LlamaRunner&) = delete;

    bool loadModel(const std::string& modelPath,
        const LlamaContextParams& p,
        std::string& errorOut);

    void unload();

    bool isLoaded() const;

    // Synchronous. Call from your background worker only.
    // Returns generated text. On failure, returns empty string and sets errorOut (if provided).
    std::string generate(const std::string& prompt,
        const LlamaInferParams& ip,
        double* outTokensPerSec = nullptr,
        std::string* errorOut = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
