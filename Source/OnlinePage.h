#pragma once
#include <JuceHeader.h>

class OnlinePage : public juce::Component, private juce::Timer
{
public:
	OnlinePage();

	std::function<void(const juce::String&)> onConvertToMidi;

	void resized() override;
	void paint(juce::Graphics& g) override;
	void paintOverChildren(juce::Graphics& g) override;

	void showStatus(const juce::String& text, juce::Colour color);

	void setCopyGlow(bool on);
	void setToMidiGlow(bool on);
	void setPromptErrorGlow(bool on);
	// State IO for persistence
	juce::String getPromptText()   const { return userPromptEditor.getText(); }
	juce::String getResponseText() const { return responseEditor.getText(); }
	juce::String getStatusText()   const { return statusLabel.getText(); }
	juce::Colour getStatusLabelColour() const { return statusLabel.findColour(juce::Label::textColourId); }

	void setPromptText(const juce::String& s) { userPromptEditor.setText(s, juce::dontSendNotification); }
	void setResponseText(const juce::String& s) { responseEditor.setText(s, juce::dontSendNotification); }
	void setStatusText(const juce::String& s, juce::Colour c)
	{
		statusLabel.setText(s, juce::dontSendNotification);
		statusLabel.setColour(juce::Label::textColourId, c);
	}
	std::function<void()> onResponseEdited;
	bool isCopyGlowOn() const { return glowCopy; }
	bool isToMidiGlowOn() const { return glowToMidi; }
	bool isPromptErrorGlowOn() const { return glowPromptError; }
	std::function<void()> onCopyPromptSucceeded;
	std::function<void()> onPromptEdited;

private:
	void copyPromptToClipboard();
	void timerCallback() override;

	// UI
	juce::Label      title{ {}, "Online LLM" };
	juce::Label      instructions;
	juce::Label      userPromptLabel{ {}, "What to generate:" };
	juce::TextEditor userPromptEditor;
	juce::TextButton copyPromptButton{ "Copy Prompt" };
	juce::TextButton toMidiButton{ "To MIDI" };
	juce::TextEditor responseEditor;
	juce::Label      statusLabel;

	// Glow state
	bool glowCopy = false;
	bool glowToMidi = false;
	bool glowPromptError = false;

	// Animation phase [0..1)
	float phase = 0.0f;

	struct Ui {
		static constexpr int pad = 10;
		static constexpr int titleH = 30;
		static constexpr int instructH = 76;
		static constexpr int rowGap = 10;
		static constexpr int promptLblW = 140;
		static constexpr int promptH = 60;
		static constexpr int btnH = 30;
	};
};
