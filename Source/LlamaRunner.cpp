#include "LlamaRunner.h"
#include "llama.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>
#include <algorithm>
#include <mutex>

struct LlamaRunner::Impl {
	llama_model* model = nullptr;
	llama_context* ctx = nullptr;
	const llama_vocab* vocab = nullptr;

	llama_context_params ctxParams{};
	int n_ctx = 2048;
	int seed = 12345;

	std::mutex mtx;

	std::atomic<bool> cancelRequested{ false };
};

// ---------- helpers ----------
namespace {

	inline void appendLine(std::string* sink, const std::string& line) {
		if (!sink) return;
		if (!sink->empty() && sink->back() != '\n') *sink += "\n";
		*sink += line;
	}

	inline void emitLog(const std::string& s, std::string* /*errorOut*/,
		LlamaRunner::LogFn onLog) {
		if (onLog) onLog(s);
	}

	inline void emitProgress(const std::string& s, LlamaRunner::LogFn onLog) {
		if (onLog) onLog(std::string("[[PROGRESS]] ") + s);
	}

	inline std::string makeBar(int cur, int total, int width = 28) {
		if (total <= 0) total = 1;
		double ratio = std::clamp(double(cur) / double(total), 0.0, 1.0);
		int filled = int(std::round(ratio * width));
		std::string out = "[";
		out.append(filled, '#');
		out.append(width - filled, '.');
		out += "] " + std::to_string(cur) + "/" + std::to_string(total) +
			" (" + std::to_string(int(std::round(ratio * 100.0))) + "%)";
		return out;
	}

	// --- tokenization / detokenization (correct negative sizes) ---
	inline bool tokenize(const llama_vocab* vocab, const std::string& s,
		std::vector<llama_token>& out, std::string* err) {
		const bool add_special = true;
		const bool parse_special = true;

		int32_t need = llama_tokenize(vocab, s.c_str(), (int32_t)s.size(),
			nullptr, 0, add_special, parse_special);
		if (need == INT32_MIN) { if (err) *err = "tokenize overflow"; return false; }
		if (need < 0) need = -need;    // IMPORTANT
		if (need == 0) { if (err) *err = "tokenize() returned 0 tokens"; return false; }

		out.resize((size_t)need);
		int32_t got = llama_tokenize(vocab, s.c_str(), (int32_t)s.size(),
			out.data(), (int32_t)out.size(),
			add_special, parse_special);
		if (got == INT32_MIN) { if (err) *err = "tokenize overflow (2)"; return false; }
		if (got < 0) got = -got;       // IMPORTANT
		if (got == 0) { if (err) *err = "tokenize failed"; return false; }
		out.resize((size_t)got);
		return true;
	}

	inline bool detokenize(const llama_vocab* vocab,
		const std::vector<llama_token>& toks,
		std::string& out) {
		if (toks.empty()) { out.clear(); return true; }
		int need = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
			nullptr, 0, /*remove_special*/false, /*unparse_special*/true);
		if (need < 0) need = -need;
		if (need <= 0) { out.clear(); return true; }
		out.resize((size_t)need);
		int got = llama_detokenize(vocab, toks.data(), (int32_t)toks.size(),
			out.data(), (int32_t)out.size(),
			/*remove_special*/false, /*unparse_special*/true);
		if (got > 0) out.resize((size_t)got);
		return true;
	}

	// --- feed prompt tokens into context ---
	inline bool feed_prompt(llama_context* ctx,
		const std::vector<llama_token>& toks,
		std::string* err,
		LlamaRunner::LogFn onLog,
		std::atomic<bool>* cancelFlag) {
		const int total = (int)toks.size();
		const int every = std::max(1, total / 20);

		emitProgress(std::string("Prompt ") + makeBar(0, total), onLog);

		int32_t pos = 0;
		for (int i = 0; i < total; ++i) {
			if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
				if (err) *err = "canceled";
				return false;
			}
			llama_token tok = toks[(size_t)i];
			llama_pos   p = (llama_pos)pos++;

			int32_t nseq = 1;
			llama_seq_id sid = 0;
			llama_seq_id* sidp = &sid;
			int8_t logits = (int8_t)((i == total - 1) ? 1 : 0);

			llama_batch b{};
			b.n_tokens = 1;
			b.token = &tok;
			b.embd = nullptr;
			b.pos = &p;
			b.n_seq_id = &nseq;
			b.seq_id = &sidp;
			b.logits = &logits;

			if (llama_decode(ctx, b) < 0) {
				if (err) *err = "llama_decode(prompt) failed";
				return false;
			}

			if ((i % every) == 0)
				emitProgress(std::string("Prompt ") + makeBar(i, total), onLog);
		}

		emitProgress(std::string("Prompt ") + makeBar(total, total), onLog);
		return true;
	}

	// --- sampler chain ---
	inline llama_sampler* build_sampler_chain(
		const LlamaInferParams& ip,
		uint32_t seed,
		std::string* errorOut,
		LlamaRunner::LogFn onLog) {

		auto sparams = llama_sampler_chain_default_params();
		llama_sampler* chain = llama_sampler_chain_init(sparams);

		if (ip.repeat_penalty != 1.0f)
			llama_sampler_chain_add(chain,
				llama_sampler_init_penalties(64, ip.repeat_penalty, 0.0f, 0.0f));
		if (ip.top_k > 0)
			llama_sampler_chain_add(chain, llama_sampler_init_top_k(ip.top_k));
		if (ip.top_p > 0.0f && ip.top_p < 1.0f)
			llama_sampler_chain_add(chain, llama_sampler_init_top_p(ip.top_p, 1));

		if (ip.temperature > 0.0f) {
			llama_sampler_chain_add(chain, llama_sampler_init_temp(ip.temperature));
			llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
		}
		else {
			llama_sampler_chain_add(chain, llama_sampler_init_greedy());
		}

		return chain;
	}

} // namespace

// ---------- LlamaRunner ----------
LlamaRunner::LlamaRunner() : impl(new Impl) {}
LlamaRunner::~LlamaRunner() { unload(); }

bool LlamaRunner::isLoaded() const {
	return impl && impl->ctx != nullptr;
}

bool LlamaRunner::loadModel(const std::string& path,
	const LlamaContextParams& p,
	std::string& err) {
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

	impl->model = llama_model_load_from_file(path.c_str(), mparams);
	if (!impl->model) { err = "model_load failed"; unload(); return false; }

	impl->ctx = llama_init_from_model(impl->model, cparams);
	if (!impl->ctx) { err = "init_from_model failed"; unload(); return false; }

	impl->vocab = llama_model_get_vocab(impl->model);
	impl->n_ctx = (int)cparams.n_ctx;
	impl->seed = (p.seed >= 0 ? p.seed : 12345);

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		loadedModelPath_ = path;
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
void LlamaRunner::requestCancel() {
	if (impl) impl->cancelRequested.store(true, std::memory_order_relaxed);
}
std::string LlamaRunner::generate(const std::string& prompt,
	const LlamaInferParams& inParams,
	double* outTokensPerSec,
	std::string* errorOut,
	LogFn onLog) {
	if (!isLoaded()) { if (errorOut) *errorOut = "Model not loaded"; return {}; }

	std::lock_guard<std::mutex> guard(impl->mtx);

	impl->cancelRequested.store(false, std::memory_order_relaxed);

	// fresh ctx each call
	if (impl->ctx) { llama_free(impl->ctx); impl->ctx = nullptr; }
	impl->ctx = llama_init_from_model(impl->model, impl->ctxParams);
	if (!impl->ctx) { if (errorOut) *errorOut = "ctx reinit failed"; return {}; }
	impl->vocab = llama_model_get_vocab(impl->model);
	const auto* vocab = impl->vocab;

	using clock = std::chrono::steady_clock;
	const auto tAllStart = clock::now();

	// tokenize prompt (robust)
	std::vector<llama_token> prompt_tokens;
	if (!tokenize(vocab, prompt, prompt_tokens, errorOut)) return {};
	emitLog("Prompt tokens: " + std::to_string(prompt_tokens.size()), errorOut, onLog);
	if ((int)prompt_tokens.size() >= impl->n_ctx) {
		appendLine(errorOut, "prompt too long for n_ctx");
		return {};
	}

	// feed
	if (!feed_prompt(impl->ctx, prompt_tokens, errorOut, onLog, &impl->cancelRequested)) return {};
	const auto tAfterPrompt = clock::now();
	emitLog("Prompt ingested OK. Starting sampling...", errorOut, onLog);

	LlamaInferParams ip = inParams;

	const uint32_t seedUse = ip.seed >= 0 ? (uint32_t)ip.seed : (uint32_t)impl->seed;
	llama_sampler* chain =
		build_sampler_chain(ip, seedUse, errorOut, onLog);

	// seed sampler with prompt
	for (auto t : prompt_tokens)
		llama_sampler_accept(chain, t);

	// generation
	const auto tGenStart = clock::now();

	std::vector<llama_token> out_tokens;
	out_tokens.reserve(ip.max_tokens);

	std::string stopReason = "max_tokens";
	const int budget = std::max(1, ip.max_tokens);
	const int every = std::max(1, budget / 20);

	// incremental buffer + depth tracking (thread-local)
	static thread_local std::string text_so_far;
	static thread_local int  braceDepth = 0;    // { }
	static thread_local int  bracketDepth = 0;  // [ ]
	static thread_local bool seenOpeningBrace = false;

	for (int32_t i = 0; i < ip.max_tokens; ++i) {
		if (impl->cancelRequested.load(std::memory_order_relaxed)) {
			stopReason = "canceled";
			break;
		}

		llama_token next = llama_sampler_sample(chain, impl->ctx, -1);
		if (next == LLAMA_TOKEN_NULL) { stopReason = "decode_fail"; break; }

		llama_sampler_accept(chain, next);
		out_tokens.push_back(next);

		// advance KV
		llama_batch b{};
		llama_token tok = next;
		b.n_tokens = 1;
		b.token = &tok;
		llama_pos pos = (llama_pos)(prompt_tokens.size() + out_tokens.size() - 1);
		b.pos = &pos;
		int32_t nseq = 1; b.n_seq_id = &nseq;
		llama_seq_id sid = 0; llama_seq_id* sidp = &sid; b.seq_id = &sidp;
		int8_t logits = 1; b.logits = &logits;
		if (llama_decode(impl->ctx, b) < 0) { stopReason = "decode_fail"; break; }

		// ---------- incremental detokenize & stopping ----------
		if (i == 0) {
			text_so_far.clear();
			braceDepth = 0;
			bracketDepth = 0;
			seenOpeningBrace = false;
		}

		// detokenize just the last token into a small piece
		std::string piece;
		{
			llama_token tok_arr[1] = { next };
			int need = llama_detokenize(vocab, tok_arr, 1, nullptr, 0,
				/*remove_special*/false, /*unparse_special*/true);
			if (need < 0) need = -need;
			if (need > 0) {
				piece.resize((size_t)need);
				int got = llama_detokenize(vocab, tok_arr, 1, piece.data(), need,
					/*remove_special*/false, /*unparse_special*/true);
				if (got > 0 && got < need) piece.resize((size_t)got);
			}
		}

		// accumulate
		if (!piece.empty())
			text_so_far += piece;

		// update depth counters from this piece
		if (!piece.empty()) {
			for (char c : piece) {
				if (c == '{') { seenOpeningBrace = true; ++braceDepth; }
				else if (c == '}') { if (braceDepth > 0) --braceDepth; }
				else if (c == '[') { ++bracketDepth; }
				else if (c == ']') { if (bracketDepth > 0) --bracketDepth; }
			}
		}

		// 1) user-provided stop strings (wrappers only; we removed raw "}")
		if (!ip.stop.empty() && !piece.empty()) {
			bool hitStop = false;
			for (const auto& stopper : ip.stop) {
				if (!stopper.empty() && text_so_far.find(stopper) != std::string::npos) {
					hitStop = true; break;
				}
			}
			if (hitStop) {
				stopReason = "stop_str";
				// small debug preview
				std::string tail = text_so_far.size() > 120 ? text_so_far.substr(text_so_far.size() - 120) : text_so_far;
				emitLog(std::string("Stop(str) tail: \"") + tail + "\"", errorOut, onLog);
				goto gen_loop_done;
			}
		}

		// 2) balanced JSON object/array: stop once both depths return to zero
		if (seenOpeningBrace && braceDepth == 0 && bracketDepth == 0) {
			stopReason = "balanced_json";
			goto gen_loop_done;
		}

		// progress
		if (((i + 1) % every) == 0 || (i + 1) == budget)
			emitProgress(std::string("Gen ") + makeBar((i + 1), budget), onLog);
	}

gen_loop_done:


	const auto tGenEnd = clock::now();
	llama_sampler_free(chain);

	// detokenize & logs
	std::string text;
	if (stopReason != "canceled")
		detokenize(vocab, out_tokens, text);

	// timing
	const auto tAllEnd = clock::now();
	const double msPrompt = std::chrono::duration<double, std::milli>(tAfterPrompt - tAllStart).count();
	const double msGen = std::chrono::duration<double, std::milli>(tGenEnd - tGenStart).count();
	const double msTotal = std::chrono::duration<double, std::milli>(tAllEnd - tAllStart).count();

	const int genTokens = (int)out_tokens.size();
	const double tokps = (msGen > 0.0) ? (genTokens * 1000.0 / msGen) : 0.0;
	if (outTokensPerSec) *outTokensPerSec = tokps;

	emitLog(std::string("Stop reason: ") + stopReason, errorOut, onLog);
	emitLog("Final tokens: " + std::to_string(genTokens), errorOut, onLog);

	// Preview (single-line, trimmed)
	{
		std::string preview = text;
		for (char& c : preview) if (c == '\n' || c == '\r') c = ' ';
		if (preview.size() > 500) { preview.resize(500); preview += "..."; }
		emitLog("Final (preview): \"" + preview + "\"", errorOut, onLog);
	}

	emitLog("Time: total=" + std::to_string(msTotal / 1000.0) +
		"s, prompt=" + std::to_string(msPrompt / 1000.0) +
		"s, gen=" + std::to_string(msGen / 1000.0) + "s", errorOut, onLog);
	emitLog("Throughput: " + std::to_string(tokps) + " tok/s", errorOut, onLog);
	if (stopReason == "canceled") return {};
	return text;
}
