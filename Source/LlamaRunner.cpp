#include "LlamaRunner.h"
#include "llama.h"

#include <mutex>
#include <sstream>

struct LlamaRunner::Impl {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    int n_ctx = 2048;
    int seed = 12345;
    std::mutex mtx;
};
static bool ends_with_any(const std::string& s, const std::vector<std::string>& stops) {
    for (const auto& t : stops) {
        if (!t.empty() && s.size() >= t.size()) {
            if (memcmp(s.data() + (s.size() - t.size()), t.data(), t.size()) == 0)
                return true;
        }
    }
    return false;
}

// detokenize whole buffer to a std::string
static std::string detok_all(const llama_vocab* vocab,
    const std::vector<llama_token>& toks,
    bool remove_special)
{
    if (toks.empty()) return {};
    // first get required size
    int need = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
        nullptr, 0, remove_special, /*unparse_special*/false);
    if (need <= 0) return {};
    std::string out;
    out.resize((size_t)need);
    int got = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
        &out[0], (int32_t)out.size(), remove_special, false);
    if (got > 0) out.resize((size_t)got);
    return out;
}
LlamaRunner::LlamaRunner() : impl(new Impl) {}
LlamaRunner::~LlamaRunner() { unload(); }

bool LlamaRunner::isLoaded() const {
    return impl && impl->ctx != nullptr;
}

bool LlamaRunner::loadModel(const std::string& modelPath,
    const LlamaContextParams& p,
    std::string& err)
{
    unload();
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    // CPU-only defaults are fine (use_mmap/use_mlock/check_tensors already defaulted)

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)p.n_ctx;
    cparams.n_batch = (uint32_t)p.n_batch;
    cparams.n_threads = 0;          // 0 lets lib pick; set if you want
    cparams.n_threads_batch = 0;
    cparams.embeddings = false;

    impl->model = llama_model_load_from_file(modelPath.c_str(), mparams);
    if (!impl->model) { err = "llama_model_load_from_file failed"; unload(); return false; }

    impl->ctx = llama_init_from_model(impl->model, cparams);
    if (!impl->ctx) { err = "llama_init_from_model failed"; unload(); return false; }

    impl->vocab = llama_model_get_vocab(impl->model);
    impl->n_ctx = (int)cparams.n_ctx;
    impl->seed = (p.seed >= 0 ? p.seed : 12345);
    return true;
}

void LlamaRunner::unload() {
    if (!impl) return;
    if (impl->ctx) { llama_free(impl->ctx);   impl->ctx = nullptr; }
    if (impl->model) { llama_model_free(impl->model); impl->model = nullptr; }
    impl->vocab = nullptr;
    llama_backend_free();
}

std::string LlamaRunner::generate(const std::string& prompt,
    const LlamaInferParams& ip,
    double* outTokensPerSec,
    std::string* errorOut)
{
    if (!isLoaded()) {
        if (errorOut) *errorOut = "Model not loaded";
        return {};
    }
    std::lock_guard<std::mutex> lock(impl->mtx);

    const auto* vocab = impl->vocab;
    if (!vocab) {
        if (errorOut) *errorOut = "No vocab";
        return {};
    }

    // 1) Tokenize prompt with BOS/EOS as configured by model
    // two-pass: ask for size, then fill
    int32_t need = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
        nullptr, 0, /*add_special*/true, /*parse_special*/false);
    if (need == INT32_MIN) {
        if (errorOut) *errorOut = "tokenize overflow";
        return {};
    }
    if (need < 0) need = -need; // API returns negative "needed" when output buffer is NULL

    std::vector<llama_token> prompt_tokens((size_t)need);
    int32_t got = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
        prompt_tokens.data(), (int32_t)prompt_tokens.size(),
        /*add_special*/true, /*parse_special*/false);
    if (got <= 0) {
        if (errorOut) *errorOut = "tokenize failed";
        return {};
    }
    prompt_tokens.resize((size_t)got);

    if ((int)prompt_tokens.size() >= impl->n_ctx) {
        if (errorOut) *errorOut = "prompt too long for n_ctx";
        return {};
    }

    // 2) Feed prompt in one go
    // use helper to get a batch for a single sequence
    {
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), (int32_t)prompt_tokens.size());
        int32_t dec = llama_decode(impl->ctx, batch);
        if (dec < 0) { /* handle error */ }
    }
    
    // 3) Build sampler chain
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(sparams);

    // repetition penalty (use last_n ~64)
    if (ip.repeat_penalty != 1.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(
            /*penalty_last_n*/64,
            /*penalty_repeat*/ip.repeat_penalty,
            /*penalty_freq*/0.0f,
            /*penalty_present*/0.0f));
    }
    if (ip.top_k > 0) {
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(ip.top_k));
    }
    if (ip.top_p > 0.0f && ip.top_p < 1.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_top_p(ip.top_p, /*min_keep*/1));
    }
    if (ip.temperature > 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_temp(ip.temperature));
        // final selector: distribution with a seed
        const uint32_t seed = (ip.seed >= 0) ? (uint32_t)ip.seed : (uint32_t)impl->seed;
        llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    }
    else {
        // temp == 0 => greedy argmax
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    }

    // Grammar (optional)
    if (!ip.grammar.empty()) {
        // start symbol "root" must match your grammar
        llama_sampler* g = llama_sampler_init_grammar(vocab, ip.grammar.c_str(), "root");
        if (g) llama_sampler_chain_add(chain, g);
    }
    for (auto t : prompt_tokens)
        llama_sampler_accept(chain, t);
    // 4) Sampling loop
    const llama_token eos = llama_vocab_eos(vocab);
    std::vector<llama_token> out_tokens;
    out_tokens.reserve((size_t)ip.max_tokens);

    int32_t generated = 0;
    int64_t t_start_us = llama_time_us();
    bool eos_at_first_token = false;
    for (; generated < ip.max_tokens; ++generated) {
        const llama_token id = llama_sampler_sample(chain, impl->ctx, -1);
        if (id == LLAMA_TOKEN_NULL) {
            if (errorOut) *errorOut = "sampler returned null token";
            break;
        }
        if (id == eos) {
            if (generated == 0 && errorOut) errorOut = errorOut, * errorOut = "sampled EOS at first token";
            break;
        }

        // accept to update internal sampler state (grammar, repeat, etc.)
        llama_sampler_accept(chain, id);

        out_tokens.push_back(id);

        // Detokenize current output to check optional stop strings
        if (!ip.stop.empty()) {
            std::string cur = detok_all(vocab, out_tokens, /*remove_special*/true);
            if (ends_with_any(cur, ip.stop)) {
                break;
            }
        }

        // Decode the just-sampled token to advance logits
        llama_batch batch = llama_batch_get_one(&out_tokens.back(), 1);
        int32_t dec = llama_decode(impl->ctx, batch);
        if (dec < 0) {
            if (errorOut) *errorOut = "llama_decode(gen) failed";
            break;
        }
    }

    // 5) Cleanup samplers
    llama_sampler_free(chain);

    // 6) Final detokenize
    std::string out = detok_all(vocab, out_tokens, /*remove_special*/true);

    // 7) Perf
    if (outTokensPerSec) {
        const auto perf = llama_perf_context(impl->ctx);
        // tokens per second based on generation stats
        double sec = perf.t_eval_ms / 1000.0;
        int n = perf.n_eval;
        *outTokensPerSec = (sec > 0.0 ? (double)n / sec : 0.0);
    }

    return out;
}

