#include "PluginProcessor.h"
#include "PluginEditor.h"

LLMidiAudioProcessor::LLMidiAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
#endif
{
    // Create a simple default sequence for smoke testing: 2 bars, 4 steps
    sequence.bars = 2;
    sequence.stepsPerBar = 4;
    sequence.midiChannel = 0;
    sequence.bpm = 120;

    sequence.data.resize(2);
    // Bar 1: C4 held for full bar
    sequence.data[0].steps = {
        llmidi::Step::makeChord({ {60,100} }), // C4
        llmidi::Step::makeSustain(),
        llmidi::Step::makeSustain(),
        llmidi::Step::makeSustain()
    };
    // Bar 2: G3 held for full bar
    sequence.data[1].steps = {
        llmidi::Step::makeChord({ {55,100} }), // G3
        llmidi::Step::makeSustain(),
        llmidi::Step::makeSustain(),
        llmidi::Step::makeSustain()
    };
}


LLMidiAudioProcessor::~LLMidiAudioProcessor() {}

const juce::String LLMidiAudioProcessor::getName() const { return JucePlugin_Name; }

bool LLMidiAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}
bool LLMidiAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}
bool LLMidiAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}
double LLMidiAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int LLMidiAudioProcessor::getNumPrograms() { return 1; }
int LLMidiAudioProcessor::getCurrentProgram() { return 0; }
void LLMidiAudioProcessor::setCurrentProgram(int) {}
const juce::String LLMidiAudioProcessor::getProgramName(int) { return {}; }
void LLMidiAudioProcessor::changeProgramName(int, const juce::String&) {}

void LLMidiAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    spb = samplesPerBlock;
    haveSchedule = false;
    // Start the background thread lazily (it autostarts on first request)
}

void LLMidiAudioProcessor::releaseResources() {}

#ifndef JucePlugin_PreferredChannelConfigurations
bool LLMidiAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif
    return true;
#endif
}
#endif

bool LLMidiAudioProcessor::getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const
{
    if (auto* ph = getPlayHead())
        return ph->getCurrentPosition(info);
    return false;
}
static double computeBeatsPerBar(const juce::AudioPlayHead::CurrentPositionInfo& pos)
{
    if (pos.timeSigNumerator > 0 && pos.timeSigDenominator > 0)
        return pos.timeSigNumerator * (4.0 / pos.timeSigDenominator);
    return 4.0; // fallback
}
static void addNoteOn(juce::MidiBuffer& midi, int channel, int pitch, int velocity, int sampleOffset)
{
    juce::MidiMessage m = juce::MidiMessage::noteOn(channel + 1, pitch, (juce::uint8)juce::jlimit(1, 127, velocity));
    midi.addEvent(m, sampleOffset);
}

static void addNoteOff(juce::MidiBuffer& midi, int channel, int pitch, int sampleOffset)
{
    juce::MidiMessage m = juce::MidiMessage::noteOff(channel + 1, pitch);
    midi.addEvent(m, sampleOffset);
}
void LLMidiAudioProcessor::scheduleNowAtCurrentBar(const juce::AudioPlayHead::CurrentPositionInfo& pos)
{
    const double beatsPerBar = computeBeatsPerBar(pos);
    const double barStart = pos.ppqPositionOfLastBarStart;
    startBarPPQ = barStart;

    generator.requestBuild(sequence, startBarPPQ, beatsPerBar);
    haveSchedule = true;
}

void LLMidiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    juce::AudioPlayHead::CurrentPositionInfo pos;
    const bool havePos = getHostPosition(pos);
    if (!havePos || !pos.isPlaying)
        return;

    if (!haveSchedule)
        scheduleNowAtCurrentBar(pos); // requests build on the background thread

    // Fetch the current immutable timeline
    auto timeline = generator.getCurrentTimeline();
    if (!timeline)
        return;

    // If the timeline changed, reset catch-up state
    if (timeline.get() != lastTimeline.get())
    {
        activeNotes.clear();
        didCatchUp = false;
        lastTimeline = timeline;
    }

    // Compute this block's PPQ range using host tempo
    const double bpm = (pos.bpm > 0.0 ? pos.bpm : (double)sequence.bpm);
    const double beatsPerSecond = bpm / 60.0;
    const double secondsPerBlock = (double)buffer.getNumSamples() / sr;
    const double blockPpqStart = pos.ppqPosition;
    const double blockPpqEnd = blockPpqStart + secondsPerBlock * beatsPerSecond;

    // Nothing to do if timeline is entirely outside this block
    if (timeline->endPPQ <= blockPpqStart || timeline->startPPQ >= blockPpqEnd)
        return;

    // One-time catch-up so sustained notes already in progress become audible now
    performCatchUpIfNeeded(pos, timeline, midi, beatsPerSecond);

    // Emit scheduled events that land inside this audio block
    for (const auto& e : timeline->events)
    {
        if (e.ppq < blockPpqStart) continue;
        if (e.ppq >= blockPpqEnd)  break;

        const double ppqFromBlockStart = e.ppq - blockPpqStart;
        const double secondsFromStart = ppqFromBlockStart / beatsPerSecond;
        int sampleOffset = (int)juce::jlimit(0, buffer.getNumSamples() - 1,
            (int)std::floor(secondsFromStart * sr + 0.5));

        if (e.type == 0) // NoteOn
        {
            Key k{ e.channel, e.pitch };
            if (activeNotes.find(k) != activeNotes.end())
                continue; // already turned on by catch-up

            addNoteOn(midi, e.channel, e.pitch, e.velocity, sampleOffset);
            activeNotes.insert(k);
        }
        else // NoteOff
        {
            addNoteOff(midi, e.channel, e.pitch, sampleOffset);
            Key k{ e.channel, e.pitch };
            activeNotes.erase(k);
        }
    }
}


// Scan the timeline at curPPQ and emit NoteOns for any notes that are already "on".
void LLMidiAudioProcessor::performCatchUpIfNeeded(const juce::AudioPlayHead::CurrentPositionInfo& pos,
    const std::shared_ptr<const EventTimeline>& timeline,
    juce::MidiBuffer& midi,
    double beatsPerSecond)
{
    if (didCatchUp || !timeline) return;

    const double curPPQ = pos.ppqPosition;

    // Find notes with onPPQ < curPPQ < offPPQ
    // We assume NoteOn events have type==0 and NoteOff type==1 for same (channel,pitch)
    for (size_t i = 0; i < timeline->events.size(); ++i)
    {
        const auto& e = timeline->events[i];
        if (e.type != 0) continue; // NoteOn
        if (e.ppq >= curPPQ) break;

        // Find its matching NoteOff
        for (size_t j = i + 1; j < timeline->events.size(); ++j)
        {
            const auto& off = timeline->events[j];
            if (off.type == 1 && off.channel == e.channel && off.pitch == e.pitch)
            {
                if (off.ppq > curPPQ)
                {
                    // This note is held right now -> emit immediate NoteOn
                    Key k{ e.channel, e.pitch };
                    if (activeNotes.find(k) == activeNotes.end())
                    {
                        // At start of this block (sampleOffset 0)
                        addNoteOn(midi, e.channel, e.pitch, e.velocity, 0);
                        activeNotes.insert(k);
                    }
                }
                break;
            }
        }
    }

    didCatchUp = true;
}
bool LLMidiAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* LLMidiAudioProcessor::createEditor() { return new LLMidiAudioProcessorEditor(*this); }
void LLMidiAudioProcessor::getStateInformation(juce::MemoryBlock&) {}
void LLMidiAudioProcessor::setStateInformation(const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LLMidiAudioProcessor(); }
