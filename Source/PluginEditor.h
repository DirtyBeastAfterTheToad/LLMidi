#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
class LLMidiAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    LLMidiAudioProcessorEditor(LLMidiAudioProcessor&);
    ~LLMidiAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Timer to poll background thread status
    void timerCallback() override;

    // Copy current full log to clipboard
    void copyLogToClipboard();

    LLMidiAudioProcessor& audioProcessor;

    // UI elements
    juce::TextButton loadButton{ "Load Model..." };
    juce::TextButton smokeButton{ "Run Smoke Test" };
    juce::TextButton copyButton{ "Copy Log" };
    juce::TextButton genButton{ "Generate Pattern" };
    // Multiline scrollable log output
    juce::TextEditor logEditor;

    // Async file chooser for model
    std::unique_ptr<juce::FileChooser> modelChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessorEditor)
};
