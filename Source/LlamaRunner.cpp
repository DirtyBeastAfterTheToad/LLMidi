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
    cparams.n_threads = std::max(1u, std::thread::hardware_concurrency());
    cparams.n_threads_batch = cparams.n_threads;
    cparams.embeddings = false;

    impl->model = llama_model_load_from_file(modelPath.c_str(), mparams);
    if (!impl->model) { err = "llama_model_load_from_file failed"; unload(); return false; }

    impl->ctx = llama_init_from_model(impl->model, cparams);
    if (!impl->ctx) { err = "llama_init_from_model failed"; unload(); return false; }

    impl->vocab = llama_model_get_vocab(impl->model);
    impl->n_ctx = (int)cparams.n_ctx;
    impl->seed = (p.seed >= 0 ? p.seed : 12345);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        loadedModelPath_.clear();
        loadedModelPath_ = modelPath;
        modelLoaded_ = true;
    }

    return true;
}

void LlamaRunner::unload() {
    if (!impl) return;
    if (impl->ctx) { llama_free(impl->ctx);   impl->ctx = nullptr; }
    if (impl->model) { llama_model_free(impl->model); impl->model = nullptr; }
    impl->vocab = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        loadedModelPath_.clear();
        modelLoaded_ = false;
    }
    llama_backend_free();
}
std::string LlamaRunner::getLoadedModelPath() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return loadedModelPath_;
}
std::string LlamaRunner::generate(const std::string& prompt,
    const LlamaInferParams& ip,
    double* outTokensPerSec,
    std::string* errorOut)
{
    auto log = [&](const std::string& line) {
        if (!errorOut) return;
        if (errorOut->empty()) *errorOut += "";
        *errorOut += (errorOut->empty() ? "" : "\n");
        *errorOut += line;
        };

    if (!isLoaded()) { if (errorOut) *errorOut = "Model not loaded"; return {}; }

    std::lock_guard<std::mutex> lock(impl->mtx);

    const auto* vocab = impl->vocab;
    if (!vocab) { if (errorOut) *errorOut = "No vocab"; return {}; }

    using clock = std::chrono::steady_clock;
    const auto tAllStart = clock::now();

    // ---- 1) Tokenize prompt ----
    const bool add_special = true;
    const bool parse_special = false;

    int32_t need = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
        nullptr, 0, add_special, parse_special);
    if (need == INT32_MIN) { if (errorOut) *errorOut = "tokenize overflow"; return {}; }
    if (need < 0) need = -need;

    std::vector<llama_token> prompt_tokens((size_t)need);
    int32_t got = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
        prompt_tokens.data(), (int32_t)prompt_tokens.size(),
        add_special, parse_special);
    if (got <= 0) { if (errorOut) *errorOut = "tokenize failed"; return {}; }
    prompt_tokens.resize((size_t)got);

    if (prompt_tokens.empty()) { if (errorOut) *errorOut = "tokenize() returned 0 tokens"; return {}; }
    if ((int)prompt_tokens.size() >= impl->n_ctx) { if (errorOut) *errorOut = "prompt too long for n_ctx"; return {}; }

    {
        std::string dbg = "Prompt tokens: " + std::to_string(prompt_tokens.size());
        log(dbg);
        dbg = "First token IDs (up to 12): ";
        for (size_t i = 0; i < prompt_tokens.size() && i < 12; ++i) dbg += std::to_string(prompt_tokens[i]) + (i + 1 < 12 ? " " : "");
        log(dbg);
    }

    // ---- 2) Feed prompt ----
    int32_t posAbs = 0;
    for (size_t i = 0; i < prompt_tokens.size(); ++i) {
        llama_token tok = prompt_tokens[i];

        llama_token  tok_arr[1] = { tok };
        llama_pos    pos_arr[1] = { (llama_pos)posAbs };
        int32_t      nseq_arr[1] = { 1 };
        llama_seq_id sid_val[1] = { 0 };
        llama_seq_id* sid_ptrs[1] = { &sid_val[0] };
        int8_t       logits_arr[1] = { (int8_t)((i == prompt_tokens.size() - 1) ? 1 : 0) };

        llama_batch batchPrompt;
        batchPrompt.n_tokens = 1;
        batchPrompt.token = tok_arr;
        batchPrompt.embd = nullptr;
        batchPrompt.pos = pos_arr;
        batchPrompt.n_seq_id = nseq_arr;
        batchPrompt.seq_id = sid_ptrs;
        batchPrompt.logits = logits_arr;

        if (llama_decode(impl->ctx, batchPrompt) < 0) {
            log("llama_decode(prompt) failed at pos " + std::to_string(i));
            if (errorOut && errorOut->empty()) *errorOut = "llama_decode(prompt) failed";
            return {};
        }
        posAbs += 1;
    }

    const auto tAfterPrompt = clock::now();
    log("Prompt ingested OK. Starting sampling...");

    // ---- 3) Sampler chain ----
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(sparams);

    if (ip.repeat_penalty != 1.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(64, ip.repeat_penalty, 0.0f, 0.0f));
    }
    if (ip.top_k > 0) llama_sampler_chain_add(chain, llama_sampler_init_top_k(ip.top_k));
    if (ip.top_p > 0.0f && ip.top_p < 1.0f) llama_sampler_chain_add(chain, llama_sampler_init_top_p(ip.top_p, 1));

    if (ip.temperature > 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_temp(ip.temperature));
        const uint32_t seedUse = (ip.seed >= 0) ? (uint32_t)ip.seed : (uint32_t)impl->seed;
        llama_sampler_chain_add(chain, llama_sampler_init_dist(seedUse));
    }
    else {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    }

    if (!ip.grammar.empty()) {
        if (auto* g = llama_sampler_init_grammar(vocab, ip.grammar.c_str(), "root"))
            llama_sampler_chain_add(chain, g);
    }

    // Block EOS for the first token only
    const llama_token eos_tok = llama_vocab_eos(vocab);
    llama_logit_bias lb{ eos_tok, -10.0f };
    llama_sampler* eosBlocker = llama_sampler_init_logit_bias(llama_vocab_n_tokens(vocab), 1, &lb);
    llama_sampler_chain_add(chain, eosBlocker);
    const int eosBlockerIndex = llama_sampler_chain_n(chain) - 1;

    // seed penalties with prompt
    for (auto t : prompt_tokens) llama_sampler_accept(chain, t);

    // ---- 4) Generation loop ----
    std::vector<llama_token> out_tokens;
    out_tokens.reserve((size_t)ip.max_tokens);

    std::string stopReason = "max_tokens";
    bool firstLogged = false;

    const auto tGenStart = clock::now();

    for (int32_t generated = 0; generated < ip.max_tokens; ++generated) {
        llama_token next_id = llama_sampler_sample(chain, impl->ctx, -1);
        if (next_id == LLAMA_TOKEN_NULL) { stopReason = "sample_null"; break; }

        if (!firstLogged) {
            log("First token id: " + std::to_string(next_id) + " (EOS=" + std::to_string(eos_tok) + ")");
            // remove eos blocker after first step
            llama_sampler* removed = llama_sampler_chain_remove(chain, eosBlockerIndex);
            if (removed) llama_sampler_free(removed);
            firstLogged = true;
        }

        llama_sampler_accept(chain, next_id);
        out_tokens.push_back(next_id);

        if (next_id == eos_tok) { stopReason = "eos"; break; }

        if (!ip.stop.empty()) {
            // detok full so far (with specials unparsed) and check trailing stop strings
            const bool remove_special = false, unparse_special = true;
            int need3 = llama_detokenize(vocab, out_tokens.data(), (int32_t)out_tokens.size(),
                nullptr, 0, remove_special, unparse_special);
            if (need3 < 0) need3 = -need3;
            std::string tmp; tmp.resize((size_t)need3);
            int got3 = llama_detokenize(vocab, out_tokens.data(), (int32_t)out_tokens.size(),
                &tmp[0], (int32_t)tmp.size(), remove_special, unparse_special);
            if (got3 > 0) tmp.resize((size_t)got3);

            if (ends_with_any(tmp, ip.stop)) { stopReason = "stop_string"; break; }
        }

        // advance kv cache
        {
            llama_token  tok_arr[1] = { next_id };
            llama_pos    pos_arr[1] = { (llama_pos)posAbs };
            int32_t      nseq_arr[1] = { 1 };
            llama_seq_id sid_val[1] = { 0 };
            llama_seq_id* sid_ptrs[1] = { &sid_val[0] };
            int8_t       logits_arr[1] = { 1 };

            llama_batch genBatch;
            genBatch.n_tokens = 1;
            genBatch.token = tok_arr;
            genBatch.embd = nullptr;
            genBatch.pos = pos_arr;
            genBatch.n_seq_id = nseq_arr;
            genBatch.seq_id = sid_ptrs;
            genBatch.logits = logits_arr;

            if (llama_decode(impl->ctx, genBatch) < 0) { stopReason = "decode_fail"; break; }
        }
        posAbs += 1;
    }

    const auto tGenEnd = clock::now();

    // ---- 5) Cleanup ----
    llama_sampler_free(chain);

    // ---- 6) Detokenize final (raw, keep specials unparsed) ----
    std::string finalText;
    if (!out_tokens.empty()) {
        const bool remove_special = false, unparse_special = true;
        int needF = llama_detokenize(vocab, out_tokens.data(), (int32_t)out_tokens.size(),
            nullptr, 0, remove_special, unparse_special);
        if (needF < 0) needF = -needF;
        finalText.resize((size_t)needF);
        int gotF = llama_detokenize(vocab, out_tokens.data(), (int32_t)out_tokens.size(),
            &finalText[0], (int32_t)finalText.size(), remove_special, unparse_special);
        if (gotF > 0) finalText.resize((size_t)gotF);
    }

    // ---- 7) Timing + logs ----
    const auto tAllEnd = clock::now();
    const double msPrompt = std::chrono::duration<double, std::milli>(tAfterPrompt - tAllStart).count();
    const double msGen = std::chrono::duration<double, std::milli>(tGenEnd - tGenStart).count();
    const double msTotal = std::chrono::duration<double, std::milli>(tAllEnd - tAllStart).count();

    const int genTokens = (int)out_tokens.size();
    const double tokPerSec = (msGen > 0.0) ? (genTokens * 1000.0 / msGen) : 0.0;

    log(std::string("Stop reason: ") + stopReason);
    log("Final tokens: " + std::to_string(genTokens));

    // sanitize preview (single line, max 300 chars)
    std::string preview = finalText;
    for (char& c : preview) if (c == '\n' || c == '\r') c = ' ';
    if (preview.size() > 500) preview.resize(500), preview += "...";
    log("Final (preview): \"" + preview + "\"");

    log("Time: total=" + std::to_string(msTotal / 1000.0) + "s, prompt=" + std::to_string(msPrompt / 1000.0) +
        "s, gen=" + std::to_string(msGen / 1000.0) + "s");
    log("Throughput: " + std::to_string(tokPerSec) + " tok/s");

    if (outTokensPerSec) *outTokensPerSec = tokPerSec;
    return finalText;
}

