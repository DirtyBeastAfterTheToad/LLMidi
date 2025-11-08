#pragma once
#include <JuceHeader.h>

// Central place for all vector icons used in the UI.
class IconLibrary
{
public:
	static std::unique_ptr<juce::Drawable> makeGearDrawable(juce::Colour c);
};
