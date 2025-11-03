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
    BackgroundGenerator();
    ~BackgroundGenerator() override;

    // Request a new build. Safe to call from the message thread or editor.
    void requestBuild(const Sequence& seq, double startBarPPQ, double beatsPerBar);

    // Request to load a model (GGUF) on the background thread.
    void requestLoadModel(const std::string& path, const LlamaContextParams& p);

    // Accessors for audio/editor threads (read-only)
    std::shared_ptr<const EventTimeline> getCurrentTimeline() const;
    bool isModelReady() const noexcept;
    juce::String getLastLlmError() const;

    void requestLlmGeneratePattern(const std::string& naturalPrompt,
        int bars,
        int stepsPerBar,
        int defaultVelocity,
        int channel,
        int seed);

    juce::String getLogText() const;
    bool getLatestGeneratedSequence(Sequence& outSeq) const;

private:
    // juce::Thread
    void run() override;

    struct BuildRequest {
        Sequence seq;
        double startPPQ = 0.0;
        double beatsPerBar = 4.0;
    };

    struct GenRequest {
        std::string prompt;
        int bars = 8;
        int steps = 8;
        int defaultVel = 100;
        int channel = 0;
        int seed = 12345;
    };

    void notifyWorkAvailable();

    void processModelRequests();
    void runPatternGeneration(const GenRequest& req);

    void publishTimeline(const Sequence& seq, double startPPQ, double beatsPerBar);
    std::string buildPrompt(bool isPhi, const std::string& user, int bars, int steps) const;
    std::optional<ParsedPhrase> sanitizeAndParse(const std::string& raw, int defaultVelocity);

    void appendLog(const juce::String& line);

private:
    juce::CriticalSection requestLock;
    BuildRequest pendingBuild{};
    std::atomic<bool> hasPendingBuild{ false };

    std::shared_ptr<const EventTimeline> currentTimeline{ nullptr };

    juce::WaitableEvent workEvent;

    std::unique_ptr<LlamaRunner> runner;
    std::atomic<bool> modelReady{ false };
    juce::String lastLlmError;

    juce::CriticalSection modelLock;
    bool pendingModelLoad = false;
    bool pendingSmokeTest = false;
    std::string pendingModelPath;
    LlamaContextParams pendingCtxParams{};

    bool pendingGen = false;
    GenRequest pendingGenReq{};

    Sequence latestGeneratedSeq;
    std::atomic<bool> haveLatestGeneratedSeq{ false };
    juce::CriticalSection latestSeqLock;

    juce::CriticalSection logLock;
    juce::StringArray logLines;

    static constexpr int kMaxLogLines = 200;
};
