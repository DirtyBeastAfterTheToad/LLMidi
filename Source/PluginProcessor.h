#pragma once
#include <JuceHeader.h>
#include "SequenceModel.h"
#include "MidiScheduler.h"
#include "BackgroundGenerator.h"
#include "Timeline.h"
#include <set>

class LLMidiAudioProcessor : public juce::AudioProcessor
{
public:
    LLMidiAudioProcessor();
    ~LLMidiAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    const llmidi::Sequence& getSequence() const { return sequence; }
    void requestLoadModelFromFile(const juce::File& file);
    void requestLlmSmokeTest();

    // For editor status polling
    bool isModelReady() const;
    juce::String getLlmStatus() const;
    juce::String getLlmLog() const;
    void requestLlmGeneratePattern(const std::string& naturalPrompt,
        int bars,
        int stepsPerBar,
        int defaultVelocity,
        int channel,
        int seed);
private:
    // Host timing cache
    double sr = 44100.0;
    int spb = 512;

    // Core data and schedule
    llmidi::Sequence sequence;

    BackgroundGenerator generator;
    // Where in PPQ the sequence starts (aligned to bar)
    bool haveSchedule = false;
    double startBarPPQ = 0.0;

    // Helpers
    bool getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const;
    void scheduleNowAtCurrentBar(const juce::AudioPlayHead::CurrentPositionInfo& pos);
    struct Key { int ch; int pitch; };
    struct KeyLess {
        bool operator()(const Key& a, const Key& b) const
        {
            return a.ch < b.ch || (a.ch == b.ch && a.pitch < b.pitch);
        }
    };

    std::set<Key, KeyLess> activeNotes;
    std::shared_ptr<const EventTimeline> lastTimeline;
    bool didCatchUp = false;
    void performCatchUpIfNeeded(const juce::AudioPlayHead::CurrentPositionInfo& pos,
        const std::shared_ptr<const EventTimeline>& timeline,
        juce::MidiBuffer& midi,
        double beatsPerSecond);
    void flushAllActiveNotes(juce::MidiBuffer& midi, int sampleOffset);
    bool wasPlaying = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessor)
    void refreshSequenceFromGeneratorIfAvailable();
};
