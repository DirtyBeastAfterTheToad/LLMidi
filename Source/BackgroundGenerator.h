#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <string>
#include "SequenceModel.h"
#include "MidiScheduler.h"
#include "Timeline.h"
#include "LlamaRunner.h"

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
    void requestBuild(const llmidi::Sequence& seq, double startBarPPQ, double beatsPerBar)
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

    void run() override
    {
        while (!threadShouldExit())
        {
            workEvent.wait(-1); // block until signaled
            if (threadShouldExit()) break;

            // 1) Handle any pending model load or smoke test first
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
                    lastLlmError = ok ? juce::String() : juce::String(err);
                }

                if (pendingSmokeTest)
                {
                    pendingSmokeTest = false;

                    if (!runner || !modelReady.load())
                    {
                        lastLlmError = "Smoke test: model not ready";
                    }
                    else
                    {
                        LlamaInferParams ip;
                        ip.temperature = 0.9f;   
                        ip.top_p = 1.0f;
                        ip.top_k = 0;
                        ip.max_tokens = 8;
                        ip.seed = 1234;
                        const std::string prompt = "[INST] Reply with exactly: OK [/INST]";
                        double tps = 0.0;
                        std::string err;
                        std::string out = runner->generate(prompt, ip, &tps, &err);

                        if (!err.empty())
                        {
                            lastLlmError = juce::String("Smoke test failed: ") + juce::String(err);
                        }
                        else if (out.empty())
                        {
                            lastLlmError = "Smoke test produced empty output (try grammar OK again or higher temp).";
                        }
                        else
                        {
                            juce::String shown = juce::String(out.substr(0, 64).c_str());
                            lastLlmError = juce::String("Smoke test OK, tps=") + juce::String(tps, 2)
                                + ", out: " + shown;
                        }
                    }
                }
            }

            // 2) Snapshot any pending build request
            bool doWork = false;
            llmidi::Sequence localSeq;
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

            // 3) Build a fresh timeline from the provided Sequence
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

            // 4) Publish atomically as const
            std::shared_ptr<const EventTimeline> timeline = mutableTimeline;
            std::atomic_store_explicit(&currentTimeline, timeline, std::memory_order_release);
        }
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
    llmidi::Sequence pendingSeq;
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
};
