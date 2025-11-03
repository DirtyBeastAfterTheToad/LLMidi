#pragma once
#include "SequenceModel.h"

// Validates consistency of a Sequence.
// Returns true if valid, false otherwise, and sets errorOut.
bool validate(const Sequence& seq, juce::String& errorOut);
