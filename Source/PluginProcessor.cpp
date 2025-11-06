#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LlamaRunner.h"
#include <unordered_set>
namespace
{
	double beatsPerBar(const juce::AudioPlayHead::CurrentPositionInfo& pos)
	{
		if (pos.timeSigNumerator > 0 && pos.timeSigDenominator > 0)
			return pos.timeSigNumerator * (4.0 / pos.timeSigDenominator);
		return 4.0;
	}

	void addNoteOn(juce::MidiBuffer& midi, int channel, int pitch, int velocity, int sampleOffset)
	{
		juce::MidiMessage m = juce::MidiMessage::noteOn(
			channel + 1, pitch, (juce::uint8)juce::jlimit(1, 127, velocity));
		midi.addEvent(m, sampleOffset);
	}

	void addNoteOff(juce::MidiBuffer& midi, int channel, int pitch, int sampleOffset)
	{
		juce::MidiMessage m = juce::MidiMessage::noteOff(channel + 1, pitch);
		midi.addEvent(m, sampleOffset);
	}
}

LLMidiAudioProcessor::LLMidiAudioProcessor()
	: AudioProcessor(juce::AudioProcessor::BusesProperties()
		.withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
	sequence.bars = 0;
	sequence.stepsPerBar = 4;
	sequence.midiChannel = 0;
	sequence.bpm = 120;
	sequence.data.clear();
}

LLMidiAudioProcessor::~LLMidiAudioProcessor() = default;

const juce::String LLMidiAudioProcessor::getName() const { return JucePlugin_Name; }
bool   LLMidiAudioProcessor::acceptsMidi()   const { return false; }
bool   LLMidiAudioProcessor::producesMidi()  const { return true; }
bool   LLMidiAudioProcessor::isMidiEffect()  const { return false; }
double LLMidiAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int  LLMidiAudioProcessor::getNumPrograms() { return 1; }
int  LLMidiAudioProcessor::getCurrentProgram() { return 0; }
void LLMidiAudioProcessor::setCurrentProgram(int) {}
const juce::String LLMidiAudioProcessor::getProgramName(int) { return {}; }
void LLMidiAudioProcessor::changeProgramName(int, const juce::String&) {}

void LLMidiAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	sr = sampleRate;
	spb = samplesPerBlock;
	haveSchedule = false;
	wasPlaying = false;
}

void LLMidiAudioProcessor::releaseResources() {}

bool LLMidiAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
	const auto out = layouts.getMainOutputChannelSet();
	return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

bool LLMidiAudioProcessor::getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const
{
	if (auto* ph = getPlayHead())
		return ph->getCurrentPosition(info);
	return false;
}

void LLMidiAudioProcessor::scheduleNowAtCurrentBar(const juce::AudioPlayHead::CurrentPositionInfo& pos)
{
	startBarPPQ = pos.ppqPositionOfLastBarStart;
	generator.requestBuild(sequence, startBarPPQ, beatsPerBar(pos));
	haveSchedule = true;
}

void LLMidiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
	juce::ScopedNoDenormals noDenormals;
	buffer.clear();
	struct EmittedKey {
		uint8_t type;
		uint8_t ch;
		uint8_t pitch;
		int     sample;
		bool operator==(const EmittedKey& o) const {
			return type == o.type && ch == o.ch && pitch == o.pitch && sample == o.sample;
		}
	};
	struct EmittedKeyHash {
		size_t operator()(const EmittedKey& k) const noexcept {
			// pack into 64-bit: t(1) | ch(7) | pitch(8) | sample(48)
			uint64_t v = 0;
			v |= (uint64_t)(k.type & 0x1);
			v |= (uint64_t)(k.ch & 0x7F) << 1;
			v |= (uint64_t)(k.pitch & 0xFF) << 8;
			v |= (uint64_t)(k.sample & 0xFFFFFFFFFFFFull) << 16;
			return std::hash<uint64_t>{}(v);
		}
	};
	std::unordered_set<EmittedKey, EmittedKeyHash> emittedThisBlock;
	juce::AudioPlayHead::CurrentPositionInfo pos;
	const bool havePos = getHostPosition(pos);

	if (!havePos || !pos.isPlaying)
	{
		if (wasPlaying)
		{
			flushAllActiveNotes(midi, 0);
			wasPlaying = false;
		}
		return;
	}

	wasPlaying = true;

	refreshSequenceFromGeneratorIfAvailable();

	if (!haveSchedule)
		scheduleNowAtCurrentBar(pos);

	auto timeline = generator.getCurrentTimeline();
	if (!timeline) return;

	if (timeline.get() != lastTimeline.get())
	{
		flushAllActiveNotes(midi, 0);
		didCatchUp = false;
		lastTimeline = timeline;
	}
	auto safeAddOn = [&](int ch, int pitch, int vel, int sampleOffset) {
		EmittedKey key{ 0, (uint8_t)ch, (uint8_t)pitch, sampleOffset };
		if (emittedThisBlock.insert(key).second) {
			juce::MidiMessage m = juce::MidiMessage::noteOn(ch + 1, pitch, (juce::uint8)juce::jlimit(1, 127, vel));
			midi.addEvent(m, sampleOffset);
		}
		};

	auto safeAddOff = [&](int ch, int pitch, int sampleOffset) {
		EmittedKey key{ 1, (uint8_t)ch, (uint8_t)pitch, sampleOffset };
		if (emittedThisBlock.insert(key).second) {
			juce::MidiMessage m = juce::MidiMessage::noteOff(ch + 1, pitch);
			midi.addEvent(m, sampleOffset);
		}
		};
	const double bpm = pos.bpm > 0.0 ? pos.bpm : (double)sequence.bpm;
	const double beatsPerSecond = bpm / 60.0;
	const double secondsPerBlock = (double)buffer.getNumSamples() / sr;
	const double blockPpqStart = pos.ppqPosition;
	const double blockPpqEnd = blockPpqStart + secondsPerBlock * beatsPerSecond;

	if (timeline->endPPQ <= blockPpqStart || timeline->startPPQ >= blockPpqEnd)
		return;

	performCatchUpIfNeeded(pos, timeline, midi, beatsPerSecond);

	for (const auto& e : timeline->events)
	{
		if (e.ppq < blockPpqStart) continue;
		if (e.ppq >= blockPpqEnd)  break;

		const double ppqFromBlockStart = e.ppq - blockPpqStart;
		const double secondsFromStart = ppqFromBlockStart / beatsPerSecond;
		const int sampleOffset = (int)juce::jlimit(
			0, buffer.getNumSamples() - 1,
			(int)std::floor(secondsFromStart * sr + 0.5));

		if (e.type == 0) {
			Key k{ e.channel, e.pitch };
			if (activeNotes.find(k) != activeNotes.end()) continue;
			safeAddOn(e.channel, e.pitch, e.velocity, sampleOffset);
			activeNotes.insert(k);
		}
		else {
			safeAddOff(e.channel, e.pitch, sampleOffset);
			Key k{ e.channel, e.pitch };
			activeNotes.erase(k);
		}
	}
}

bool LLMidiAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* LLMidiAudioProcessor::createEditor()
{
	return new LLMidiAudioProcessorEditor(*this);
}

void LLMidiAudioProcessor::requestLoadModelFromFile(const juce::File& file)
{
	LlamaContextParams p;
	p.n_ctx = 2048;
	p.n_batch = 2048;
	p.seed = 12345;

	generator.requestLoadModel(file.getFullPathName().toStdString(), p);
}

bool         LLMidiAudioProcessor::isModelReady() const { return generator.isModelReady(); }
juce::String LLMidiAudioProcessor::getLlmStatus() const { return generator.getLastLlmError(); }
juce::String LLMidiAudioProcessor::getLlmLog() const { return generator.getLogText(); }
void LLMidiAudioProcessor::clearLlmLog() { generator.clearLog(); }
void LLMidiAudioProcessor::requestLlmGeneratePattern(const std::string& naturalPrompt,
	int bars,
	int stepsPerBar,
	int defaultVelocity,
	int channel,
	int seed)
{
	generator.requestLlmGeneratePattern(naturalPrompt, bars, stepsPerBar,
		defaultVelocity, channel, seed);
}

void LLMidiAudioProcessor::refreshSequenceFromGeneratorIfAvailable()
{
	Sequence newSeq;
	if (!generator.getLatestGeneratedSequence(newSeq))
		return;

	const bool shapeChanged =
		(newSeq.bars != sequence.bars) ||
		(newSeq.stepsPerBar != sequence.stepsPerBar);

	sequence = newSeq;

	haveSchedule = false;
	didCatchUp = false;

	if (shapeChanged)
		activeNotes.clear();

	lastBurnCandidate = sequence;
}

void LLMidiAudioProcessor::performCatchUpIfNeeded(
	const juce::AudioPlayHead::CurrentPositionInfo& pos,
	const std::shared_ptr<const EventTimeline>& timeline,
	juce::MidiBuffer& midi,
	double /*beatsPerSecond*/)
{

	if (didCatchUp || !timeline) return;

	const double curPPQ = pos.ppqPosition;

	for (size_t i = 0; i < timeline->events.size(); ++i)
	{
		const auto& on = timeline->events[i];
		if (on.type != 0)     continue;
		if (on.ppq >= curPPQ) break;

		for (size_t j = i + 1; j < timeline->events.size(); ++j)
		{
			const auto& off = timeline->events[j];
			if (off.type == 1 && off.channel == on.channel && off.pitch == on.pitch)
			{
				if (off.ppq > curPPQ)
				{
					Key k{ on.channel, on.pitch };
					if (activeNotes.find(k) == activeNotes.end())
					{
						addNoteOn(midi, on.channel, on.pitch, on.velocity, 0);
						activeNotes.insert(k);
					}
				}
				break;
			}
		}
	}

	didCatchUp = true;
}

void LLMidiAudioProcessor::flushAllActiveNotes(juce::MidiBuffer& midi, int sampleOffset)
{
	for (const auto& k : activeNotes)
		addNoteOff(midi, k.ch, k.pitch, sampleOffset);
	activeNotes.clear();
}

void LLMidiAudioProcessor::getStateInformation(juce::MemoryBlock&) {}
void LLMidiAudioProcessor::setStateInformation(const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new LLMidiAudioProcessor();
}
