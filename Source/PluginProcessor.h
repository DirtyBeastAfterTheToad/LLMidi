#pragma once

#include <JuceHeader.h>
#include <set>

#include "SequenceModel.h"
#include "MidiScheduler.h"
#include "BackgroundGenerator.h"
#include "Timeline.h"

class LLMidiAudioProcessor final : public juce::AudioProcessor
{
public:
	LLMidiAudioProcessor();
	~LLMidiAudioProcessor() override;

	// Audio/MIDI lifecycle
	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
#ifndef JucePlugin_PreferredChannelConfigurations
	bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif
	void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	// Editor
	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override;

	// Plugin identity
	const juce::String getName() const override;
	bool acceptsMidi()   const override;
	bool producesMidi()  const override;
	bool isMidiEffect()  const override;
	double getTailLengthSeconds() const override;

	// Programs (not used)
	int getNumPrograms() override;
	int getCurrentProgram() override;
	void setCurrentProgram(int index) override;
	const juce::String getProgramName(int index) override;
	void changeProgramName(int index, const juce::String& newName) override;

	// State (not used)
	void getStateInformation(juce::MemoryBlock& destData) override;
	void setStateInformation(const void* data, int sizeInBytes) override;

	// --- Editor hooks ---
	const Sequence& getSequence() const { return sequence; }
	const Sequence& getLastGeneratedSequence() const { return lastBurnCandidate; }

	void requestLoadModelFromFile(const juce::File& file);

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
	// --- Helpers ---
	bool getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const;
	void scheduleNowAtCurrentBar(const juce::AudioPlayHead::CurrentPositionInfo& pos);

	void refreshSequenceFromGeneratorIfAvailable();
	void performCatchUpIfNeeded(const juce::AudioPlayHead::CurrentPositionInfo& pos,
		const std::shared_ptr<const EventTimeline>& timeline,
		juce::MidiBuffer& midi,
		double beatsPerSecond);
	void flushAllActiveNotes(juce::MidiBuffer& midi, int sampleOffset);

	struct Key { int ch; int pitch; };
	struct KeyLess {
		bool operator()(const Key& a, const Key& b) const {
			return a.ch < b.ch || (a.ch == b.ch && a.pitch < b.pitch);
		}
	};

private:
	// Host timing cache
	double sr = 44100.0;
	int    spb = 512;

	// Current pattern to schedule
	Sequence sequence;

	// LLM + timeline producer
	BackgroundGenerator generator;

	// Scheduling state
	bool   haveSchedule = false;
	double startBarPPQ = 0.0;

	// Playback state
	std::set<Key, KeyLess> activeNotes;
	std::shared_ptr<const EventTimeline> lastTimeline;
	bool didCatchUp = false;
	bool wasPlaying = false;

	// Editor helpers
	Sequence lastBurnCandidate;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessor)
};
