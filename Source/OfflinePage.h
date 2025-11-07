#pragma once
#include <JuceHeader.h>
#include "StatusDot.h"
#include "AnimatedProgressBar.h"

class OfflinePage : public juce::Component, private juce::Timer
{
public:
	OfflinePage();

	// Callbacks (wired by PluginEditor)
	std::function<void()> onLoadModel;
	std::function<void()> onGenerate;
	std::function<void()> onCopyLog;
	std::function<void()> onToggleLogs;
	std::function<void()> onStop;
	std::function<void()> onReroll;

	// Accessors for editor logic
	juce::TextEditor& getPromptEditor() { return promptEditor; }
	juce::TextEditor& getSeedEditor() { return seedEditor; }
	juce::TextEditor& getLogEditor() { return logEditor; }
	AnimatedProgressBar& getProgressBar() { return genProgressBar; }
	juce::Label& getStageLabel() { return genStageLabel; }
	juce::TextButton& getStopButton() { return stopButton; }
	StatusDot& getModelDot() { return modelDot; }
	juce::Label& getModelNameLabel() { return modelName; }
	juce::Label& getModelLoadingLabel() { return modelLoading; }

	// Stateful API 
	void setLogsVisible(bool visible);
	bool areLogsVisible() const { return logsVisible; }

	void setProgressVisible(bool visible);
	void setStopVisible(bool visible);
	void setStageText(const juce::String& text, juce::Colour colour);

	// Glow helpers (guided flow)
	void setLoadGlow(bool on);
	void setGenerateGlow(bool on);
	void setPromptErrorGlow(bool on);

	void resized() override;
	void paint(juce::Graphics& g) override;
	void paintOverChildren(juce::Graphics& g) override;

private:
	void timerCallback() override;

	// Controls (moved from old editor)
	juce::Label title{ {}, "Offline LLM" };

	juce::TextButton loadButton{ "Load Model..." };
	juce::TextButton genButton{ "Generate Pattern" };
	juce::TextButton copyButton{ "Copy Log" };
	juce::TextButton logToggleButton{ "Show Logs" };
	juce::TextButton stopButton{ "Stop" };
	juce::TextButton rerollButton{ "Reroll" };

	juce::Label   modelLabel{ {}, "Model:" };
	juce::Label   modelName;
	StatusDot     modelDot;
	juce::Label   modelLoading{ {}, "Loading..." };

	juce::Label      seedLabel{ {}, "Seed:" };
	juce::TextEditor seedEditor;

	juce::Label      promptLabel{ {}, "Prompt:" };
	juce::TextEditor promptEditor;

	AnimatedProgressBar genProgressBar;
	juce::Label         genStageLabel;

	juce::TextEditor logEditor;

	// Local UI state
	bool logsVisible = false;

	// Glow state
	bool glowLoad = false;
	bool glowGenerate = false;
	bool glowPromptError = false;
	float phase = 0.0f;

	// Layout constants (mirroring your previous Ui struct)
	struct Ui {
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
		static constexpr int seedRerollW = 70;
	};
};
