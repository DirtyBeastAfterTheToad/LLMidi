#pragma once
#include <JuceHeader.h>

class SettingsPage : public juce::Component
{
public:
	SettingsPage();

	std::function<void()> onDeleteCacheRequested;
	std::function<void(int)> onThemeChanged;

	void setCacheSizeText(const juce::String& s);

	void resized() override;
	void paint(juce::Graphics& g) override;

private:
	// Cache section
	juce::Label      title{ {}, "Settings" };
	juce::GroupComponent cacheGroup{ "cacheGroup", "Cache" };
	juce::Label      cacheSizeLbl{ {}, "Cache size:" };
	juce::Label      cacheSizeValue;
	juce::TextButton deleteCacheBtn{ "Delete cache..." };

	// Theme section
	juce::GroupComponent themeGroup{ "themeGroup", "Theme" };
	juce::Label          themeLbl{ {}, "Appearance:" };
	juce::ComboBox       themeBox;

	struct Ui {
		static constexpr int pad = 10;
		static constexpr int rowGap = 8;
		static constexpr int labelW = 100;
		static constexpr int ctrlH = 26;
		static constexpr int groupH = 110;
		static constexpr int titleH = 28;
	};
};
