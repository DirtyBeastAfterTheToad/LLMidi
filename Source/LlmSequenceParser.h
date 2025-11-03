#pragma once
#include <string>
#include <vector>

struct PlayedNote {
	int midi;      // 0..127
	int velocity;  // 1..127
};

struct StepEvent {
	enum class Kind { Rest, Sustain, Notes };
	Kind kind{};
	std::vector<PlayedNote> notes;
};

struct ParsedPhrase {
	// bars[barIndex][stepIndex]
	std::vector<std::vector<StepEvent>> bars;

	int barsCount()   const { return (int)bars.size(); }
	int stepsPerBar() const { return bars.empty() ? 0 : (int)bars.front().size(); }
};

// Parse JSON of shape:
// [
//   ["A#3-70","-","D#4","."],
//   [["A3","C4-90"], "-", ".", "G3-64"],
//   ...
// ]
//
// defaultVelocity is applied when a note omits velocity.
// Returns true on success; on failure, returns false and sets 'err'.
bool parseJsonBars(const std::string& json,
	int defaultVelocity,
	std::string& err,
	ParsedPhrase& out);

// Converts "A#3" or "A#3-72" -> (midi, velOrDefault).
// Throws std::invalid_argument on bad token. Velocity is clamped 1..127.
std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity);
