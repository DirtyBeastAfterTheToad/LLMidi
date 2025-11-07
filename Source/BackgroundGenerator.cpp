#include "BackgroundGenerator.h"
#include "SequenceValidator.h"
#include <regex>
#include <functional>
namespace
{
	static Sequence phraseToSequence(const ParsedPhrase& phrase,
		int defaultVelocity,
		int midiChannel,
		int fallbackBpm = 120)
	{
		Sequence seq;

		const int numBars = phrase.barsCount();
		const int stepsPerBar = phrase.stepsPerBar();

		seq.bars = numBars;
		seq.stepsPerBar = stepsPerBar;
		seq.midiChannel = (uint8_t)juce::jlimit(0, 15, midiChannel);
		seq.bpm = fallbackBpm;   // host tempo overrides during playback
		seq.key = "LLM draft";   // cosmetic only

		seq.data.resize((size_t)numBars);
		for (int b = 0; b < numBars; ++b)
		{
			auto& outBar = seq.data[(size_t)b];
			outBar.steps.reserve((size_t)stepsPerBar);

			for (int s = 0; s < stepsPerBar; ++s)
			{
				const StepEvent& inStep = phrase.bars[(size_t)b][(size_t)s];

				switch (inStep.kind)
				{
				case StepEvent::Kind::Rest:
					outBar.steps.push_back(Step::makeRest());
					break;

				case StepEvent::Kind::Sustain:
					outBar.steps.push_back(Step::makeSustain());
					break;

				case StepEvent::Kind::Notes:
				{
					std::vector<Note> ns;
					ns.reserve(inStep.notes.size());

					for (const auto& pn : inStep.notes)
					{
						Note n;
						n.midi = juce::jlimit(0, 127, pn.midi);
						n.velocity = (uint8_t)juce::jlimit(1, 127,
							pn.velocity > 0 ? pn.velocity : defaultVelocity);
						ns.push_back(n);
					}

					outBar.steps.push_back(Step::makeChord(std::move(ns)));
					break;
				}
				}
			}
		}

		juce::String validationErr;
		(void)validate(seq, validationErr);

		return seq;
	}
	static void coercePhraseShape(ParsedPhrase& p, int wantBars, int wantSteps, std::function<void(const juce::String&)> log)
	{
		auto makeRest = []() { return StepEvent{ StepEvent::Kind::Rest, {} }; };

		for (auto& bar : p.bars)
		{
			if ((int)bar.size() > wantSteps)
			{
				bar.resize((size_t)wantSteps);
				if (log) log("Coerce: trimmed extra steps to match requested stepsPerBar.");
			}
			else while ((int)bar.size() < wantSteps)
			{
				bar.push_back(makeRest());
			}
		}

		if ((int)p.bars.size() > wantBars)
		{
			p.bars.resize((size_t)wantBars);
			if (log) log("Coerce: trimmed extra bars to match requested bar count.");
		}
		else if ((int)p.bars.size() < wantBars)
		{
			std::vector<StepEvent> filler;
			if (!p.bars.empty()) filler = p.bars.back();
			else                 filler.assign((size_t)wantSteps, makeRest());

			while ((int)p.bars.size() < wantBars)
				p.bars.push_back(filler);

			if (log) log("Coerce: padded missing bars to match requested bar count.");
		}
	}

	static bool stripMarkdownFence(std::string& s) {
		size_t open = s.find("```");
		if (open == std::string::npos) return false;

		size_t close = s.rfind("```");
		if (close == std::string::npos || close <= open) return false;

		size_t probe = open + 3; // after ```
		size_t bracket = s.find('[', probe);
		if (bracket != std::string::npos && bracket < close) {
			size_t lastBracket = s.rfind(']', close - 1);
			if (lastBracket != std::string::npos && lastBracket > bracket) {
				s = s.substr(bracket, lastBracket - bracket + 1);
				return true;
			}
		}

		size_t contentStart = open + 3;
		size_t nl = s.find('\n', contentStart);
		if (nl != std::string::npos && nl + 1 < close) {
			s = s.substr(nl + 1, close - (nl + 1));
			return true;
		}

		return false;
	}

	static void stripTrailingSpecialTokens(std::string& s) {
		static const char* toks[] = {
			"<|end|>", "<|endoftext|>", "<|im_end|>", "</s>", "[/INST]"
		};
		bool changed = true;
		while (changed) {
			changed = false;
			while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
			for (auto* t : toks) {
				size_t L = std::strlen(t);
				if (s.size() >= L && s.compare(s.size() - L, L, t) == 0) {
					s.erase(s.size() - L);
					changed = true;
					break;
				}
			}
		}
	}

} // namespace

BackgroundGenerator::BackgroundGenerator()
	: juce::Thread("LLMidi-Generator")
{
}

BackgroundGenerator::~BackgroundGenerator()
{
	signalThreadShouldExit();
	notifyWorkAvailable();
	stopThread(2000);
}

void BackgroundGenerator::requestBuild(const Sequence& seq,
	double startBarPPQ,
	double beatsPerBar)
{
	const juce::ScopedLock sl(requestLock);
	pendingBuild.seq = seq;
	pendingBuild.startPPQ = startBarPPQ;
	pendingBuild.beatsPerBar = beatsPerBar;
	hasPendingBuild.store(true);
	notifyWorkAvailable();
}

void BackgroundGenerator::requestLoadModel(const std::string& path,
	const LlamaContextParams& p)
{
	const juce::ScopedLock sl(modelLock);
	pendingModelPath = path;
	pendingCtxParams = p;
	pendingModelLoad = true;
	notifyWorkAvailable();
}

std::shared_ptr<const EventTimeline> BackgroundGenerator::getCurrentTimeline() const
{
	return std::atomic_load_explicit(&currentTimeline, std::memory_order_acquire);
}

bool BackgroundGenerator::isModelReady() const noexcept
{
	return modelReady.load();
}

juce::String BackgroundGenerator::getLastLlmError() const
{
	return lastLlmError;
}

void BackgroundGenerator::requestLlmGeneratePattern(const std::string& naturalPrompt,
	int bars,
	int stepsPerBar,
	int defaultVelocity,
	int channel,
	int seed)
{
	const juce::ScopedLock sl(modelLock);
	pendingGenReq.prompt = naturalPrompt;
	pendingGenReq.bars = bars;
	pendingGenReq.steps = stepsPerBar;
	pendingGenReq.defaultVel = defaultVelocity;
	pendingGenReq.channel = channel;
	pendingGenReq.seed = seed;
	pendingGen = true;
	notifyWorkAvailable();
}

juce::String BackgroundGenerator::getLogText() const
{
	const juce::ScopedLock sl(logLock);
	juce::String combined;
	for (auto& line : logLines)
		combined << line << "\n";
	return combined.trimEnd();
}

bool BackgroundGenerator::getLatestGeneratedSequence(Sequence& outSeq) const
{
	if (!haveLatestGeneratedSeq.exchange(false, std::memory_order_acq_rel))
		return false;

	const juce::ScopedLock sl(latestSeqLock);
	outSeq = latestGeneratedSeq;
	return true;
}

void BackgroundGenerator::run()
{
	while (!threadShouldExit())
	{
		workEvent.wait(-1);
		if (threadShouldExit()) break;

		processModelRequests();

		// 2) Generation
		bool doGen = false;
		GenRequest gen{};
		{
			const juce::ScopedLock sl(modelLock);
			if (pendingGen)
			{
				pendingGen = false;
				doGen = true;
				gen = pendingGenReq;
			}
		}
		if (doGen) runPatternGeneration(gen);

		// 3) Timeline builds from external requests
		bool doBuild = false;
		BuildRequest build{};
		{
			const juce::ScopedLock sl(requestLock);
			if (hasPendingBuild.load())
			{
				hasPendingBuild.store(false);
				doBuild = true;
				build = pendingBuild;
			}
		}

		if (doBuild)
			publishTimeline(build.seq, build.startPPQ, build.beatsPerBar);
	}
}

void BackgroundGenerator::notifyWorkAvailable()
{
	workEvent.signal();
	if (!isThreadRunning())
		startThread();
}

void BackgroundGenerator::processModelRequests()
{
	std::string        modelPath;
	LlamaContextParams params{};

	{
		const juce::ScopedLock sl(modelLock);
		if (!pendingModelLoad) return;
		pendingModelLoad = false;
		modelPath = pendingModelPath;
		params = pendingCtxParams;
	}

	if (!runner)
		runner.reset(new LlamaRunner());

	std::string err;
	const bool ok = runner->loadModel(modelPath, params, err);
	modelReady.store(ok);

	if (ok)
	{
		lastLlmError = "Model load OK: " + juce::String(modelPath.c_str());
		appendLog(lastLlmError);
		loadedModelPath = modelPath;
	}
	else
	{
		loadedModelPath.clear();
		lastLlmError = "Model load FAILED: " + juce::String(err);
		appendLog(lastLlmError);
	}
}
juce::String BackgroundGenerator::getLoadedModelPath() const
{
	return juce::String(loadedModelPath.c_str());
}
void BackgroundGenerator::runPatternGeneration(const GenRequest& req)
{
	if (!runner || !modelReady.load())
	{
		appendLog("Pattern gen: model not ready");
		return;
	}

	appendLog("Pattern gen: starting...");

	const std::string user = req.prompt.empty()
		? "Nostalgic plucky arpeggio in E minor with space, light syncopation."
		: req.prompt;

	std::string modelPath;
	try { modelPath = runner->getLoadedModelPath(); }
	catch (...) { modelPath.clear(); }

	const juce::String mp(modelPath.c_str());
	const bool isPhi = mp.isNotEmpty() && mp.toLowerCase().contains("phi");

	const std::string prompt = buildPrompt(isPhi, user, req.bars, req.steps);

	LlamaInferParams ip;
	ip.temperature = 0.45f;
	ip.top_p = 0.95f;
	ip.top_k = 0;
	ip.repeat_penalty = 1.05f;
	ip.max_tokens = 2048;
	ip.seed = req.seed;
	ip.grammar.clear();
	ip.stop.clear();
	ip.stop.push_back("```");
	ip.stop.push_back("<|end|>");
	ip.stop.push_back("<|endoftext|>");
	ip.stop.push_back("<|im_end|>");
	ip.stop.push_back("</s>");
	ip.stop.push_back("[/INST]");

	double tps = 0.0;
	std::string genErr;
	const std::string raw = runner->generate(
		prompt,
		ip,
		&tps,
		/*errorOut*/ &genErr,
		/*onLog*/ [this](const std::string& line)
		{
			this->appendLog(juce::String(line));
		});
	if (!genErr.empty())
		appendLog("LLM error: " + juce::String(genErr));

	if (raw.empty())
	{
		appendLog("Pattern gen failed: empty raw output");
		return;
	}

	appendLog("Pattern gen raw size: " + juce::String((int)raw.size()));

	auto phraseOpt = sanitizeAndParse(raw, req.defaultVel);
	if (!phraseOpt.has_value())
	{
		appendLog("Pattern gen: empty phrase after parse.");
		return;
	}

	ParsedPhrase phrase = *phraseOpt;
	coercePhraseShape(phrase, req.bars, req.steps,
		[this](const juce::String& m) { this->appendLog(m); });
	const auto summary = summarize(phrase);

	{
		juce::String s;
		s << "Parsed "
			<< summary.bars << " bars x "
			<< summary.stepsPerBar << " steps; "
			<< "notes=" << summary.totalPlayableNotes
			<< ", playable steps=" << summary.totalPlayableSteps
			<< ", sustains=" << summary.totalSustains
			<< ", rests=" << summary.totalRests;
		appendLog(s);
	}

	Sequence builtSeq = phraseToSequence(phrase, req.defaultVel, req.channel, /*fallbackBpm*/ 120);

	{
		juce::String verr;
		if (!validate(builtSeq, verr))
			appendLog("Validation warning: " + verr);
	}

	{
		const juce::ScopedLock sl(latestSeqLock);
		latestGeneratedSeq = builtSeq;
		haveLatestGeneratedSeq.store(true);
	}

	appendLog("Pattern gen: sequence ready (waiting for host-aligned schedule).");
}

void BackgroundGenerator::publishTimeline(const Sequence& seq,
	double startPPQ,
	double beatsPerBar)
{
	auto mutableTimeline = std::make_shared<EventTimeline>();

	MidiScheduler sched;
	sched.buildFromSequence(seq, startPPQ, beatsPerBar);

	std::vector<const ScheduledMidi*> ptrs;
	sched.getEventsInRange(startPPQ, 1.0e12, ptrs);

	mutableTimeline->events.reserve(ptrs.size());
	for (auto* e : ptrs)
		mutableTimeline->events.push_back(*e);

	mutableTimeline->startPPQ = startPPQ;
	mutableTimeline->endPPQ = mutableTimeline->events.empty()
		? startPPQ
		: mutableTimeline->events.back().ppq;

	std::shared_ptr<const EventTimeline> timeline = mutableTimeline;
	std::atomic_store_explicit(&currentTimeline, timeline, std::memory_order_release);
}

std::string BackgroundGenerator::buildPrompt(bool isPhi,
	const std::string& user,
	int bars,
	int steps) const
{
	std::ostringstream rules;
	rules
		<< "You are a MIDI pattern generator.\n"
		<< "Return ONLY one JSON object with this exact shape:\n"
		<< "{\"b\":" << bars << ",\"s\":" << steps << ",\"e\":[ ... ]}\n"
		<< "Event format: [startStep, noteOrNotes, durationSteps, velocity]\n"
		<< "  - startStep: 0-based integer\n"
		<< "  - noteOrNotes: \"C4\" or [\"C4\",\"E4-80\",\"G4\"]\n"
		<< "  - durationSteps: positive integer\n"
		<< "  - velocity: 1..127 (default for notes without -NN)\n"
		<< "Rules:\n"
		<< "  - No text before '{' and nothing after the final '}'.\n"
		<< "  - In note fields, use note names only (e.g., \"C3\", \"G#4\").\n"
		<< "    Do NOT write chord symbols like \"Cm7\".\n"
		<< "  - COVER ALL BARS: for each bar k in 0.." << (bars - 1) << ", include AT LEAST\n"
		<< "    ONE  EVENT whose startStep is in\n"
		<< "    [k*" << steps << ", (k+1)*" << steps << " - 1].\n"
		<< "  - If multiple notes BEGIN at the same step, put them in ONE event (a chord array).\n"
		<< "  - Prefer musical variation: you may add single-note events as arpeggios."
		<< "Indices reminder for b=" << bars << ", s=" << steps << ":\n"
		<< "  bar0: steps 0.." << (steps - 1)
		<< ", bar1: " << (steps) << ".." << (2 * steps - 1)
		<< ", bar2: " << (2 * steps) << ".." << (3 * steps - 1)
		<< ", bar3: " << (3 * steps) << ".." << (4 * steps - 1) << " etc.\n"
		<< "Example:\n"
		<< "{\"b\":8,\"s\":4,\"e\":[[0,[\"A#3\",\"D4\",\"F4\"],4,85],[4,[\"G3\",\"C4\",\"D#4\"],4,85],[8,[\"A#3\",\"D4\"],2,85],[9,\"F4\",1,72],[12,[\"G3\",\"C4\",\"D#4\"],4,85]]}\n"
		<< "Style: " << user << "\n";
	std::ostringstream prompt;
	if (isPhi) {
		prompt << "<|user|>\n" << rules.str()
			<< "<|end|>\n<|assistant|>";
	}
	else {
		prompt << "<s>[INST] <<SYS>>" << rules.str() << "<</SYS>> [/INST]";
	}
	return prompt.str();
}

std::optional<ParsedPhrase> BackgroundGenerator::sanitizeAndParse(const std::string& raw,
	int defaultVelocity)
{
	std::string text = raw;

	(void)stripMarkdownFence(text);
	stripTrailingSpecialTokens(text);

	while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
	while (!text.empty() && std::isspace((unsigned char)text.back()))  text.pop_back();

	// Extract balanced top-level {...}
	auto extractBalancedBraces = [](const std::string& s) -> std::optional<std::string>
		{
			size_t start = s.find('{');
			if (start == std::string::npos) return std::nullopt;

			int depth = 0;
			for (size_t i = start; i < s.size(); ++i) {
				char c = s[i];
				if (c == '{') ++depth;
				else if (c == '}') {
					--depth;
					if (depth == 0) return s.substr(start, i - start + 1);
				}
			}
			return std::nullopt;
		};

	if (auto balanced = extractBalancedBraces(text))
		text = *balanced;
	else {
		// last-chance slice between first '{' and last '}'
		auto l = text.find('{');
		auto r = text.rfind('}');
		if (l != std::string::npos && r != std::string::npos && r > l)
			text = text.substr(l, r - l + 1);
	}

	ParsedPhrase phrase;
	std::string perr;
	appendLog("Slice to parse (len=" + juce::String((int)text.size()) + "): " +
		juce::String(text.substr(0, std::min<size_t>(text.size(), 160)).c_str()));

	if (!parseEventJson(text, defaultVelocity, perr, phrase)) {
		appendLog("Parse error: " + juce::String(perr));
		return std::nullopt;
	}

	if (phrase.bars.empty())
		return std::nullopt;

	return phrase;
}

void BackgroundGenerator::clearLog()
{
	const juce::ScopedLock sl(logLock);
	logLines.clear();
}

void BackgroundGenerator::appendLog(const juce::String& line)
{
	const juce::ScopedLock sl(logLock);

	static const juce::String kProgressPrefix{ "[[PROGRESS]] " };

	if (line.startsWith(kProgressPrefix))
	{
		// “Live” progress: replace the previous line
		juce::String cleaned = line.fromFirstOccurrenceOf(kProgressPrefix, false, false);

		if (logLines.isEmpty())
		{
			logLines.add(cleaned);
		}
		else
		{
			logLines.set(logLines.size() - 1, cleaned);
		}
	}
	else
	{
		logLines.add(line);
		while (logLines.size() > kMaxLogLines)
			logLines.remove(0);
	}
}
void BackgroundGenerator::requestCancelGeneration()
{
	if (runner)
	{
		appendLog("[[PROGRESS]] Canceling generation...");
		runner->requestCancel();
	}
}
