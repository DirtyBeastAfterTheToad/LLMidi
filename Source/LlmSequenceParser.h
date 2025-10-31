#pragma once
#include <string>
#include <vector>

// SRP: Parse LLM JSON (bars/steps) into a structured form with
//      per-note velocities, rests ".", and sustains "-".
//
// This module intentionally has no JUCE or llama.cpp includes.
// You can reuse it in tests.


struct PlayedNote {
    int midi;      // 0..127
    int velocity;  // 1..127
};

struct StepEvent {
    enum class Kind { Rest, Sustain, Notes };
    Kind kind{};
    std::vector<PlayedNote> notes; // only if Kind::Notes
};

struct ParsedPhrase {
    // bars[barIndex][stepIndex]
    std::vector<std::vector<StepEvent>> bars;

    // Convenience
    int barsCount() const { return static_cast<int>(bars.size()); }
    int stepsPerBar() const { return bars.empty() ? 0 : static_cast<int>(bars.front().size()); }
};

// Parse a JSON string of the shape:
//
// [
//   ["A#3-70","-","D#4","."],
//   [["A3","C4-90"], "-", ".", "G3-64"],
//   ...
// ]
//
// - Step may be:
//   "."        = Rest
//   "-"        = Sustain
//   "A#3"      = single note (default velocity used later by adapter)
//   "A#3-72"   = single note with velocity
//   ["A#3","D4-90","F4"] = chord (each may have velocity)
//
// defaultVelocity is applied when a note omits velocity.
//
// Returns true on success; on failure, returns false and sets 'err'.
bool parseJsonBars(const std::string& json,
    int defaultVelocity,
    std::string& err,
    ParsedPhrase& out);

// Utility exposed for tests. Converts "A#3" or "A#3-72" -> (midi, velOrDefault)
// Throws std::invalid_argument on bad input. Velocity is clamped 1..127.
std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity);


