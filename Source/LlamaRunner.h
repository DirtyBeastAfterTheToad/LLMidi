#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

struct LlamaContextParams {
	int   n_ctx = 2048;
	int   n_batch = 512;
	int   n_gpu_layers = 0; // CPU-only
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
	std::string grammar;              // optional grammar text (empty = off)
};

class LlamaRunner {
public:
	using LogFn = std::function<void(const std::string&)>;

	LlamaRunner();
	~LlamaRunner();

	LlamaRunner(const LlamaRunner&) = delete;
	LlamaRunner& operator=(const LlamaRunner&) = delete;

	bool loadModel(const std::string& modelPath,
		const LlamaContextParams& p,
		std::string& errorOut);

	void unload();
	std::string getLoadedModelPath() const;
	bool isLoaded() const;

	std::string generate(const std::string& prompt,
		const LlamaInferParams& ip,
		double* outTokensPerSec,
		std::string* errorOut,
		LogFn onLog);
	void requestCancel();
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	bool modelLoaded_ = false;
	mutable std::mutex stateMutex_;
	std::string loadedModelPath_;
};
