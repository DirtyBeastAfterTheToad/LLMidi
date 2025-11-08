#include "SettingsPage.h"

SettingsPage::SettingsPage()
{
	addAndMakeVisible(title);
	title.setFont(juce::FontOptions(18.0f, juce::Font::bold));
	title.setColour(juce::Label::textColourId, juce::Colours::white);

	// Cache
	addAndMakeVisible(cacheGroup);
	addAndMakeVisible(cacheSizeLbl);
	addAndMakeVisible(cacheSizeValue);
	addAndMakeVisible(deleteCacheBtn);
	cacheSizeLbl.setColour(juce::Label::textColourId, juce::Colours::white);
	cacheSizeValue.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
	deleteCacheBtn.onClick = [this] { if (onDeleteCacheRequested) onDeleteCacheRequested(); };

	// Theme
	addAndMakeVisible(themeGroup);
	addAndMakeVisible(themeLbl);
	addAndMakeVisible(themeBox);
	themeLbl.setColour(juce::Label::textColourId, juce::Colours::white);
	themeBox.addItem("System / Auto", 1);
	themeBox.addItem("Classic", 2);
	themeBox.addItem("Dark", 3);
	themeBox.onChange = [this] { if (onThemeChanged) onThemeChanged(themeBox.getSelectedItemIndex()); };
	themeBox.setSelectedId(1, juce::dontSendNotification);
}

void SettingsPage::setCacheSizeText(const juce::String& s)
{
	cacheSizeValue.setText(s, juce::dontSendNotification);
}

void SettingsPage::resized()
{
	auto r = getLocalBounds().reduced(Ui::pad);
	title.setBounds(r.removeFromTop(Ui::titleH));
	r.removeFromTop(Ui::rowGap);

	// Cache group
	auto cacheArea = r.removeFromTop(Ui::groupH);
	cacheGroup.setBounds(cacheArea);
	cacheArea = cacheArea.reduced(10);
	{
		auto row = cacheArea.removeFromTop(Ui::ctrlH);
		cacheSizeLbl.setBounds(row.removeFromLeft(Ui::labelW));
		cacheSizeValue.setBounds(row);
		cacheArea.removeFromTop(Ui::rowGap);

		auto row2 = cacheArea.removeFromTop(Ui::ctrlH);
		deleteCacheBtn.setBounds(row2.removeFromLeft(140));
	}

	r.removeFromTop(Ui::rowGap * 2);

	// Theme group
	auto themeArea = r.removeFromTop(Ui::groupH);
	themeGroup.setBounds(themeArea);
	themeArea = themeArea.reduced(10);
	themeArea.removeFromTop(4);
	{
		auto row = themeArea.removeFromTop(Ui::ctrlH);
		themeLbl.setBounds(row.removeFromLeft(Ui::labelW));
		themeBox.setBounds(row.removeFromLeft(220));
	}
}

void SettingsPage::paint(juce::Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}
