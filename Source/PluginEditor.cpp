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

	r.removeFromTop(Ui::titleH + Ui::rowGap);

	// Row 1: main actions
	{
		auto row = r.removeFromTop(Ui::buttonRowH);
		auto eachW = row.getWidth() / 2;
		loadButton.setBounds(row.removeFromLeft(eachW).reduced(2));
		genButton.setBounds(row.removeFromLeft(eachW).reduced(2));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 2: model status (dot + name + optional "Loading...")
	{
		auto row = r.removeFromTop(Ui::modelRowH);
		modelLabel.setBounds(row.removeFromLeft(50));
		modelDot.setBounds(row.removeFromLeft(16));
		modelName.setBounds(row.removeFromLeft(juce::jmax(120, row.getWidth() / 2)));
		modelLoading.setBounds(row.removeFromLeft(100));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 3: Seed
	{
		auto row = r.removeFromTop(Ui::seedRowH);
		seedLabel.setBounds(row.removeFromLeft(Ui::seedLabelW));
		seedEditor.setBounds(row.removeFromLeft(Ui::seedEditW));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 4: Generation progress
	{
		auto row = r.removeFromTop(Ui::genRowH);
		genProgressBar.setBounds(row.removeFromLeft(row.getWidth() * 2 / 3).reduced(2));
		genStageLabel.setBounds(row);
	}

	r.removeFromTop(Ui::rowGap);

	// Row 5: Prompt label
	{
		auto row = r.removeFromTop(Ui::promptLblH);
		promptLabel.setBounds(row.removeFromLeft(60));
	}

	// Row 6: Prompt box
	{
		auto row = r.removeFromTop(Ui::promptH);
		promptEditor.setBounds(row.reduced(0, 2));
	}

	r.removeFromTop(Ui::rowGap + 4);

	// Bottom row: log toggle + copy (copy only visible with logs)
	auto bottomRow = r.removeFromBottom(Ui::logToggleRowH);
	{
		auto toggleW = 110;
		logToggleButton.setBounds(bottomRow.removeFromLeft(toggleW).reduced(2));
		auto copyW = 100;
		copyButton.setBounds(bottomRow.removeFromLeft(copyW).reduced(2));
	}

	// Center area: log
	if (logsVisible)
	{
		logEditor.setVisible(true);
		copyButton.setVisible(true);
		logToggleButton.setButtonText("Hide Logs");
		logEditor.setBounds(r);
	}
	else
	{
		logEditor.setVisible(false);
		copyButton.setVisible(false);
		logToggleButton.setButtonText("Show Logs");
	}
}



void LLMidiAudioProcessorEditor::timerCallback()
{
	updateModelUi();
	updateGenProgress();
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
	copyButton.setVisible(false);

	addAndMakeVisible(genButton);
	genButton.onClick = [this] { onClickGenerate(); };

	addAndMakeVisible(logToggleButton);
	logToggleButton.onClick = [this] { onClickToggleLogs(); };
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
	logEditor.setVisible(false);
	// Seed
	addAndMakeVisible(seedLabel);
	seedLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	seedLabel.setFont(juce::FontOptions(14.0f));

	addAndMakeVisible(seedEditor);
	seedEditor.setMultiLine(false);
	seedEditor.setInputRestrictions(16, "0123456789-"); // allow '-' so user can type -1
	const int restoredSeed = audioProcessor.getLastSeed();
	seedEditor.setText(restoredSeed < 0 ? "-1" : juce::String(restoredSeed),
		juce::dontSendNotification);
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

	promptEditor.setTextToShowWhenEmpty("Piano melody on E minor", juce::Colours::grey);
	promptEditor.setText(audioProcessor.getLastPrompt(), juce::dontSendNotification);

	promptEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	promptEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	promptEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	promptEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
	promptEditor.setFont(juce::FontOptions(14.0f));

	//  Model row 
	addAndMakeVisible(modelLabel);
	modelLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	modelLabel.setFont(juce::FontOptions(13.0f));

	addAndMakeVisible(modelDot);
	modelDot.setColour(juce::Colours::red);

	addAndMakeVisible(modelName);
	modelName.setText({}, juce::dontSendNotification);
	modelName.setColour(juce::Label::textColourId, juce::Colours::white);
	modelName.setFont(juce::FontOptions(13.0f));

	addAndMakeVisible(modelLoading);
	modelLoading.setColour(juce::Label::textColourId, juce::Colours::yellow);
	modelLoading.setFont(juce::FontOptions(13.0f));
	modelLoading.setVisible(false);

	//  Generation progress row 
	addAndMakeVisible(genProgressBar);
	genProgressBar.setVisible(false);
	genProgressBar.setProgress(0.0);

	addAndMakeVisible(genStageLabel);
	genStageLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	genStageLabel.setFont(juce::FontOptions(13.0f));
	genStageLabel.setText("", juce::dontSendNotification);
	genStageLabel.setVisible(false);

	const bool hasCandidate = audioProcessor.hasBurnCandidate();
	const bool logSaysReady = audioProcessor.getLlmLog().containsIgnoreCase("sequence ready");

	if (hasCandidate && logSaysReady)
	{
		genProgress = 1.0;
		genProgressBar.setProgress(1.0);
		genProgressBar.setActive(false);
		genProgressBar.setVisible(true);
		genStageLabel.setVisible(true);
		genStageLabel.setText("Ready to burn!", juce::dontSendNotification);
	}
	else
	{
		genProgress = 0.0;
		genProgressBar.setProgress(0.0);
		genProgressBar.setActive(false);
		genProgressBar.setVisible(false);
		genStageLabel.setVisible(false);
		genStageLabel.setText("", juce::dontSendNotification);
	}
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

			modelLoadingFlag = true;
			modelLoading.setVisible(true);
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
	audioProcessor.setLastPrompt(promptEditor.getText());

	const int bars = 8;
	const int stepsPerBar = 4;
	const int defaultVel = 96;
	const int channel = 0;

	const int seedToUse = readSeedOrRandom();
	audioProcessor.setLastSeed(seedToUse);
	audioProcessor.clearLlmLog();
	lastRenderedLog.clear();
	logEditor.clear();

	audioProcessor.requestLlmGeneratePattern(
		naturalPrompt, bars, stepsPerBar, defaultVel, channel, seedToUse);

	appendUiLogLine("[UI] Generate pattern requested with seed " + juce::String(seedToUse));

	seedEditor.setText(juce::String(seedToUse), juce::dontSendNotification);


	generationActive = true;
	genProgress = 0.0;
	genProgressBar.setProgress(0.0);
	genProgressBar.setActive(true);
	genProgressBar.setVisible(true);
	genStageLabel.setVisible(true);
	genStageLabel.setText("Ingesting prompt...", juce::dontSendNotification);

}

void LLMidiAudioProcessorEditor::onClickToggleLogs()
{
	logsVisible = !logsVisible;
	resized();
}
void LLMidiAudioProcessorEditor::updateGenProgress()
{
	const juce::String bgLog = audioProcessor.getLlmLog();

	if (!generationActive)
	{
		const bool midPrompt = bgLog.lastIndexOf("Prompt [") >= 0;
		const bool midGen = bgLog.lastIndexOf("Gen [") >= 0;
		const bool done = bgLog.containsIgnoreCase("sequence ready");

		if ((midPrompt || midGen) && !done)
		{
			generationActive = true;
			genProgressBar.setVisible(true);
			genStageLabel.setVisible(true);
			genProgressBar.setActive(true);
		}
		else
		{
			genProgressBar.setActive(false);
			return;
		}
	}

	if (bgLog.containsIgnoreCase("sequence ready"))
	{
		genProgress = 1.0;
		genProgressBar.setProgress(1.0);
		genProgressBar.setActive(false);
		genStageLabel.setText("Ready to burn!", juce::dontSendNotification);
		generationActive = false;
		return;
	}

	const int lastPrompt = bgLog.lastIndexOf("Prompt [");
	const int lastGen = bgLog.lastIndexOf("Gen [");
	const int useIdx = juce::jmax(lastPrompt, lastGen);

	if (useIdx < 0)
	{
		genProgressBar.setActive(true);
		return;
	}

	const auto tail = bgLog.substring(useIdx);
	const int  open = tail.lastIndexOfChar('(');
	const int  close = tail.lastIndexOfChar(')');

	if (open >= 0 && close > open)
	{
		const auto percentStr = tail.substring(open + 1, close)
			.removeCharacters("% ")
			.trim();
		const int pct = percentStr.getIntValue();
		genProgress = juce::jlimit(0, 100, pct) / 100.0;
		genProgressBar.setProgress(genProgress);
	}

	if (useIdx == lastPrompt)
		genStageLabel.setText("Ingesting prompt...", juce::dontSendNotification);
	else
		genStageLabel.setText("Generating the pattern...", juce::dontSendNotification);

	genProgressBar.setActive(true);
}

void LLMidiAudioProcessorEditor::updateModelUi()
{
	const bool ready = audioProcessor.isModelReady();
	if (ready)
	{
		modelDot.setColour(juce::Colours::limegreen);
		const auto path = audioProcessor.getLoadedModelPath();
		const juce::String name = juce::File(path).getFileName();
		modelName.setText(name, juce::dontSendNotification);
		modelLoadingFlag = false;
		modelLoading.setVisible(false);
	}
	else
	{
		modelDot.setColour(juce::Colours::red);
		if (modelLoadingFlag)
			modelLoading.setVisible(true);
	}
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
