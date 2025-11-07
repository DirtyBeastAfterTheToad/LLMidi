#pragma once
#include <string>
#include <vector>

struct PlayedNote {
	int midi;      // 0..127
	int velocity;  // 1..127
};
inline bool operator==(const PlayedNote& a, const PlayedNote& b)
{
	return a.midi == b.midi && a.velocity == b.velocity;
}

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

// Parse compact event JSON of shape:
// {"b":8,"s":4,"e":[ [0,"C4",16,90], [4,["F3","A3","C4-100"],4,85] ]}
//
// - b: number of bars
// - s: steps per bar
// - e: list of events
//     event = [startStep, noteOrNotes, durationSteps, velocity]
//     noteOrNotes = "C4" or ["C4","E4-80","G4"]
//
// Velocity precedence inside a chord:
// - If a note has a "-NN" suffix, that per-note velocity overrides.
// - Otherwise, the event velocity applies to that note.
//
// defaultVelocity is used when no velocity is provided anywhere.
// Returns true on success; on failure, returns false and sets 'err'.
bool parseEventJson(const std::string& json,
	int defaultVelocity,
	std::string& err,
	ParsedPhrase& out);

// Converts "A#3" or "A#3-72" -> (midi, velOrDefault).
// Throws std::invalid_argument on bad token. Velocity is clamped 1..127.
std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity);