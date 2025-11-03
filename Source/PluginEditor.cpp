#include "PluginEditor.h"
#include "PluginProcessor.h"

LLMidiAudioProcessorEditor::LLMidiAudioProcessorEditor(LLMidiAudioProcessor& p)
	: AudioProcessorEditor(&p)
	, audioProcessor(p)
{
	setSize(Ui::windowW, Ui::windowH);
	setupUi();
	startTimerHz(10);
}

LLMidiAudioProcessorEditor::~LLMidiAudioProcessorEditor()
{
}

void LLMidiAudioProcessorEditor::paint(juce::Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
	g.setFont(juce::FontOptions(16.0f));
	g.setColour(juce::Colours::white);

	auto header = getLocalBounds().removeFromTop(Ui::titleH);
	g.drawFittedText("LLMidi", header, juce::Justification::centred, 1);
}

void LLMidiAudioProcessorEditor::resized()
{
	auto r = getLocalBounds().reduced(Ui::pad);

	// leave space for title (paint() draws it)
	r.removeFromTop(Ui::titleH + Ui::rowGap);

	// Row 1: buttons
	{
		auto row = r.removeFromTop(Ui::buttonRowH);
		auto eachW = row.getWidth() / 3;
		loadButton.setBounds(row.removeFromLeft(eachW).reduced(2));
		copyButton.setBounds(row.removeFromLeft(eachW).reduced(2));
		genButton.setBounds(row.removeFromLeft(eachW).reduced(2));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 2: Seed
	{
		auto row = r.removeFromTop(Ui::seedRowH);
		seedLabel.setBounds(row.removeFromLeft(Ui::seedLabelW));
		seedEditor.setBounds(row.removeFromLeft(Ui::seedEditW));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 3: Prompt label
	{
		auto row = r.removeFromTop(Ui::promptLblH);
		promptLabel.setBounds(row.removeFromLeft(60));
	}

	// Row 4: Prompt editor
	{
		auto row = r.removeFromTop(Ui::promptH);
		promptEditor.setBounds(row.reduced(0, 2));
	}

	r.removeFromTop(Ui::rowGap + 4);

	// Remaining: log
	logEditor.setBounds(r);
}

void LLMidiAudioProcessorEditor::timerCallback()
{
	updateLogView();
}

void LLMidiAudioProcessorEditor::setupUi()
{
	setupButtons();
	setupEditors();

	juce::String intro;
	intro << "LLMidi\n"
		<< (audioProcessor.isModelReady() ? "Model ready.\n\n" : "No model loaded yet.\n\n");
	logEditor.setText(intro, juce::dontSendNotification);
	lastRenderedLog = logEditor.getText();
}

void LLMidiAudioProcessorEditor::setupButtons()
{
	addAndMakeVisible(loadButton);
	loadButton.onClick = [this] { onClickLoadModel(); };

	addAndMakeVisible(copyButton);
	copyButton.onClick = [this] { onClickCopyLog(); };

	addAndMakeVisible(genButton);
	genButton.onClick = [this] { onClickGenerate(); };
}

void LLMidiAudioProcessorEditor::setupEditors()
{
	// Log
	addAndMakeVisible(logEditor);
	logEditor.setMultiLine(true);
	logEditor.setReadOnly(true);
	logEditor.setScrollbarsShown(true);
	logEditor.setCaretVisible(false);
	logEditor.setPopupMenuEnabled(true);
	logEditor.setFont(juce::FontOptions(14.0f));
	logEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	logEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	logEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	logEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
	logEditor.setScrollToShowCursor(false);

	// Seed
	addAndMakeVisible(seedLabel);
	seedLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	seedLabel.setFont(juce::FontOptions(14.0f));

	addAndMakeVisible(seedEditor);
	seedEditor.setMultiLine(false);
	seedEditor.setInputRestrictions(16, "0123456789-"); // allow '-' so user can type -1
	seedEditor.setText("-1", juce::dontSendNotification);
	seedEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	seedEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	seedEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	seedEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
	seedEditor.setFont(juce::FontOptions(14.0f));

	// Prompt
	addAndMakeVisible(promptLabel);
	promptLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	promptLabel.setFont(juce::FontOptions(14.0f));

	addAndMakeVisible(promptEditor);
	promptEditor.setMultiLine(true);
	promptEditor.setReturnKeyStartsNewLine(true);
	promptEditor.setScrollbarsShown(true);
	promptEditor.setCaretVisible(true);
	promptEditor.setPopupMenuEnabled(true);
	promptEditor.setText(
		"Nostalgic pluck arpeggio in E minor, light syncopation, leave space.",
		juce::dontSendNotification);
	promptEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	promptEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	promptEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	promptEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
	promptEditor.setFont(juce::FontOptions(14.0f));
}

void LLMidiAudioProcessorEditor::onClickLoadModel()
{
	modelChooser = std::make_unique<juce::FileChooser>(
		"Select a GGUF model", juce::File(), "*.gguf");

	modelChooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this](const juce::FileChooser& chooser)
		{
			auto file = chooser.getResult();
			modelChooser.reset();

			if (!file.existsAsFile())
				return;

			audioProcessor.requestLoadModelFromFile(file);
			appendUiLogLine("[UI] Loading model: " + file.getFullPathName());
		});
}

void LLMidiAudioProcessorEditor::onClickCopyLog()
{
	juce::SystemClipboard::copyTextToClipboard(logEditor.getText());
	appendUiLogLine("[UI] Log copied to clipboard.");
}

void LLMidiAudioProcessorEditor::onClickGenerate()
{
	const std::string naturalPrompt = promptEditor.getText().toStdString();

	const int bars = 8;
	const int stepsPerBar = 4;
	const int defaultVel = 96;
	const int channel = 0;

	const int seedToUse = readSeedOrRandom();

	audioProcessor.requestLlmGeneratePattern(
		naturalPrompt, bars, stepsPerBar, defaultVel, channel, seedToUse);

	appendUiLogLine("[UI] Generate pattern requested with seed " + juce::String(seedToUse));

	// Reflect the actual seed used back into the box
	seedEditor.setText(juce::String(seedToUse), juce::dontSendNotification);
}

void LLMidiAudioProcessorEditor::updateLogView()
{
	juce::String statusTop = audioProcessor.isModelReady() ? "Model ready.\n" : "Model not ready.\n";
	juce::String bgLog = audioProcessor.getLlmLog();

	juce::String combined;
	combined << statusTop
		<< "\n--- Background log ---\n"
		<< bgLog
		<< "\n";

	if (combined == lastRenderedLog)
		return;

	const bool userAtEnd = (logEditor.getCaretPosition() >= logEditor.getTotalNumChars() - 1);

	logEditor.setText(combined, juce::dontSendNotification);

	if (userAtEnd)
	{
		logEditor.moveCaretToEnd();
		logEditor.scrollEditorToPositionCaret(0, logEditor.getCaretRectangle().getY());
	}

	lastRenderedLog = combined;
}

void LLMidiAudioProcessorEditor::appendUiLogLine(const juce::String& line)
{
	logEditor.moveCaretToEnd();
	logEditor.insertTextAtCaret(line + "\n");
	lastRenderedLog = logEditor.getText();
}

int LLMidiAudioProcessorEditor::readSeedOrRandom() const
{
	// If empty or -1 => choose a positive random seed (avoid 0)
	const juce::String text = seedEditor.getText().trim();
	if (text.isEmpty() || text == "-1")
	{
		int s = juce::Random::getSystemRandom().nextInt();
		if (s <= 0) s = std::abs(s) + 1;
		return s;
	}

	// Otherwise parse int (invalid becomes 0 -> we guard by making it positive)
	int s = text.getIntValue();
	if (s <= 0) s = std::abs(s) + 1;
	return s;
}
