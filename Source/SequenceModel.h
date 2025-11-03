#pragma once
#include <vector>
#include <cstdint>
#include <optional>
#include <string>
#include <JuceHeader.h>

// What a step represents
enum class StepType : uint8_t
{
	Rest,      // "."
	Sustain,   // "-"
	Note,      // "C#4"
	Chord      // ["C#4", "F4", "G#4"]
};

// A single MIDI note definition
struct Note
{
	int midi = 60;           // 0..127
	uint8_t velocity = 100;  // 1..127
};

// One cell in the step grid
struct Step
{
	StepType type = StepType::Rest;
	std::vector<Note> notes;

	static Step makeRest() { return { StepType::Rest, {} }; }
	static Step makeSustain() { return { StepType::Sustain, {} }; }
	static Step makeNote(Note n) { return { StepType::Note, { n } }; }
	static Step makeChord(std::vector<Note> ns)
	{
		Step s;
		s.type = (ns.size() <= 1 ? StepType::Note : StepType::Chord);
		s.notes = std::move(ns);
		return s;
	}

	bool isRest()    const { return type == StepType::Rest; }
	bool isSustain() const { return type == StepType::Sustain; }
	bool isNote()    const { return type == StepType::Note; }
	bool isChord()   const { return type == StepType::Chord; }
};

// One musical bar
struct Bar
{
	std::vector<Step> steps;
};

// Whole sequence (what the LLM will generate and we will schedule)
struct Sequence
{
	int bars = 8;
	int stepsPerBar = 4;
	int bpm = 120;
	uint8_t midiChannel = 0;
	juce::String key = "A# minor";

	std::vector<Bar> data;

	double stepLengthBeats() const
	{
		jassert(stepsPerBar > 0);
		return 4.0 / static_cast<double>(stepsPerBar);
	}

	bool isEmpty() const { return data.empty(); }
};
