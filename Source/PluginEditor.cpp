#include "PluginProcessor.h"
#include "PluginEditor.h"

LLMidiAudioProcessorEditor::LLMidiAudioProcessorEditor(LLMidiAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    // Window size a bit larger to make room for log + buttons
    setSize(500, 500);

    // --- load model button ---
    addAndMakeVisible(loadButton);
    loadButton.onClick = [this]()
        {
            modelChooser = std::make_unique<juce::FileChooser>(
                "Select a GGUF model",
                juce::File(),
                "*.gguf");

            modelChooser->launchAsync(juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser)
                {
                    auto file = chooser.getResult();
                    modelChooser.reset(); // release dialog

                    if (file.existsAsFile())
                    {
                        audioProcessor.requestLoadModelFromFile(file);

                        // small immediate feedback
                        logEditor.moveCaretToEnd();
                        logEditor.insertTextAtCaret("[UI] Loading model: " + file.getFullPathName() + "\n");
                    }
                });
        };

    // --- smoke test button ---
    addAndMakeVisible(smokeButton);
    smokeButton.onClick = [this]()
        {
            audioProcessor.requestLlmSmokeTest();

            logEditor.moveCaretToEnd();
            logEditor.insertTextAtCaret("[UI] Smoke test requested...\n");
        };

    // --- copy log button ---
    addAndMakeVisible(copyButton);
    copyButton.onClick = [this]()
        {
            copyLogToClipboard();
        };
    // --- generate pattern button ---
    addAndMakeVisible(genButton);
    genButton.onClick = [this]()
        {
            // Minimal starting prompt; you can replace with a TextEditor later
            const std::string naturalPrompt =
                "Nostalgic pluck arpeggio in E minor, light syncopation, leave space.";

            // For now, fixed grid; we’ll later infer from clip/note length
            const int bars = 8;
            const int stepsPerBar = 8;
            const int defaultVel = 96;
            const int channel = 0;

            audioProcessor.requestLlmGeneratePattern(naturalPrompt, bars, stepsPerBar, defaultVel, channel);

            logEditor.moveCaretToEnd();
            logEditor.insertTextAtCaret("[UI] Generate pattern requested...\n");
        };
    // --- log editor setup ---
    addAndMakeVisible(logEditor);
    logEditor.setMultiLine(true);
    logEditor.setReadOnly(true);
    logEditor.setScrollbarsShown(true);
    logEditor.setCaretVisible(false);
    logEditor.setPopupMenuEnabled(true); // let user right-click/copy too
#if JUCE_MAJOR_VERSION >= 8
    logEditor.setFont(juce::FontOptions(14.0f));
#else
    logEditor.setFont(juce::Font(14.0f));
#endif
    logEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
    logEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    logEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
    logEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
    logEditor.setScrollToShowCursor(false);

    // seed the log box
    {
        juce::String intro;
        intro << "LLMidi - LLM smoke test panel\n"
            << "No model loaded yet.\n\n";
        logEditor.setText(intro, juce::dontSendNotification);
    }

    // poll the processor / background thread ~10 Hz
    startTimerHz(10);
}

LLMidiAudioProcessorEditor::~LLMidiAudioProcessorEditor()
{
    // timer auto-stops in destructor
}

void LLMidiAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

#if JUCE_MAJOR_VERSION >= 8
    g.setFont(juce::FontOptions(16.0f));
#else
    g.setFont(juce::Font(16.0f));
#endif
    g.setColour(juce::Colours::white);

    auto headerArea = getLocalBounds().removeFromTop(24);
    g.drawFittedText("LLMidi - LLM smoke test", headerArea,
        juce::Justification::centred, 1);
}

void LLMidiAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced(10);

    // header already painted in paint(), so start layout below it
    r.removeFromTop(30); // spacing under title

    auto buttonRow = r.removeFromTop(30);

    // lay out buttons horizontally:
    // [Load Model...] [Run Smoke Test] [Copy Log]
    auto b = buttonRow;
    auto eachW = b.getWidth() / 4;

    loadButton.setBounds(b.removeFromLeft(eachW).reduced(2));
    smokeButton.setBounds(b.removeFromLeft(eachW).reduced(2));
    copyButton.setBounds(b.removeFromLeft(eachW).reduced(2));
    genButton.setBounds(b.removeFromLeft(eachW).reduced(2));
    r.removeFromTop(10);

    // remaining area = log editor
    logEditor.setBounds(r);
}

void LLMidiAudioProcessorEditor::timerCallback()
{
    // Build status text from processor
    // We'll show top status first, then full rolling log from bg thread
    juce::String statusTop;
    statusTop << (audioProcessor.isModelReady() ? "Model ready.\n" : "Model not ready.\n");

    // Add backgroundGenerator's rolling log
    // This is already limited to ~10 lines in BackgroundGenerator,
    // but now that we want full scroll, let's just append every poll.
    // We'll keep the editor text growing.
    juce::String bgLog = audioProcessor.getLlmLog(); // this calls getLogText()

    // We'll maintain a cached tail to avoid spamming duplicates.
    // Easiest approach: just replace whole content each tick, and keep caret at end if user isn't actively scrolling.
    // For now let's just replace; that's simpler and guarantees we see everything.

    juce::String combined;
    combined << statusTop
        << "\n--- Background log ---\n"
        << bgLog
        << "\n";

    // only update if changed to avoid resetting scroll all the time
    if (logEditor.getText() != combined)
    {
        const bool userIsAtEnd = (logEditor.getCaretPosition() >= logEditor.getTotalNumChars() - 1);

        logEditor.setText(combined, juce::dontSendNotification);

        if (userIsAtEnd)
        {
            // scroll to bottom politely
            logEditor.moveCaretToEnd();
            logEditor.scrollEditorToPositionCaret(0, logEditor.getCaretRectangle().getY());
        }
    }
}

void LLMidiAudioProcessorEditor::copyLogToClipboard()
{
    // Copy whatever is currently in the TextEditor
    juce::SystemClipboard::copyTextToClipboard(logEditor.getText());

    // Give tiny UI feedback (non-blocking, in log box itself)
    logEditor.moveCaretToEnd();
    logEditor.insertTextAtCaret("[UI] Log copied to clipboard.\n");
}
