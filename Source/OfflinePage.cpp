#include "OfflinePage.h"

OfflinePage::OfflinePage()
{
	addAndMakeVisible(title);
	title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
	title.setColour(juce::Label::textColourId, juce::Colours::white);

	// Buttons
	addAndMakeVisible(loadButton);
	loadButton.onClick = [this] { if (onLoadModel) onLoadModel(); };

	addAndMakeVisible(genButton);
	genButton.onClick = [this] { if (onGenerate) onGenerate(); };

	addAndMakeVisible(copyButton);
	copyButton.onClick = [this] { if (onCopyLog) onCopyLog(); };
	copyButton.setVisible(false);

	addAndMakeVisible(logToggleButton);
	logToggleButton.onClick = [this] { if (onToggleLogs) onToggleLogs(); };

	addAndMakeVisible(stopButton);
	stopButton.onClick = [this] { if (onStop) onStop(); };
	stopButton.setVisible(false);

	addAndMakeVisible(rerollButton);
	rerollButton.setTooltip("Set seed to -1 (random each run)");
	rerollButton.onClick = [this] { if (onReroll) onReroll(); };

	// Labels / Editors
	addAndMakeVisible(modelLabel);
	addAndMakeVisible(modelDot);
	modelDot.setColour(juce::Colours::red);
	addAndMakeVisible(modelName);
	addAndMakeVisible(modelLoading);
	modelLoading.setColour(juce::Label::textColourId, juce::Colours::yellow);
	modelLoading.setVisible(false);

	addAndMakeVisible(seedLabel);
	addAndMakeVisible(seedEditor);
	seedEditor.setMultiLine(false);
	seedEditor.setInputRestrictions(16, "0123456789-");
	seedEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	seedEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	seedEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	seedEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));

	addAndMakeVisible(promptLabel);
	addAndMakeVisible(promptEditor);
	promptEditor.setMultiLine(true);
	promptEditor.setReturnKeyStartsNewLine(true);
	promptEditor.setScrollbarsShown(true);
	promptEditor.setCaretVisible(true);
	promptEditor.setPopupMenuEnabled(true);
	promptEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	promptEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	promptEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	promptEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::yellow.withAlpha(0.4f));
	promptEditor.onTextChange = [this]
		{
			if (!promptEditor.getText().trim().isEmpty())
				setPromptErrorGlow(false);
		};

	// Progress
	addAndMakeVisible(genProgressBar);
	genProgressBar.setVisible(false);
	genProgressBar.setProgress(0.0);

	addAndMakeVisible(genStageLabel);
	genStageLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	genStageLabel.setMinimumHorizontalScale(0.8f);
	genStageLabel.setText("", juce::dontSendNotification);
	genStageLabel.setVisible(false);

	// Log editor
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

	seedEditor.setText("-1", juce::dontSendNotification);

	startTimerHz(30);
}

void OfflinePage::setLogsVisible(bool visible)
{
	logsVisible = visible;
	logEditor.setVisible(visible);
	copyButton.setVisible(visible);
	logToggleButton.setButtonText(visible ? "Hide Logs" : "Show Logs");
	resized();
}

void OfflinePage::setProgressVisible(bool visible)
{
	genProgressBar.setVisible(visible);
	genStageLabel.setVisible(visible);
}

void OfflinePage::setStopVisible(bool visible)
{
	stopButton.setVisible(visible);
	stopButton.setEnabled(visible);
}

void OfflinePage::setStageText(const juce::String& text, juce::Colour colour)
{
	genStageLabel.setText(text, juce::dontSendNotification);
	genStageLabel.setColour(juce::Label::textColourId, colour);
}

void OfflinePage::setLoadGlow(bool on) { glowLoad = on;       if (on) startTimerHz(30); repaint(); }
void OfflinePage::setGenerateGlow(bool on) { glowGenerate = on;   if (on) startTimerHz(30); repaint(); }
void OfflinePage::setPromptErrorGlow(bool on) { glowPromptError = on; if (on) startTimerHz(30); repaint(); }

void OfflinePage::resized()
{
	auto r = getLocalBounds().reduced(Ui::pad);

	auto header = r.removeFromTop(30);
	title.setBounds(header);
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
		row.removeFromLeft(6);
		rerollButton.setBounds(row.removeFromLeft(Ui::seedRerollW));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 4: Generation progress
	{
		auto row = r.removeFromTop(Ui::genRowH);

		constexpr int stopW = 70;
		constexpr int labelW = 180;
		auto stopArea = row.removeFromRight(stopW);
		auto labelArea = row.removeFromRight(labelW);
		auto barArea = row;

		genProgressBar.setBounds(barArea.reduced(2));
		genStageLabel.setBounds(labelArea.reduced(2));
		stopButton.setBounds(stopArea.reduced(2));
	}

	r.removeFromTop(Ui::rowGap);

	// Row 5: Prompt label
	{
		auto row = r.removeFromTop(Ui::promptLblH);
		promptLabel.setBounds(row.removeFromLeft(60));
	}

	// Row 6: Prompt box (height matches Online page)
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

	if (logsVisible)
		logEditor.setBounds(r);
}

void OfflinePage::paint(juce::Graphics& g)
{
	g.fillAll(juce::Colours::black.withBrightness(0.12f));
}

void OfflinePage::paintOverChildren(juce::Graphics& g)
{
	auto drawGlow = [&](juce::Rectangle<int> r, juce::Colour colour)
		{
			auto rr = r.toFloat().expanded(4.0f);
			float t = (std::sin(phase * juce::MathConstants<float>::twoPi) * 0.5f + 0.5f);
			float alpha = juce::jmap(t, 0.35f, 0.9f);
			g.setColour(colour.withAlpha(alpha));
			g.drawRoundedRectangle(rr, 6.0f, 3.0f);

			g.setColour(colour.withAlpha(alpha * 0.6f));
			g.drawRoundedRectangle(rr.expanded(3.0f), 8.0f, 2.0f);
		};

	if (glowLoad)        drawGlow(loadButton.getBounds(), juce::Colours::limegreen);
	if (glowGenerate)    drawGlow(genButton.getBounds(), juce::Colours::limegreen);
	if (glowPromptError) drawGlow(promptEditor.getBounds(), juce::Colours::orangered);
}

void OfflinePage::timerCallback()
{
	if (!(glowLoad || glowGenerate || glowPromptError))
	{
		stopTimer();
		return;
	}
	phase += 0.02f;
	if (phase >= 1.0f) phase -= 1.0f;
	repaint();
}
