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

		size_t probe = open + 3;
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

}

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
		<< "Task: Generate a " << bars << "-bar musical pattern for the request: \"" << user << "\".\n"
		<< "\n"
		<< "Output REQUIREMENTS (must follow EXACTLY):\n"
		<< "* Output ONLY a single JSON object. No prose, no markdown, no code fences, no prefix/suffix.\n"
		<< "* JSON schema:\n"
		<< "{\n"
		<< "  \"b\": " << bars << ",\n"
		<< "  \"s\": " << steps << ",               // integer: steps per bar\n"
		<< "  \"e\": [                // array of events\n"
		<< "    [startStep, noteOrNotes, durationSteps, velocity],\n"
		<< "    ...\n"
		<< "  ]\n"
		<< "}\n"
		<< "\n"
		<< "Event format details:\n"
		<< "* startStep: integer >= 0. The timeline is in steps, totalSteps = b * s.\n"
		<< "* durationSteps: integer >= 1.\n"
		<< "* velocity: integer 1..127 (typical 60..110).\n"
		<< "* noteOrNotes: either a string note token (e.g., \"C4\", \"Bb3-90\", \"F#5\")\n"
		<< "  or an array of note tokens for chords (e.g., [\"C4\",\"E4-88\",\"G4\"]).\n"
		<< "  A note token may include an optional per-note velocity suffix \"-NN\" (1..127),\n"
		<< "  which overrides the event velocity for that note.\n"
		<< "\n"
		<< "Rhythm & durations (important):\n"
		<< "* Do NOT quantize everything to durationSteps=1.\n"
		<< "* Use a VARIETY of durationSteps values (e.g., 1,2,3,4...).\n"
		<< "* At least 50% of events MUST have durationSteps >= 2 (sustained notes/chords).\n"
		<< "* Include some longer notes/chords that span across steps or even across bar boundaries when musical.\n"
		<< "* If the style truly calls for staccato, you may use more 1-step notes, but still keep >=30% with durationSteps >= 2.\n"
		<< "* Quick cheat sheet for s=" << steps << ": 1 = " << (steps == 4 ? "quarter" : "1/" + std::to_string(steps))
		<< " note, 2 = longer sustain, etc.\n"
		<< "\n"
		<< "Mini example (for illustration only — your output must be a single JSON object without comments):\n"
		<< "{\n"
		<< "  \"b\": 8,\n"
		<< "  \"s\": 4,\n"
		<< "  \"e\": [\n"
		<< "    [0,  \"C4\",        2, 96],\n"
		<< "    [2,  [\"E4\",\"G4\"],3, 92],\n"
		<< "    [8,  \"D4-88\",     4, 88],\n"
		<< "    [16, [\"E4\",\"G4\"],2, 96],\n"
		<< "    [22, \"B3\",        1, 90]\n"
		<< "  ]\n"
		<< "}\n"
		<< "\n"
		<< "Constraints and guidance:\n"
		<< "* Use exactly b=" << bars << " bars. Use s=" << steps << ".\n"
		<< "* Keep events within the total range (0 .. b*s-1). Overlap is allowed.\n"
		<< "* Sort events in ascending startStep. Avoid zero/negative durations.\n"
		<< "* Style should reflect the request (key, register, density, repetition/variation).\n"
		<< "* Use varied durationSteps; at least half of the events must sustain (durationSteps >= 2).\n"
		<< "* Avoid making all events durationSteps=1 unless the style explicitly demands strict staccato.\n"
		<< "* COVER ALL BARS: for each bar k in 0.." << (bars - 1) << ", include AT LEAST ONE event with startStep in "
		<< "[k*" << steps << ", (k+1)*" << steps << " - 1].\n"
		<< "* If multiple notes BEGIN at the same step, put them in ONE event (a chord array).\n"
		<< "\n"
		<< "Important: Return ONLY the JSON object. Start with '{' and end with '}'.\n";

	// Wrap for different model chat templates
	std::ostringstream prompt;
	if (isPhi)
	{
		prompt << "<|user|>\n" << rules.str()
			<< "<|end|>\n<|assistant|>";
	}
	else
	{
		prompt << "<s>[INST] <<SYS>>\n" << rules.str() << "\n<</SYS>> [/INST]";
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
