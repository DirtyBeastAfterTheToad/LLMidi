#include "OnlinePage.h"

OnlinePage::OnlinePage()
{
	addAndMakeVisible(title);
	title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
	title.setColour(juce::Label::textColourId, juce::Colours::white);

	addAndMakeVisible(instructions);
	instructions.setJustificationType(juce::Justification::topLeft);
	instructions.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
	instructions.setText(
		"1) Enter what you want to generate.\n"
		"2) Click 'Copy Prompt' and paste it in your favorite chatbot (ChatGPT, Claude, Gemini, etc.).\n"
		"3) Copy the chatbot's JSON reply and paste it below.\n"
		"4) Click 'To MIDI' to import the pattern into your DAW.",
		juce::dontSendNotification);

	addAndMakeVisible(userPromptLabel);
	userPromptLabel.setColour(juce::Label::textColourId, juce::Colours::white);

	addAndMakeVisible(userPromptEditor);
	userPromptEditor.setMultiLine(true);
	userPromptEditor.setReturnKeyStartsNewLine(true);
	userPromptEditor.setText("Piano melody on E minor", juce::dontSendNotification);
	userPromptEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	userPromptEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	userPromptEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	userPromptEditor.setTextToShowWhenEmpty(
		"Describe the style (e.g., \"Piano melody on e minor\")", juce::Colours::grey);
	userPromptEditor.onTextChange = [this]
		{
			if (!userPromptEditor.getText().trim().isEmpty())
				setPromptErrorGlow(false);
			setCopyGlow(true);
			setToMidiGlow(false);
			if (onPromptEdited) onPromptEdited();
		};

	addAndMakeVisible(copyPromptButton);
	copyPromptButton.onClick = [this] { copyPromptToClipboard(); };

	addAndMakeVisible(toMidiButton);
	toMidiButton.onClick = [this]
		{
			setToMidiGlow(false);
			if (onConvertToMidi)
				onConvertToMidi(responseEditor.getText());
		};

	addAndMakeVisible(responseEditor);
	responseEditor.setMultiLine(true);
	responseEditor.setScrollbarsShown(true);
	responseEditor.setCaretVisible(true);
	responseEditor.setPopupMenuEnabled(true);
	responseEditor.setTextToShowWhenEmpty(
		"Paste chatbot's JSON response here...", juce::Colours::grey);
	responseEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
	responseEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
	responseEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::darkgrey);
	responseEditor.onTextChange = [this]
		{
			if (onResponseEdited) onResponseEdited();
		};

	addAndMakeVisible(statusLabel);
	statusLabel.setFont(juce::FontOptions(13.0f));
	statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
	statusLabel.setJustificationType(juce::Justification::centredLeft);

	startTimerHz(30);
}

void OnlinePage::resized()
{
	auto area = getLocalBounds().reduced(Ui::pad);
	title.setBounds(area.removeFromTop(Ui::titleH));
	instructions.setBounds(area.removeFromTop(Ui::instructH));

	{
		auto row = area.removeFromTop(Ui::promptH);
		auto lbl = row.removeFromLeft(Ui::promptLblW);
		userPromptLabel.setBounds(lbl);
		userPromptEditor.setBounds(row.reduced(0, 2));
	}

	area.removeFromTop(Ui::rowGap);

	auto buttonRow = area.removeFromTop(Ui::btnH);
	copyPromptButton.setBounds(buttonRow.removeFromLeft(120));
	buttonRow.removeFromLeft(10);
	toMidiButton.setBounds(buttonRow.removeFromLeft(120));

	area.removeFromTop(10);
	responseEditor.setBounds(area.removeFromTop(area.getHeight() - 30));
	statusLabel.setBounds(area);
}

void OnlinePage::paint(juce::Graphics& g)
{
	g.fillAll(juce::Colours::black.withBrightness(0.12f));
}

void OnlinePage::paintOverChildren(juce::Graphics& g)
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

	if (glowCopy)      drawGlow(copyPromptButton.getBounds(), juce::Colours::limegreen);
	if (glowToMidi)    drawGlow(toMidiButton.getBounds(), juce::Colours::limegreen);
	if (glowPromptError) drawGlow(userPromptEditor.getBounds(), juce::Colours::orangered);
}

void OnlinePage::copyPromptToClipboard()
{
	const juce::String userNL = userPromptEditor.getText().trim();

	if (userNL.isEmpty())
	{
		showStatus("Please input the style you want.", juce::Colours::orangered);
		setPromptErrorGlow(true);
		setCopyGlow(true);
		userPromptEditor.grabKeyboardFocus();
		return;
	}

	juce::String prompt;
	prompt
		<< "You are a MIDI pattern generator.\n"
		<< "Task: Generate an 8-bar musical pattern for the request: \"" << userNL << "\".\n"
		<< "\n"
		<< "Output REQUIREMENTS (must follow EXACTLY):\n"
		<< "* Output ONLY a single JSON object. No prose, no markdown, no code fences, no prefix/suffix.\n"
		<< "* JSON schema:\n"
		<< "{\n"
		<< "  \"b\": 8,               \n"
		<< "  \"s\": 4,               // integer: steps per bar (4 or 8 supported; use 4 unless necessary)\n"
		<< "  \"e\": [                // array of events\n"
		<< "    [startStep, noteOrNotes, durationSteps, velocity],\n"
		<< "    ...\n"
		<< "  ]\n"
		<< "}\n"
		<< "\n"
		<< "Event format details:\n"
		<< "* startStep: integer >= 0. The timeline is in steps, totalSteps = b * s.\n"
		<< "* durationSteps: integer >= 1.\n"
		<< "* velocity: integer 1..127 (typical 60..110).\n"
		<< "* noteOrNotes: either a string note token (e.g., \"C4\", \"Bb3-90\", \"F#5\")\n"
		<< "  or an array of note tokens for chords (e.g., [\"C4\",\"E4-88\",\"G4\"]).\n"
		<< "  A note token may include an optional per-note velocity suffix \"-NN\" (1..127),\n"
		<< "  which overrides the event velocity for that note.\n"
		<< "\n"
		<< "Rhythm & durations (important):\n"
		<< "* Do NOT quantize everything to durationSteps=1.\n"
		<< "* Use a VARIETY of durationSteps values (e.g., 1,2,3,4...).\n"
		<< "* At least 50% of events MUST have durationSteps >= 2 (sustained notes/chords).\n"
		<< "* Include some longer notes/chords that span across steps or even across bar boundaries when musical.\n"
		<< "* If the style truly calls for staccato, you may use more 1-step notes, but still keep >=30% with durationSteps >= 2.\n"
		<< "* Quick cheat sheet for s=4: 1=quarter note, 2=half note, 3=dotted half, 4=whole note.\n"
		<< "\n"
		<< "Mini example (for illustration only — your output must be a single JSON object without comments):\n"
		<< "{\n"
		<< "  \"b\": 8,\n"
		<< "  \"s\": 4,\n"
		<< "  \"e\": [\n"
		<< "    [0,  \"C4\",        2, 96],\n"
		<< "    [2,  [\"E4\",\"G4\"],3, 92],\n"
		<< "    [8,  \"D4-88\",     4, 88],\n"
		<< "    [16, [\"E4\",\"G4\"],2, 96],\n"
		<< "    [22, \"B3\",        1, 90]\n"
		<< "  ]\n"
		<< "}\n"
		<< "Constraints and guidance:\n"
		<< "* Use exactly b=8 bars. Use s=4 unless the material logically needs s=8.\n"
		<< "* Keep events within the total range (0 .. b*s-1). Ignore overlapping concerns; overlap is allowed.\n"
		<< "* Sort events in ascending startStep. Avoid zero/negative durations.\n"
		<< "* Style should reflect the request (key, register, density, repetition/variation).\n"
		<< "* Use varied durationSteps; at least half of the events must sustain (durationSteps >= 2).\n"
		<< "* Avoid making all events durationSteps=1 unless the style explicitly demands strict staccato.\n"
		<< "\n"
		<< "Important: Return ONLY the JSON object. Do not add backticks, markdown, or explanatory text.\n";

	juce::SystemClipboard::copyTextToClipboard(prompt);
	showStatus("Prompt copied. Paste into your chatbot.", juce::Colours::limegreen);

	setCopyGlow(false);
	setToMidiGlow(true);
	setPromptErrorGlow(false);

	if (onCopyPromptSucceeded) onCopyPromptSucceeded();

}

void OnlinePage::showStatus(const juce::String& text, juce::Colour color)
{
	statusLabel.setText(text, juce::dontSendNotification);
	statusLabel.setColour(juce::Label::textColourId, color);
}

void OnlinePage::setCopyGlow(bool on) { glowCopy = on;        if (on) startTimerHz(30); repaint(); }
void OnlinePage::setToMidiGlow(bool on) { glowToMidi = on;      if (on) startTimerHz(30); repaint(); }
void OnlinePage::setPromptErrorGlow(bool on) { glowPromptError = on; if (on) startTimerHz(30); repaint(); }

void OnlinePage::timerCallback()
{
	if (!(glowCopy || glowToMidi || glowPromptError))
	{
		stopTimer();
		return;
	}
	phase += 0.02f;
	if (phase >= 1.0f) phase -= 1.0f;
	repaint();
}
