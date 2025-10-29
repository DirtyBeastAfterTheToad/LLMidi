#pragma once
#include <vector>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <string>
#include <cassert>

#include <JuceHeader.h>

// -----------------------------------------------
// Core musical data model for LLMidi
// -----------------------------------------------

namespace llmidi
{
    // What a step represents
    enum class StepType : uint8_t
    {
        Rest,      // "."
        Sustain,   // "-"
        Note,      // "C#4"
        Chord      // ["C#4","F4","G#4"]
    };

    // A single MIDI note definition
    struct Note
    {
        int midi = 60;                 // 0..127
        uint8_t velocity = 100;        // 1..127
    };

    // One cell in the step grid (4 or 8 steps per bar)
    struct Step
    {
        StepType type = StepType::Rest;
        std::vector<Note> notes; // used for Note (size=1) or Chord (size>=2). Empty for Rest/Sustain.

        static Step makeRest() { return { StepType::Rest, {} }; }
        static Step makeSustain() { return { StepType::Sustain, {} }; }
        static Step makeNote(Note n) { return { StepType::Note, { n } }; }
        static Step makeChord(std::vector<Note> ns)
        {
            Step s; s.type = (ns.size() <= 1 ? StepType::Note : StepType::Chord);
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
        int bars = 8;                  // number of bars (e.g., 8)
        int stepsPerBar = 4;           // 4 or 8
        int bpm = 120;                 // tempo (for "dump-to-clip" scenarios)
        uint8_t midiChannel = 0;       // 0..15, used when emitting MIDI
        juce::String key = "A# minor"; // informational; not enforced yet

        std::vector<Bar> data;         // size == bars

        // Derived helper: step length in beats (4/stepsPerBar)
        double stepLengthBeats() const
        {
            jassert(stepsPerBar > 0);
            return 4.0 / static_cast<double>(stepsPerBar);
        }

        bool isEmpty() const { return data.empty(); }
    };

    // -----------------------------------------------
    // Name - MIDI conversion helpers
    // -----------------------------------------------

    // Parse "A#3" / "Bb3" / "C4" into MIDI 0..127. Returns std::nullopt on error.
    std::optional<int> parseNoteName(const juce::String& name);

    // Convert MIDI 0..127 to "C4" style name (sharps)
    juce::String midiToName(int midi);

    // -----------------------------------------------
    // Validation for safety before scheduling
    // -----------------------------------------------

    // Returns true if shape is consistent: bars match, each bar has stepsPerBar steps,
    // notes in range 0..127, velocities 1..127, and sustains never start a bar (optional rule).
    bool validate(const Sequence& seq, juce::String& errorOut);
}
