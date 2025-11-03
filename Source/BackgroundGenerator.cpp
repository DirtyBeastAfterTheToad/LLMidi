#include "BackgroundGenerator.h"
#include "SequenceValidator.h"

// ---------- Local helpers (file-scope) ----------
namespace
{
    // Convert a parsed phrase into our runtime Sequence
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

        // Best-effort validation (warning only)
        juce::String validationErr;
        (void)validate(seq, validationErr);

        return seq;
    }
} // namespace

// ---------- BackgroundGenerator ----------
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
    // Atomically test-and-clear
    if (!haveLatestGeneratedSeq.exchange(false, std::memory_order_acq_rel))
        return false;

    const juce::ScopedLock sl(latestSeqLock);
    outSeq = latestGeneratedSeq;    // (optional) you can also std::move if you want
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
    }
    else
    {
        lastLlmError = "Model load FAILED: " + juce::String(err);
        appendLog(lastLlmError);
    }
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

    const std::string raw = runner->generate(
        prompt,
        ip,
        &tps,
        /*errorOut*/ nullptr,
        /*onLog*/ [this](const std::string& line)
        {
            this->appendLog(juce::String(line));
        });

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
    const auto summary = summarize(phrase);

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

    // Build a playable sequence and store it
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

    // Do not publish an immediate audition timeline; wait for the host to request a
    // schedule aligned to the bar via requestBuild() from the processor.
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

    // Optional short dump (disabled by default)
    constexpr bool kDumpTimeline = true;
    if (kDumpTimeline)
    {
        appendLog("Timeline events: " + juce::String((int)mutableTimeline->events.size()));
        for (int i = 0; i < (int)mutableTimeline->events.size() && i < 60; ++i)
        {
            const auto& e = mutableTimeline->events[(size_t)i];
            appendLog(juce::String(e.type == 0 ? " On " : "Off ")
                + "ch=" + juce::String(e.channel)
                + " p=" + juce::String(e.pitch)
                + " ppq=" + juce::String(e.ppq, 6));
        }
    }
}

std::string BackgroundGenerator::buildPrompt(bool isPhi,
    const std::string& user,
    int bars,
    int steps) const
{
    // Compact, token-friendly rules with your examples (no escapes in the prompt content)
    std::ostringstream rules;
    rules
        << "You are a step-based MIDI pattern generator.\n"
        << "\n"
        << "RULES:\n"
        << "- Output ONLY a JSON array of exactly " << bars << " bars.\n"
        << "- Each bar is a list of exactly " << steps << " steps.\n"
        << "- Each step can be:\n"
        << "  .   = rest\n"
        << "  -   = sustain previous note or chord\n"
        << "  NOTE = e.g. A#3 or A#3-80 (velocity 1–127)\n"
        << "  [NOTE, ...] = chord (each note may include -velocity)\n"
        << "\n"
        << "NOTES:\n"
        << "- Use C, C#, D, D#, E, F, F#, G, G#, A, A#, B with octaves.\n"
        << "- Flats (b) allowed (e.g. Bb3).\n"
        << "- Do NOT use a dash before octave (B-3, E-4 are invalid).\n"
        << "- Do NOT start notes with '-' (-E4-64 invalid).\n"
        << "- No combos like '-.' or '--'.\n"
        << "- Bars cannot start with a sustain '-'.\n"
        << "\n"
        << "STYLE:\n"
        << "- Minor or trap groove.\n"
        << "- Mix short notes, chords, sustains, and rests.\n"
        << "- Use '-' to hold notes, '.' for silence.\n"
        << "\n"
        << "EXAMPLES:\n"
        << "[[\"C4\",\"-\",\"E4\",\"-\"],[\"G4\",\"-\",\"C5\",\"-\"]]\n"
        << "[[\"A#3\",\"-\",\"D#4\",\"-\"],[\"F4\",\"-\",\".\",\"G#3\"],[\"C#4\",\"-\",\"D#4\",\"-\"],[\"G#3\",\".\",\"A#3\",\"-\"]]\n"
        << "[[[\"C3\",\"E3\",\"G3\"],\"-\",\"-\",\"-\"],[[\"F3\",\"A3\",\"C4\"],\"-\",\"-\",\"-\"],[[\"G3\",\"B3\",\"D4\"],\"-\",\"-\",\"-\"],[[\"C3\",\"E3\",\"G3\"],\"-\",\"-\",\"-\"]]\n"
        << "[[[\"A#3\",\"D#4\",\"F4\"],\"-\",\"-\",\"-\",\"-\",\"-\",\"-\",\"-\"],[\"A#3\",\"-\",\"C#4\",\".\",\"D#4\",\"-\",\"F4\",\".\"],[[\"G#3\",\"C#4\",\"D#4\"],\"-\",\"-\",\"-\",[\"F3\",\"A#3\",\"C#4\"],\"-\",\"-\",\"-\"],[\"G#3\",\"-\",\"-\",\".\",\"A#3\",\"C#4\",\"-\",\"D#4\"],[[\"B2\",\"F#3\",\"D#4\"],\"-\",\"A#3\",\"C#4\",\"-\",\"D#4\",\"-\",\".\"],[\".\",\".\",\"A#2\",\"-\",\"-\",\".\",\"G#2\",\"-\"],[\"A#3\",\"C#4\",\"D#4\",\"F4\",\"G#4\",\"F4\",\"D#4\",\"C#4\"],[[\"A#3\",\"D#4\",\"F4\",\"A#4\"],\"-\",\"-\",\"-\",\"-\",\"-\",\"-\",\"-\"]]\n"
        << "\n"
        << "Output ONLY the JSON array. No text, no comments, no quotes around it.\n"
        << "After the final ']', stop.\n";
    std::ostringstream prompt;
    if (isPhi)
    {
        // Phi-3 chat format
        prompt << "<|user|>\n"
            << rules.str()
            << "\nTask: " << user << "\n"
            << "Return ONLY the JSON.\n"
            << "<|end|>\n"
            << "<|assistant|>";
    }
    else
    {
        // Mistral Instruct v0.3 format
        prompt << "<s>[INST] <<SYS>>"
            << rules.str()
            << "<</SYS>> "
            << "Task: " << user << " "
            << "Return ONLY the JSON. [/INST]";
    }

    return prompt.str();
}

std::optional<ParsedPhrase> BackgroundGenerator::sanitizeAndParse(const std::string& raw,
    int defaultVelocity)
{
    auto extractBalancedArray = [](const std::string& s) -> std::optional<std::string>
        {
            size_t start = s.find('[');
            if (start == std::string::npos) return std::nullopt;

            int depth = 0;
            for (size_t i = start; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '[') ++depth;
                else if (c == ']')
                {
                    --depth;
                    if (depth == 0)
                        return s.substr(start, i - start + 1);
                }
            }
            return std::nullopt;
        };

    // Trim
    std::string trimmed = raw;
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))  trimmed.pop_back();

    // Keep up to last ']'
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

    // Prefer balanced outer array if available
    if (auto balanced = extractBalancedArray(trimmed))
        trimmed = *balanced;

    ParsedPhrase phrase;
    std::string perr;

    if (!parseJsonBars(trimmed, defaultVelocity, perr, phrase))
    {
        appendLog("Parse error: " + juce::String(perr));

        // Try slice first '[' .. last ']'
        auto l = trimmed.find('[');
        auto r = trimmed.rfind(']');
        if (l != std::string::npos && r != std::string::npos && r > l)
        {
            std::string salvage = trimmed.substr(l, r - l + 1);
            perr.clear();
            ParsedPhrase salvagePhrase;
            if (parseJsonBars(salvage, defaultVelocity, perr, salvagePhrase))
            {
                appendLog("Recovered JSON by slicing outer brackets.");
                phrase = std::move(salvagePhrase);
            }
            else
            {
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
