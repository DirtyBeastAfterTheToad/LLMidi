#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "StatusDot.h"
#include "AnimatedProgressBar.h"

class LLMidiAudioProcessorEditor final
	: public juce::AudioProcessorEditor
	, private juce::Timer
{
public:
	explicit LLMidiAudioProcessorEditor(LLMidiAudioProcessor& processor);
	~LLMidiAudioProcessorEditor() override;

	void paint(juce::Graphics& g) override;
	void resized() override;

private:
	// Timer (poll background log + status)
	void timerCallback() override;

	// --- UI setup helpers ---
	void setupUi();
	void setupButtons();
	void setupEditors();

	// --- Event handlers ---
	void onClickLoadModel();
	void onClickCopyLog();
	void onClickGenerate();
	void onClickToggleLogs();
	// --- Utilities ---
	void updateLogView();
	void appendUiLogLine(const juce::String& line);
	int  readSeedOrRandom() const;
	void updateModelUi();
	void updateGenProgress();

	// --- Layout helpers ---
	struct Ui
	{
		static constexpr int windowW = 500;
		static constexpr int windowH = 500;
		static constexpr int pad = 10;
		static constexpr int rowGap = 6;
		static constexpr int titleH = 24;
		static constexpr int buttonRowH = 30;
		static constexpr int seedRowH = 24;
		static constexpr int promptLblH = 18;
		static constexpr int promptH = 60;
		static constexpr int logToggleRowH = 26;
		static constexpr int seedLabelW = 40;
		static constexpr int seedEditW = 100;
		static constexpr int modelRowH = 20;
		static constexpr int genRowH = 26;
	};

private:
	// Processor reference
	LLMidiAudioProcessor& audioProcessor;

	// Controls
	juce::TextButton loadButton{ "Load Model..." };
	juce::TextButton copyButton{ "Copy Log" };
	juce::TextButton genButton{ "Generate Pattern" };
	// Model row
	juce::Label modelLabel{ "modelLabel", "Model:" };
	juce::Label modelName;
	StatusDot modelDot;
	juce::Label modelLoading{ "modelLoading", "Loading..." };
	juce::TextButton logToggleButton{ "Show Logs" };
	// Progress row for generation
	double genProgress = 0.0;
	AnimatedProgressBar genProgressBar;
	juce::Label genStageLabel{ "genStage", "" };

	juce::Label      seedLabel{ "seedLabel",   "Seed:" };
	juce::TextEditor seedEditor;

	juce::Label      promptLabel{ "promptLabel", "Prompt:" };
	juce::TextEditor promptEditor;

	juce::TextEditor logEditor;
	bool logsVisible = false;
	bool modelLoadingFlag = false;
	bool generationActive = false;
	// Async file chooser for model
	std::unique_ptr<juce::FileChooser> modelChooser;

	// Small cache to avoid rewriting the log editor every tick
	juce::String lastRenderedLog;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessorEditor)
};
