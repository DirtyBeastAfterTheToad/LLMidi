#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "OnlinePage.h"
#include "OfflinePage.h"
#include "SettingsPage.h"
#include "IconLibrary.h"
class LLMidiAudioProcessorEditor final
	: public juce::AudioProcessorEditor,
	private juce::Timer
{
public:
	explicit LLMidiAudioProcessorEditor(LLMidiAudioProcessor& processor);
	~LLMidiAudioProcessorEditor() override;

	void paint(juce::Graphics& g) override;
	void resized() override;
	void visibilityChanged() override;

private:
	// Timer (poll background log + status)
	void timerCallback() override;

	// --- Event handlers (wired to OfflinePage) ---
	void onClickLoadModel();
	void onClickCopyLog();
	void onClickGenerate();
	void onClickToggleLogs();
	void onClickStop();
	void onClickReroll();

	// --- Internal UI logic ---
	void syncUiFromProcessorOnce();
	void updateWindowSizeForLogs();
	void updateLogView();
	void appendUiLogLine(const juce::String& line);
	int  readSeedOrRandom() const;
	void updateModelUi();
	void updateGenProgress();
	void saveUiMemoryToProcessor();
	void loadUiMemoryFromProcessor();

	// --- Processor reference ---
	LLMidiAudioProcessor& audioProcessor;

	// --- Tabbed layout ---
	juce::TabbedComponent tabs{ juce::TabbedButtonBar::TabsAtTop };
	std::unique_ptr<OfflinePage> offlinePage;
	std::unique_ptr<OnlinePage>  onlinePage;
	// Settings tab + button
	std::unique_ptr<SettingsPage> settingsPage;
	int settingsTabIndex = -1;

	// Helpers (impl in .cpp)
	static juce::int64 getFolderSizeRecursive(const juce::File& dir);
	static juce::String prettyBytes(juce::int64 bytes);
	static juce::File cacheDirPath();
	void applyThemeFromId(int id);
	std::unique_ptr<juce::PropertiesFile> userProps;
	// --- State ---
	bool logsVisible = false;
	bool modelLoadingFlag = false;
	bool generationActive = false;
	bool canceledThisRun = false;
	juce::String lastRenderedLog;

	// Remember tab & visibility across wrapper settings dialog
	int  lastSelectedTabIndex = 0;
	bool wasVisible = false;

	std::unique_ptr<juce::FileChooser> modelChooser;

	// window sizing
	static constexpr int kWindowW = 600;
	static constexpr int kBaseH = 500; // tabbed layout height
	static constexpr int kLogsExtraH = 220;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LLMidiAudioProcessorEditor)
};
