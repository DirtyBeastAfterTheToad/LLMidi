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
            const std::string naturalPrompt =
                "Nostalgic pluck arpeggio in E minor, light syncopation, leave space.";

            const int bars = 8;
            const int stepsPerBar = 8;
            const int defaultVel = 96;
            const int channel = 0;

            // Read seed from UI (fallback if empty)
            int seedToUse = seedEditor.getText().getIntValue();

            // if user typed -1 or left it blank, pick a random seed
            if (seedEditor.getText().isEmpty() || seedToUse == -1)
            {
                // JUCE has Random::getSystemRandom()
                seedToUse = juce::Random::getSystemRandom().nextInt(); // 32-bit signed
                if (seedToUse < 0)
                    seedToUse = -seedToUse; // keep it non-negative for simplicity
                if (seedToUse == 0)
                    seedToUse = 1;          // avoid 0 just in case
            }

            // send request with seedToUse
            audioProcessor.requestLlmGeneratePattern(
                naturalPrompt,
                bars,
                stepsPerBar,
                defaultVel,
                channel,
                seedToUse);

            logEditor.moveCaretToEnd();
            logEditor.insertTextAtCaret("[UI] Generate pattern requested with seed "
                + juce::String(seedToUse) + "\n");
        };

    // --- log editor setup ---
    addAndMakeVisible(logEditor);
    logEditor.setMultiLine(true);
    logEditor.setReadOnly(true);
    logEditor.setScrollbarsShown(true);
    logEditor.setCaretVisible(false);
    logEditor.setPopupMenuEnabled(true); // let user right-click/copy too
    logEditor.setFont(juce::FontOptions(14.0f));
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
    // --- seed UI ---
    addAndMakeVisible(seedLabel);
    seedLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    seedLabel.setFont(juce::FontOptions(14.0f));

    addAndMakeVisible(seedEditor);
    seedEditor.setMultiLine(false);
    seedEditor.setInputRestrictions(16, "0123456789"); // unsigned int for now
    seedEditor.setText("-1", juce::dontSendNotification); // default
    seedEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
    seedEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    seedEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
    seedEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
    seedEditor.setFont(juce::FontOptions(14.0f));

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

    // leave space for title
    r.removeFromTop(30);

    // first row: buttons
    auto buttonRow = r.removeFromTop(30);
    auto eachW = buttonRow.getWidth() / 4;

    loadButton.setBounds(buttonRow.removeFromLeft(eachW).reduced(2));
    smokeButton.setBounds(buttonRow.removeFromLeft(eachW).reduced(2));
    copyButton.setBounds(buttonRow.removeFromLeft(eachW).reduced(2));
    genButton.setBounds(buttonRow.removeFromLeft(eachW).reduced(2));

    r.removeFromTop(6);

    // second row: seed label + editor (fixed width)
    auto seedRow = r.removeFromTop(24);
    auto labelW = 40;
    auto editW = 80;

    seedLabel.setBounds(seedRow.removeFromLeft(labelW));
    seedEditor.setBounds(seedRow.removeFromLeft(editW));

    r.removeFromTop(10);

    // remaining space: log
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
