#include "PluginProcessor.h"
#include "PluginEditor.h"

LLMidiAudioProcessorEditor::LLMidiAudioProcessorEditor(LLMidiAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(460, 160);

    addAndMakeVisible(loadButton);
    addAndMakeVisible(smokeButton);
    addAndMakeVisible(statusLabel);

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::FontOptions(14.0f));
    statusLabel.setText("No model loaded.", juce::dontSendNotification);

    loadButton.onClick = [this]()
        {
            modelChooser = std::make_unique<juce::FileChooser>("Select a GGUF model",
                juce::File(),
                "*.gguf");
            modelChooser->launchAsync(juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser)
                {
                    auto file = chooser.getResult();
                    // release the chooser now that the dialog has closed
                    modelChooser.reset();

                    if (file.existsAsFile())
                    {
                        audioProcessor.requestLoadModelFromFile(file);
                        statusLabel.setText("Loading model...", juce::dontSendNotification);
                    }
                });
        };

    smokeButton.onClick = [this]()
        {
            audioProcessor.requestLlmSmokeTest();
            statusLabel.setText("Running smoke test...", juce::dontSendNotification);
        };

    startTimerHz(10); // poll status 10 Hz
}

LLMidiAudioProcessorEditor::~LLMidiAudioProcessorEditor() {}

void LLMidiAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(16.0f));
    g.drawFittedText("LLMidi - LLM smoke test", getLocalBounds().removeFromTop(24),
        juce::Justification::centred, 1);
}

void LLMidiAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced(10);

    auto top = r.removeFromTop(40);
    loadButton.setBounds(top.removeFromLeft(160).reduced(0, 6));
    smokeButton.setBounds(top.removeFromLeft(140).reduced(10, 6));

    r.removeFromTop(4);
    statusLabel.setBounds(r.removeFromTop(80));
}

void LLMidiAudioProcessorEditor::timerCallback()
{
    // Show generator status and last error if any
    juce::String s;

    s << (audioProcessor.isModelReady() ? "Model ready." : "Model not ready.");
    auto last = audioProcessor.getLlmStatus();
    if (last.isNotEmpty())
    {
        s << "  " << last;
    }

    statusLabel.setText(s, juce::dontSendNotification);
}
