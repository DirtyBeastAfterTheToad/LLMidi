#include "BackgroundGenerator.h"
#include "SequenceValidator.h"
namespace {

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
		seq.bpm = fallbackBpm;     // host tempo will override in playback
		seq.key = "LLM draft";     // cosmetic

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
						n.velocity = (uint8_t)juce::jlimit(1, 127, pn.velocity > 0 ? pn.velocity : defaultVelocity);
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

void BackgroundGenerator::requestBuild(const Sequence& seq, double startBarPPQ, double beatsPerBar)
{
	const juce::ScopedLock sl(requestLock);
	pendingBuild.seq = seq;
	pendingBuild.startPPQ = startBarPPQ;
	pendingBuild.beatsPerBar = beatsPerBar;
	hasPendingBuild.store(true);
	notifyWorkAvailable();
}

void BackgroundGenerator::requestLoadModel(const std::string& path, const LlamaContextParams& p)
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
	if (!haveLatestGeneratedSeq.load()) return false;
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

		// 1) Model ops
		processModelRequests();

		// 2) Generation
		bool doGen = false;
		GenRequest gen{};
		{
			const juce::ScopedLock sl(modelLock);
			if (pendingGen) {
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
			if (hasPendingBuild.load()) {
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
	bool doLoad = false;
	std::string modelPath;
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
	}
	else
	{
		lastLlmError = "Model load FAILED: " + juce::String(err);
		appendLog(lastLlmError);
	}
}


void BackgroundGenerator::runPatternGeneration(const GenRequest& req)
{
	if (!runner || !modelReady.load()) {
		appendLog("Pattern gen: model not ready");
		return;
	}

	appendLog("Pattern gen: starting...");

	const std::string user = req.prompt.empty()
		? "Nostalgic plucky arpeggio in E minor with space, light syncopation."
		: req.prompt;

	// Detect model family for prompt layout
	std::string modelPath;
	try { modelPath = runner->getLoadedModelPath(); }
	catch (...) { modelPath.clear(); }
	const juce::String mp(modelPath.c_str());
	const bool isPhi = mp.isNotEmpty() && mp.toLowerCase().contains("phi");

	const std::string prompt = buildPrompt(isPhi, user, req.bars, req.steps);

	LlamaInferParams ip;
	ip.temperature = 0.30f;
	ip.top_p = 0.90f;
	ip.top_k = 0;
	ip.repeat_penalty = 1.05f;
	ip.max_tokens = 512;
	ip.seed = req.seed;
	ip.grammar.clear();
	ip.stop = isPhi ? std::vector<std::string>{ "<|end|>" }
	: std::vector<std::string>{ "</s>" };

	double tps = 0.0;
	std::string genLog;
	const std::string raw = runner->generate(
		prompt,
		ip,
		&tps,
		/*errorOut*/ nullptr,
		/*onLog*/ [this](const std::string& line) {
			this->appendLog(juce::String(line));
		}
	);

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

	const auto phrase = *phraseOpt;
	auto summary = summarize(phrase);

	{
		juce::String s;
		s << "Parsed "
			<< summary.bars << " bars x "
			<< summary.stepsPerBar << " steps; "
			<< "notes=" << summary.totalPlayableNotes
			<< ", playable steps=" << summary.totalPlayableSteps
			<< ", sustains=" << summary.totalSustains
			<< ", rests=" << summary.totalRests
			<< "\nPreview: " << summary.shortPreview;
		appendLog(s);
	}

	// Build a playable sequence
	Sequence builtSeq = phraseToSequence(phrase, req.defaultVel, req.channel, /*fallbackBpm*/120);

	{
		juce::String verr;
		if (!validate(builtSeq, verr))
			appendLog("Validation warning: " + verr);
	}

	// Save latest
	{
		const juce::ScopedLock sl(latestSeqLock);
		latestGeneratedSeq = builtSeq;
		haveLatestGeneratedSeq.store(true);
	}

	// Publish an audition timeline (bar 0, PPQ 0.0, assume 4/4)
	publishTimeline(builtSeq, /*startPPQ*/0.0, /*beatsPerBar*/4.0);

	appendLog("Pattern gen: timeline published for audition.");
}

void BackgroundGenerator::publishTimeline(const Sequence& seq, double startPPQ, double beatsPerBar)
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

std::string BackgroundGenerator::buildPrompt(bool isPhi, const std::string& user, int bars, int steps) const
{
	std::ostringstream rules;
	rules
		<< "You are a step-based MIDI pattern generator.\n"
		<< "\n"
		<< "OUTPUT REQUIREMENTS:\n"
		<< "- Output ONLY a JSON array of exactly " << bars << " bars.\n"
		<< "- The top-level array MUST have length " << bars << ".\n"
		<< "- Each bar MUST be a JSON array of exactly " << steps << " steps.\n"
		<< "- So each bar must have length " << steps << ".\n"
		<< "\n"
		<< "EACH STEP MUST BE ONE OF:\n"
		<< "  \".\"                     = rest / silence\n"
		<< "  \"-\"                     = sustain/hold the previous note or chord\n"
		<< "  \"NOTE\"                  = e.g. \"A#3\" or \"A#3-72\" (velocity 1..127)\n"
		<< "  [\"NOTE\", \"NOTE\", ...]  = chord. Each NOTE may include -velocity.\n"
		<< "\n"
		<< "NOTE FORMAT IS STRICT:\n"
		<< "- Legal single note: \"A#3\" (pitch + octave)\n"
		<< "- Or \"A#3-72\" (same, plus velocity 1..127 after a dash)\n"
		<< "- Chord example: [\"A#3\",\"D#4\",\"F4-100\"]\n"
		<< "- ILLEGAL: \"B-3\" (dash before octave is NOT allowed)\n"
		<< "- ILLEGAL: \"E-4\" (not allowed)\n"
		<< "- ILLEGAL: objects like {\"note\": \"C4\"}\n"
		<< "- Notes never start with '-'. Example: \"E4-64\" is valid, but \"-E4-64\" is invalid.\n"
		<< "- Never merge symbols. Each step must be a single symbol: '.', '-', or a note. No combos like '-.' allowed.\n"
		<< "\n"
		<< "STYLE HINTS:\n"
		<< "- Use rests \".\" to leave space; not every step should trigger.\n"
		<< "- You may hold notes/chords across multiple steps using \"-\" instead of repeating them.\n"
		<< "- You may mix bass notes and higher notes or chords.\n"
		<< "\n"
		<< "DO NOT ADD ANY TEXT OUTSIDE THE JSON.\n"
		<< "- No explanations, no comments, no code fences, no extra sentences.\n"
		<< "- After the final ']' of the top-level array, STOP IMMEDIATELY.\n"
		<< "- The last character in your response MUST be ']'.\n";

	std::ostringstream prompt;
	if (isPhi) {
		// Phi-3 chat format:
		prompt << "<|user|>\n"
			<< rules.str()
			<< "\nTask: " << user << "\n"
			<< "Return ONLY the JSON.\n"
			<< "<|end|>\n"
			<< "<|assistant|>";
	}
	else {
		// Mistral Instruct v0.3 format:
		prompt << "<s>[INST] <<SYS>>"
			<< rules.str()
			<< "<</SYS>> "
			<< "Task: " << user << " "
			<< "Return ONLY the JSON. [/INST]";
	}

	return prompt.str();
}

std::optional<ParsedPhrase> BackgroundGenerator::sanitizeAndParse(const std::string& raw, int defaultVelocity)
{
	// Trim whitespace
	std::string trimmed = raw;
	while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
	while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))  trimmed.pop_back();

	// Slice to the last closing bracket if the model appended junk
	if (auto pos = trimmed.rfind(']'); pos != std::string::npos)
		trimmed = trimmed.substr(0, pos + 1);

	// Quick typo sanitation
	{
		juce::String s = trimmed.c_str();
		s = s.replace("\"-.\"", "\"-\"");
		s = s.replace("\".-\"", "\".\"");
		s = s.replace("\"_\"", "\"-\"");
		trimmed = s.toStdString();
	}

	ParsedPhrase phrase;
	std::string perr;
	if (!parseJsonBars(trimmed, defaultVelocity, perr, phrase))
	{
		appendLog("Parse error: " + juce::String(perr));

		// Try to salvage between first '[' and last ']'
		auto l = trimmed.find('[');
		auto r = trimmed.rfind(']');
		if (l != std::string::npos && r != std::string::npos && r > l) {
			std::string salvage = trimmed.substr(l, r - l + 1);
			perr.clear();
			ParsedPhrase salvagePhrase;
			if (parseJsonBars(salvage, defaultVelocity, perr, salvagePhrase)) {
				appendLog("Recovered JSON by slicing outer brackets.");
				phrase = std::move(salvagePhrase);
			}
			else {
				appendLog("Salvage also failed: " + juce::String(perr));
			}
		}
	}

	if (phrase.bars.empty())
		return std::nullopt;

	return phrase;
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
