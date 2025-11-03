#include "LlamaRunner.h"
#include "llama.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>

struct LlamaRunner::Impl {
	llama_model* model = nullptr;
	llama_context* ctx = nullptr;
	const llama_vocab* vocab = nullptr;

	llama_context_params ctxParams{};
	int n_ctx = 2048;
	int seed = 12345;

	std::mutex mtx;
};

namespace {

	inline void appendLine(std::string* sink, const std::string& line) {
		if (!sink) return;
		if (sink->empty()) *sink += "";
		*sink += (sink->empty() ? "" : "\n");
		*sink += line;
	}

	inline void emitLog(const std::string& s, std::string* errorOut, LlamaRunner::LogFn onLog) {
		appendLine(errorOut, s);
		if (onLog) onLog(s);
	}

	inline void emitProgress(const std::string& s, LlamaRunner::LogFn onLog) {
		if (onLog) onLog(std::string("[[PROGRESS]] ") + s);
	}

	inline std::string make_progress_bar(int current, int total, int width = 28) {
		if (total <= 0) total = 1;
		double ratio = std::min(1.0, std::max(0.0, (double)current / (double)total));
		int filled = (int)std::round(ratio * width);
		std::string bar = "[";
		bar.append(filled, '#');
		bar.append(std::max(0, width - filled), '.');
		bar += "]";
		int pct = (int)std::round(ratio * 100.0);
		bar += " " + std::to_string(current) + "/" + std::to_string(total) + " (" + std::to_string(pct) + "%)";
		return bar;
	}

	inline bool ends_with_any(const std::string& s, const std::vector<std::string>& stops) {
		for (const auto& t : stops) {
			if (!t.empty() && s.size() >= t.size()) {
				if (std::memcmp(s.data() + (s.size() - t.size()), t.data(), t.size()) == 0)
					return true;
			}
		}
		return false;
	}

	inline bool detokenize_raw(const llama_vocab* vocab,
		const std::vector<llama_token>& toks,
		bool remove_special,
		bool unparse_special,
		std::string& out)
	{
		out.clear();
		if (toks.empty()) return true;
		int need = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
			nullptr, 0, remove_special, unparse_special);
		if (need < 0) need = -need;
		if (need <= 0) return false;
		out.resize((size_t)need);
		int got = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
			&out[0], (int32_t)out.size(), remove_special, unparse_special);
		if (got > 0) out.resize((size_t)got);
		return true;
	}

	inline bool tokenize_prompt(const llama_vocab* vocab,
		const std::string& prompt,
		std::vector<llama_token>& out,
		std::string* errorOut)
	{
		const bool add_special = true;
		const bool parse_special = false;

		int32_t need = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
			nullptr, 0, add_special, parse_special);
		if (need == INT32_MIN) { if (errorOut) *errorOut = "tokenize overflow"; return false; }
		if (need < 0) need = -need;

		out.resize((size_t)need);
		int32_t got = llama_tokenize(vocab, prompt.c_str(), (int32_t)prompt.size(),
			out.data(), (int32_t)out.size(), add_special, parse_special);
		if (got <= 0) { if (errorOut) *errorOut = "tokenize failed"; return false; }
		out.resize((size_t)got);

		if (out.empty()) { if (errorOut) *errorOut = "tokenize() returned 0 tokens"; return false; }
		return true;
	}

	inline bool feed_prompt(llama_context* ctx,
		const std::vector<llama_token>& toks,
		std::string* errorOut,
		LlamaRunner::LogFn onLog)
	{
		const int total = (int)toks.size();
		const int every = std::max(1, total / 20);

		// initial bar at 0
		emitProgress("Prompt " + make_progress_bar(0, total), onLog);

		int32_t posAbs = 0;
		for (int i = 0; i < total; ++i) {
			llama_token  tok_arr[1] = { toks[(size_t)i] };
			llama_pos    pos_arr[1] = { (llama_pos)posAbs };
			int32_t      nseq_arr[1] = { 1 };
			llama_seq_id sid_val[1] = { 0 };
			llama_seq_id* sid_ptrs[1] = { &sid_val[0] };
			int8_t       logits_arr[1] = { (int8_t)((i == total - 1) ? 1 : 0) };

			llama_batch b{};
			b.n_tokens = 1;
			b.token = tok_arr;
			b.embd = nullptr;
			b.pos = pos_arr;
			b.n_seq_id = nseq_arr;
			b.seq_id = sid_ptrs;
			b.logits = logits_arr;

			if (llama_decode(ctx, b) < 0) {
				emitLog("llama_decode(prompt) failed at pos " + std::to_string(i), errorOut, onLog);
				if (errorOut && errorOut->empty()) *errorOut = "llama_decode(prompt) failed";
				return false;
			}
			posAbs += 1;

			if ((i % every == 0) || (i + 1 == total)) {
				emitProgress("Prompt " + make_progress_bar(i + 1, total), onLog);
			}
		}
		return true;
	}

	inline llama_sampler* build_sampler_chain(const LlamaInferParams& ip,
		const llama_vocab* vocab,
		uint32_t seedForDist)
	{
		auto sparams = llama_sampler_chain_default_params();
		llama_sampler* chain = llama_sampler_chain_init(sparams);

		if (ip.repeat_penalty != 1.0f)
			llama_sampler_chain_add(chain, llama_sampler_init_penalties(64, ip.repeat_penalty, 0.0f, 0.0f));
		if (ip.top_k > 0)
			llama_sampler_chain_add(chain, llama_sampler_init_top_k(ip.top_k));
		if (ip.top_p > 0.0f && ip.top_p < 1.0f)
			llama_sampler_chain_add(chain, llama_sampler_init_top_p(ip.top_p, 1));

		if (ip.temperature > 0.0f) {
			llama_sampler_chain_add(chain, llama_sampler_init_temp(ip.temperature));
			llama_sampler_chain_add(chain, llama_sampler_init_dist(seedForDist));
		}
		else {
			llama_sampler_chain_add(chain, llama_sampler_init_greedy());
		}

		if (!ip.grammar.empty()) {
			if (auto* g = llama_sampler_init_grammar(vocab, ip.grammar.c_str(), "root"))
				llama_sampler_chain_add(chain, g);
		}

		return chain;
	}

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

	llama_context_params cparams = llama_context_default_params();
	cparams.n_ctx = (uint32_t)p.n_ctx;
	cparams.n_batch = (uint32_t)p.n_batch;
	cparams.n_threads = std::max(1u, std::thread::hardware_concurrency());
	cparams.n_threads_batch = cparams.n_threads;
	cparams.embeddings = false;

	impl->ctxParams = cparams;
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
	if (impl->ctx) { llama_free(impl->ctx);         impl->ctx = nullptr; }
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
	std::string* errorOut,
	LogFn onLog)
{
	if (!isLoaded()) { if (errorOut) *errorOut = "Model not loaded"; return {}; }

	std::lock_guard<std::mutex> guard(impl->mtx);

	// fresh context for each call
	if (impl->ctx) { llama_free(impl->ctx); impl->ctx = nullptr; }
	impl->ctx = llama_init_from_model(impl->model, impl->ctxParams);
	if (!impl->ctx) { if (errorOut) *errorOut = "llama_init_from_model (reinit) failed"; return {}; }
	impl->vocab = llama_model_get_vocab(impl->model);

	const auto* vocab = impl->vocab;
	if (!vocab) { if (errorOut) *errorOut = "No vocab"; return {}; }

	using clock = std::chrono::steady_clock;
	const auto tAllStart = clock::now();

	// 1) Tokenize prompt
	std::vector<llama_token> prompt_tokens;
	if (!tokenize_prompt(vocab, prompt, prompt_tokens, errorOut)) return {};
	if ((int)prompt_tokens.size() >= impl->n_ctx) { if (errorOut) *errorOut = "prompt too long for n_ctx"; return {}; }

	emitLog("Prompt tokens: " + std::to_string(prompt_tokens.size()), errorOut, onLog);

	// 2) Feed prompt
	if (!feed_prompt(impl->ctx, prompt_tokens, errorOut, onLog)) return {};
	const auto tAfterPrompt = clock::now();
	emitLog("Prompt ingested OK. Starting sampling...", errorOut, onLog);

	// 3) Sampler chain
	const uint32_t seedUse = (ip.seed >= 0) ? (uint32_t)ip.seed : (uint32_t)impl->seed;
	llama_sampler* chain = build_sampler_chain(ip, vocab, seedUse);

	// Block EOS for first token
	const llama_token eos_tok = llama_vocab_eos(vocab);
	llama_logit_bias lb{ eos_tok, -10.0f };
	llama_sampler* eosBlocker = llama_sampler_init_logit_bias(llama_vocab_n_tokens(vocab), 1, &lb);
	llama_sampler_chain_add(chain, eosBlocker);
	const int eosBlockerIndex = llama_sampler_chain_n(chain) - 1;

	// seed penalties with prompt
	for (auto t : prompt_tokens) llama_sampler_accept(chain, t);

	// 4) Generation 
	std::vector<llama_token> out_tokens;
	out_tokens.reserve((size_t)ip.max_tokens);

	std::string stopReason = "max_tokens";
	bool firstLogged = false;
	int32_t posAbs = (int32_t)prompt_tokens.size();

	const auto tGenStart = clock::now();

	const int budget = std::max(1, ip.max_tokens);
	const int barEvery = std::max(1, budget / 20);

	for (int32_t generated = 0; generated < ip.max_tokens; ++generated) {
		llama_token next_id = llama_sampler_sample(chain, impl->ctx, -1);
		if (next_id == LLAMA_TOKEN_NULL) { stopReason = "decode_fail"; break; }

		if (!firstLogged) {
			emitLog("First token id: " + std::to_string(next_id) + " (EOS=" + std::to_string(eos_tok) + ")", errorOut, onLog);
			if (llama_sampler* removed = llama_sampler_chain_remove(chain, eosBlockerIndex))
				llama_sampler_free(removed);
			firstLogged = true;
		}

		llama_sampler_accept(chain, next_id);
		out_tokens.push_back(next_id);

		if (next_id == eos_tok) { stopReason = "eos"; break; }

		if (!ip.stop.empty()) {
			std::string tmp;
			detokenize_raw(vocab, out_tokens, /*remove_special*/false, /*unparse_special*/true, tmp);
			if (ends_with_any(tmp, ip.stop)) { stopReason = "stop_string"; break; }
		}

		// advance kv
		{
			llama_token  tok_arr[1] = { next_id };
			llama_pos    pos_arr[1] = { (llama_pos)posAbs };
			int32_t      nseq_arr[1] = { 1 };
			llama_seq_id sid_val[1] = { 0 };
			llama_seq_id* sid_ptrs[1] = { &sid_val[0] };
			int8_t       logits_arr[1] = { 1 };

			llama_batch b{};
			b.n_tokens = 1;
			b.token = tok_arr;
			b.embd = nullptr;
			b.pos = pos_arr;
			b.n_seq_id = nseq_arr;
			b.seq_id = sid_ptrs;
			b.logits = logits_arr;

			if (llama_decode(impl->ctx, b) < 0) { stopReason = "decode_fail"; break; }
		}
		posAbs += 1;

		const int produced = (int)out_tokens.size();
		if ((produced % barEvery == 0) || (produced == budget)) {
			emitProgress("Gen " + make_progress_bar(produced, budget) + " (budget)", onLog);
		}
	}

	const auto tGenEnd = clock::now();

	llama_sampler_free(chain);

	{
		const int produced = (int)out_tokens.size();
		std::string suffix;
		if (stopReason == "max_tokens")       suffix = " (budget reached)";
		else if (stopReason == "eos")         suffix = " (EOS)";
		else if (stopReason == "stop_string") suffix = " (stopped early)";
		else                                   suffix = " (" + stopReason + ")";
		emitProgress("Gen " + make_progress_bar(produced, budget) + suffix, onLog);
	}

	// 6) Detokenize final
	std::string finalText;
	if (!out_tokens.empty()) {
		detokenize_raw(vocab, out_tokens, /*remove_special*/false, /*unparse_special*/true, finalText);
	}

	// 7) Timing + logs
	const auto tAllEnd = clock::now();
	const double msPrompt = std::chrono::duration<double, std::milli>(tAfterPrompt - tAllStart).count();
	const double msGen = std::chrono::duration<double, std::milli>(tGenEnd - tGenStart).count();
	const double msTotal = std::chrono::duration<double, std::milli>(tAllEnd - tAllStart).count();

	const int genTokens = (int)out_tokens.size();
	const double tokPerSec = (msGen > 0.0) ? (genTokens * 1000.0 / msGen) : 0.0;

	emitLog(std::string("Stop reason: ") + stopReason, errorOut, onLog);
	emitLog("Final tokens: " + std::to_string(genTokens), errorOut, onLog);

	std::string preview = finalText;
	for (char& c : preview) if (c == '\n' || c == '\r') c = ' ';
	if (preview.size() > 500) preview.resize(500), preview += "...";
	emitLog("Final (preview): \"" + preview + "\"", errorOut, onLog);

	emitLog("Time: total=" + std::to_string(msTotal / 1000.0) +
		"s, prompt=" + std::to_string(msPrompt / 1000.0) +
		"s, gen=" + std::to_string(msGen / 1000.0) + "s", errorOut, onLog);
	emitLog("Throughput: " + std::to_string(tokPerSec) + " tok/s", errorOut, onLog);

	if (outTokensPerSec) *outTokensPerSec = tokPerSec;
	return finalText;
}
