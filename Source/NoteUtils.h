#pragma once
#include <optional>
#include <JuceHeader.h>

// Convert textual note name ("A#3", "Bb2", "C4") into MIDI 0..127
std::optional<int> parseNoteName(const juce::String& name);

// Convert MIDI note (0..127) to "C4"-style name (sharps)
juce::String midiToName(int midi);
