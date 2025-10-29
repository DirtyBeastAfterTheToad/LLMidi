#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class LLMidiAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    LLMidiAudioProcessorEditor(LLMidiAudioProcessor&);
    ~LLMidiAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    LLMidiAudioProcessor& audioProcessor;

    juce::TextButton loadButton{ "Load Model..." };
    juce::TextButton smokeButton{ "Smoke Test" };
    juce::Label statusLabel;
    std::unique_ptr<juce::FileChooser> modelChooser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessorEditor)
};
