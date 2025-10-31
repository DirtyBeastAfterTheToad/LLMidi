#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <string>
#include "SequenceModel.h"
#include "MidiScheduler.h"
#include "Timeline.h"
#include "LlamaRunner.h"
#include "LlmSequenceParser.h"
#include "LlmGenAdapter.h"

// Builds timelines off the audio thread and exposes the latest safely.
// Also owns the LlamaRunner and handles model load + simple smoke tests.
class BackgroundGenerator : private juce::Thread
{
public:
    BackgroundGenerator()
        : juce::Thread("LLMidi-Generator")
    {
    }

    ~BackgroundGenerator() override
    {
        signalThreadShouldExit();
        notifyWorkAvailable();
        stopThread(2000);
    }

    // Request a new build. Safe to call from the message thread or editor.
    // Stores the parameters and wakes the thread.
    void requestBuild(const Sequence& seq, double startBarPPQ, double beatsPerBar)
    {
        const juce::ScopedLock sl(requestLock);
        pendingSeq = seq;
        pendingStartBarPPQ = startBarPPQ;
        pendingBeatsPerBar = beatsPerBar;
        hasPending.store(true);
        notifyWorkAvailable();
    }

    // Request to load a model (GGUF) on the background thread.
    void requestLoadModel(const std::string& path, const LlamaContextParams& p)
    {
        const juce::ScopedLock sl(modelLock);
        pendingModelPath = path;
        pendingCtxParams = p;
        pendingModelLoad = true;
        notifyWorkAvailable();
    }

    // Optional: ask the background thread to run a tiny LLM smoke test.
    // Result goes into lastLlmError (either error text or a brief "OK ..." message).
    void requestLlmSmokeTest()
    {
        const juce::ScopedLock sl(modelLock);
        pendingSmokeTest = true;
        notifyWorkAvailable();
    }

    // Accessors for audio/editor threads (read-only)
    std::shared_ptr<const EventTimeline> getCurrentTimeline() const
    {
        return std::atomic_load_explicit(&currentTimeline, std::memory_order_acquire);
    }

    bool isModelReady() const noexcept { return modelReady.load(); }
    juce::String getLastLlmError() const { return lastLlmError; }

    void requestLlmGeneratePattern(const std::string& naturalPrompt,
        int bars,
        int stepsPerBar,
        int defaultVelocity,
        int channel,
        int seed)
    {
        const juce::ScopedLock sl(modelLock);
        pendingGen = true;
        pendingGenPrompt = naturalPrompt;
        pendingGenBars = bars;
        pendingGenSteps = stepsPerBar;
        pendingGenDefaultVel = defaultVelocity;
        pendingGenChannel = channel;
        pendingGenSeed = seed;
        notifyWorkAvailable();
    }
    void run() override
    {
        while (!threadShouldExit())
        {
            workEvent.wait(-1);
            if (threadShouldExit()) break;

            // --- 1) handle model load + capture smoke request, under lock
            bool doSmoke = false;
            {
                const juce::ScopedLock sl(modelLock);

                if (pendingModelLoad)
                {
                    pendingModelLoad = false;

                    if (!runner)
                        runner.reset(new LlamaRunner());

                    std::string err;
                    const bool ok = runner->loadModel(pendingModelPath, pendingCtxParams, err);
                    modelReady.store(ok);

                    if (ok)
                    {
                        lastLlmError = "Model load OK: " + juce::String(pendingModelPath.c_str());
                        appendLog(lastLlmError);
                    }
                    else
                    {
                        lastLlmError = "Model load FAILED: " + juce::String(err);
                        appendLog(lastLlmError);
                    }
                }

                if (pendingSmokeTest)
                {
                    pendingSmokeTest = false;
                    doSmoke = true;
                }
            } // <-- release modelLock here

            // --- 2) run smoke test outside the lock
            if (doSmoke)
            {
                if (!runner || !modelReady.load())
                {
                    lastLlmError = "Smoke test: model not ready";
                    appendLog(lastLlmError);
                }
                else
                {
                    appendLog("Smoke test: starting...");

                    LlamaInferParams ip;
                    ip.temperature = 0.9f;
                    ip.top_p = 1.0f;
                    ip.top_k = 0;
                    ip.repeat_penalty = 1.0f;
                    ip.max_tokens = 4;   // keep short
                    ip.seed = 1234;
                    ip.grammar.clear();

                    const std::string prompt =
                        "<s>[INST] <<SYS>>You are a helpful assistant.<</SYS>> "
                        "Say only the word OK and nothing else. [/INST]";


                    double tps = 0.0;
                    std::string err;
                    std::string out = runner->generate(prompt, ip, &tps, &err);

                    if (!err.empty())
                    {
                        lastLlmError = juce::String("Smoke test failed: ") + juce::String(err);
                        appendLog(lastLlmError);
                    }
                    else if (out.empty())
                    {
                        lastLlmError = "Smoke test: empty output.";
                        appendLog(lastLlmError);
                    }
                    else
                    {
                        juce::String shown = juce::String(out.substr(0, 64).c_str());
                        lastLlmError = juce::String("Smoke test OK, tps=") + juce::String(tps, 2)
                            + ", out: " + shown;
                        appendLog(lastLlmError);
                    }
                }
            }

            // --- 2b) capture pending pattern-gen outside lock
            bool doGen = false;
            std::string natPrompt;
            int genBars = 8, genSteps = 8, genDefaultVel = 100, genChannel = 0, genSeed = 12345;

            {
                const juce::ScopedLock sl(modelLock);
                if (pendingGen) {
                    pendingGen = false;
                    doGen = true;
                    natPrompt = pendingGenPrompt;
                    genBars = pendingGenBars;
                    genSteps = pendingGenSteps;
                    genDefaultVel = pendingGenDefaultVel;
                    genChannel = pendingGenChannel;
                    genSeed = pendingGenSeed;
                }
            }

            // --- 2c) run LLM pattern generation
            if (doGen) {
                if (!runner || !modelReady.load()) {
                    appendLog("Pattern gen: model not ready");
                }
                else {
                    appendLog("Pattern gen: starting...");

                    // ---- Prepare task text
                    std::string user = natPrompt;
                    if (user.empty()) {
                        user = "Nostalgic plucky arpeggio in E minor with space, light syncopation.";
                    }

                    // Core rules: *array-of-arrays only*, no objects/keys, strict step set
                    std::ostringstream rules;
                    rules
                        << "You are a step-based MIDI pattern generator.\n"
                        << "\n"
                        << "OUTPUT REQUIREMENTS:\n"
                        << "- Output ONLY a JSON array of exactly " << genBars << " bars.\n"
                        << "- The top-level array MUST have length " << genBars << ".\n"
                        << "- Each bar MUST be a JSON array of exactly " << genSteps << " steps.\n"
                        << "- So each bar must have length " << genSteps << ".\n"
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
                        << "\n"
                        << "STYLE HINTS:\n"
                        << "- Use \".\" (rest) to leave space; not every step should trigger.\n"
                        << "- Long notes = write a NOTE once, then use \"-\" in the following steps to keep it ringing.\n"
                        << "  Example bar (4 steps): [\"E3\", \"-\", \"-\", \".\"] means E3 rings, then silence.\n"
                        << "- You may mix low single notes and higher chords.\n"
                        << "\n"
                        << "DO NOT ADD ANY TEXT OUTSIDE THE JSON.\n"
                        << "- No explanations, no comments, no code fences, no extra sentences.\n"
                        << "- After the final ']' of the top-level array, STOP IMMEDIATELY.\n"
                        << "- The last character in your response MUST be ']'.\n";

                    // ---- Detect model family for prompt layout
                    std::string modelPath;
                    try {
                        modelPath = runner->getLoadedModelPath(); 
                    }
                    catch (...) {
                        modelPath.clear();
                    }
                    const juce::String mp(modelPath.c_str());
                    const bool isPhi = mp.isNotEmpty() && mp.toLowerCase().contains("phi");

                    // ---- Build the actual prompt per model
                    std::ostringstream prompt;
                    if (isPhi) {
                        // Phi-3 chat format:
                        // <|user|>\n...<|end|>\n<|assistant|>
                        prompt << "<|user|>\n"
                            << rules.str()
                            << "\nTask: " << user << "\n"
                            << "Return ONLY the JSON.\n"
                            << "<|end|>\n"
                            << "<|assistant|>";
                    }
                    else {
                        // Mistral Instruct v0.3 format:
                        // <s>[INST] <<SYS>>...rules...<</SYS>> ...user/task... [/INST]
                        prompt << "<s>[INST] <<SYS>>"
                            << rules.str()
                            << "<</SYS>> "
                            << "Task: " << user << " "
                            << "Return ONLY the JSON. [/INST]";
                    }

                    // ---- Inference params (tight for JSON)
                    LlamaInferParams ip;
                    ip.temperature = 0.30f;
                    ip.top_p = 0.90f;
                    ip.top_k = 0;           // rely on nucleus only for stability
                    ip.repeat_penalty = 1.05f;
                    ip.max_tokens = 512;         // you said 512 is fine for 8 bars
                    ip.seed = genSeed;
                    ip.grammar.clear();               // keep off for now; enable later if you add a JSON grammar
                    ip.stop.clear();
                    if (isPhi) {
                        // Help the model terminate cleanly once it would switch roles
                        ip.stop = { "<|end|>" };
                    }
                    else {
                        // Mistral will usually emit </s> (EOS id=2). Adding it as a string stop is harmless.
                        ip.stop = { "</s>" };
                    }

                    double tps = 0.0;
                    std::string genLog; 
                    const std::string raw = runner->generate(prompt.str(), ip, &tps, &genLog);

                    if (!genLog.empty())
                        appendLog("LLM dbg:\n" + juce::String(genLog));

                    if (raw.empty())
                    {
                        appendLog("Pattern gen failed: empty raw output");
                    }
                    else
                    {
                        appendLog("Pattern gen raw size: " + juce::String((int)raw.size()));

                        // ---- Minimal cleanup
                        std::string trimmed = raw;
                        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
                        while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))  trimmed.pop_back();

                        // ---- Parse JSON -> ParsedPhrase
                        ParsedPhrase phrase;
                        std::string perr;
                        if (!parseJsonBars(trimmed, genDefaultVel, perr, phrase)) {
                            appendLog("Parse error: " + juce::String(perr));

                            // Try to slice between first '[' and last ']' to salvage a clean array
                            auto l = trimmed.find('[');
                            auto r = trimmed.rfind(']');
                            if (l != std::string::npos && r != std::string::npos && r > l) {
                                std::string salvage = trimmed.substr(l, r - l + 1);
                                perr.clear();
                                ParsedPhrase salvagePhrase;
                                if (parseJsonBars(salvage, genDefaultVel, perr, salvagePhrase)) {
                                    phrase = std::move(salvagePhrase);
                                    appendLog("Recovered JSON by slicing outer brackets.");
                                }
                                else {
                                    appendLog("Salvage also failed: " + juce::String(perr));
                                }
                            }
                        }

                        if (phrase.bars.empty()) {
                            appendLog("Pattern gen: empty phrase after parse.");
                        }
                        else {

                        auto summary = summarize(phrase);

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

                        // ---- NEW: turn ParsedPhrase into a schedulable llmidi::Sequence
                        Sequence builtSeq =
                            phraseToSequence(phrase,
                                genDefaultVel,
                                genChannel,
                                /*fallbackBpm*/120);

                        // Validate & log any issues
                        {
                            juce::String verr;
                            if (!validate(builtSeq, verr))
                            {
                                appendLog("Validation warning: " + verr);
                            }
                        }

                        // Store it as "latestGeneratedSeq" so UI / processor can reuse
                        {
                            const juce::ScopedLock sl(latestSeqLock);
                            latestGeneratedSeq = builtSeq;
                            haveLatestGeneratedSeq.store(true);
                        }

                        // Immediately build and publish a new timeline for audition
                        // We'll align it to bar 0 at PPQ 0.0 and assume 4 beats/bar for now.
                        const double startPpq = 0.0;
                        const double beatsPerBar = 4.0; // TODO: infer from host or prompt

                        {
                            // Build a fresh MidiScheduler (local stack)
                            MidiScheduler schedTmp;
                            schedTmp.buildFromSequence(builtSeq, startPpq, beatsPerBar);

                            // Translate to an EventTimeline and publish atomically,
                            // mirroring the code later in run() that handles requestBuild().
                            auto mutableTimeline = std::make_shared<EventTimeline>();

                            std::vector<const ScheduledMidi*> ptrs;
                            schedTmp.getEventsInRange(startPpq, 1.0e12, ptrs);

                            mutableTimeline->events.reserve(ptrs.size());
                            for (auto* e : ptrs)
                                mutableTimeline->events.push_back(*e);

                            mutableTimeline->startPPQ = startPpq;
                            mutableTimeline->endPPQ = mutableTimeline->events.empty()
                                ? startPpq
                                : mutableTimeline->events.back().ppq;

                            std::shared_ptr<const EventTimeline> timeline = mutableTimeline;
                            std::atomic_store_explicit(&currentTimeline, timeline, std::memory_order_release);
                        }

                        appendLog("Pattern gen: timeline published for audition.");
                        }

                    }
                }
            }


            // --- 3) timeline build (unchanged)
            bool doWork = false;
            Sequence localSeq;
            double localStartPPQ = 0.0;
            double localBeatsPerBar = 4.0;

            {
                const juce::ScopedLock sl(requestLock);
                if (hasPending.load())
                {
                    localSeq = pendingSeq;
                    localStartPPQ = pendingStartBarPPQ;
                    localBeatsPerBar = pendingBeatsPerBar;
                    hasPending.store(false);
                    doWork = true;
                }
            }

            if (!doWork)
                continue;

            auto mutableTimeline = std::make_shared<EventTimeline>();

            MidiScheduler sched;
            sched.buildFromSequence(localSeq, localStartPPQ, localBeatsPerBar);

            std::vector<const ScheduledMidi*> ptrs;
            sched.getEventsInRange(localStartPPQ, 1.0e12, ptrs);

            mutableTimeline->events.reserve(ptrs.size());
            for (auto* e : ptrs)
                mutableTimeline->events.push_back(*e);

            mutableTimeline->startPPQ = localStartPPQ;
            mutableTimeline->endPPQ = mutableTimeline->events.empty()
                ? localStartPPQ
                : mutableTimeline->events.back().ppq;

            std::shared_ptr<const EventTimeline> timeline = mutableTimeline;
            std::atomic_store_explicit(&currentTimeline, timeline, std::memory_order_release);
        }
    }
    juce::String getLogText() const
    {
        const juce::ScopedLock sl(logLock);
        juce::String combined;
        for (auto& line : logLines)
            combined << line << "\n";
        return combined.trimEnd();
    }
    bool getLatestGeneratedSequence(Sequence& outSeq) const
    {
        if (!haveLatestGeneratedSeq.load()) return false;
        const juce::ScopedLock sl(latestSeqLock);
        outSeq = latestGeneratedSeq;
        return true;
    }
private:
    void notifyWorkAvailable()
    {
        workEvent.signal();
        if (!isThreadRunning())
            startThread();
    }

    // ===== Pending build request =====
    double pendingBeatsPerBar = 4.0;
    juce::CriticalSection requestLock;
    Sequence pendingSeq;
    double pendingStartBarPPQ = 0.0;
    std::atomic<bool> hasPending{ false };

    // ===== Published timeline (immutable) =====
    std::shared_ptr<const EventTimeline> currentTimeline{ nullptr };

    // ===== Waker =====
    juce::WaitableEvent workEvent;

    // ===== LLM ownership and state =====
    std::unique_ptr<LlamaRunner> runner;
    std::atomic<bool> modelReady{ false };
    juce::String lastLlmError;

    // ===== Pending model ops =====
    juce::CriticalSection modelLock;
    bool pendingModelLoad = false;
    bool pendingSmokeTest = false;
    std::string pendingModelPath;
    LlamaContextParams pendingCtxParams;
    void appendLog(const juce::String& line)
    {
        const juce::ScopedLock sl(logLock);
        logLines.add(line);

        // keep only the last ~10 lines
        const int maxLines = 200;
        while (logLines.size() > maxLines)
            logLines.remove(0);
    }
    Sequence latestGeneratedSeq;
    std::atomic<bool> haveLatestGeneratedSeq{ false };
    juce::CriticalSection latestSeqLock;
    // Convert ParsedPhrase (LLM JSON) -> llmidi::Sequence so MidiScheduler can play it.
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
        seq.bpm = fallbackBpm;            // host tempo will override in playback but we keep a value
        seq.key = "LLM draft";            // purely cosmetic for now

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
                {
                    outBar.steps.push_back(Step::makeRest());
                    break;
                }

                case StepEvent::Kind::Sustain:
                {
                    outBar.steps.push_back(Step::makeSustain());
                    break;
                }

                case StepEvent::Kind::Notes:
                {
                    // Translate each PlayedNote {midi, velocity} -> llmidi::Note
                    std::vector<Note> ns;
                    ns.reserve(inStep.notes.size());
                    for (const auto& pn : inStep.notes)
                    {
                        Note n;
                        n.midi = juce::jlimit(0, 127, pn.midi);
                        n.velocity = (uint8_t)juce::jlimit(1, 127, pn.velocity > 0 ? pn.velocity : defaultVelocity);
                        ns.push_back(n);
                    }

                    // Decide Note vs Chord automatically
                    outBar.steps.push_back(Step::makeChord(std::move(ns)));
                    break;
                }
                }
            }
        }
        
        // Sanity check / clamp in case the model lied
        juce::String validationErr;
        if (!validate(seq, validationErr))
        {
            // We won’t throw; we’ll just log later. Sequence may be partially weird,
            // but still playable. You could also choose to zero it out here.
        }

        return seq;
    }

    // ===== Debug / status log =====
    juce::CriticalSection logLock;
    juce::StringArray logLines;


    bool        pendingGen = false;
    std::string pendingGenPrompt;
    int         pendingGenBars = 8;
    int         pendingGenSteps = 8;
    int         pendingGenDefaultVel = 100;
    int         pendingGenChannel = 0;
    int         pendingGenSeed = 12345;
};
