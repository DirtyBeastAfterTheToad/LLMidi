#pragma once
#include <string>
#include "LlmSequenceParser.h"

struct ParsedSummary {
	int bars = 0;
	int stepsPerBar = 0;
	int totalPlayableNotes = 0; // counts each note in chords
	int totalPlayableSteps = 0; // counts steps that are notes/chords
	int totalSustains = 0;
	int totalRests = 0;

	std::string shortPreview; // first few steps detok for logs
};

ParsedSummary summarize(const ParsedPhrase& p);

