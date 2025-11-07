#include "LlmSequenceParser.h"
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <JuceHeader.h>
#include "NoteUtils.h"

namespace {
	// Matches:  A#3  or  A#3-72
	// group1: pitch name (A..G with optional #/b)
	// group2: octave (may be negative)
	// group3: optional velocity (1..3 digits; clamped later)
	const std::regex kNoteRegex(R"(^([A-G][#b]?)(-?\d+)(?:-([0-9]{1,3}))?$)");

	inline int clampVel(int v) { return std::max(1, std::min(127, v)); }

	// Convert chord-like tokens (Cm7, Cmaj7, Cmin9, Bb7-90, etc.)
// into a playable single-note token by keeping the root
// and replacing the rest with an octave number.
// - "Cm7" -> "C7"
// - "Bbmaj7-90" -> "Bb7-90"
// - "F#sus2" -> "F#7"
// - keeps per-note velocity suffix if present
// - If the token already matches kNoteRegex, returns unchanged
	static std::string normalizeChordishToken(const std::string& in, int defaultOctave)
	{
		// already a clean note token? keep it
		if (std::regex_match(in, kNoteRegex))
			return in;

		// split optional velocity suffix "-NNN" at end
		std::string core = in;
		std::string velSuffix;
		{
			std::smatch mm;
			static const std::regex velTail(R"(^(.*?)-([0-9]{1,3})$)");
			if (std::regex_match(core, mm, velTail)) {
				core = mm[1].str();
				velSuffix = mm[2].str();
			}
		}

		// Handle minor and major chords and normalize
		if (core.find("min") != std::string::npos || core.find("m") != std::string::npos) {
			// Handle minor: for example, "Emin" -> "E4"
			core = core.substr(0, core.find_first_of("m")) + "4";  // or whatever octave makes sense
		}
		else if (core.find("maj") != std::string::npos || core.find("M") != std::string::npos) {
			// Handle major: for example, "Cmaj" -> "C4"
			core = core.substr(0, core.find_first_of("maj")) + "4";  // or whatever octave makes sense
		}

		// find root (A–G) + optional accidental
		size_t i = 0;
		while (i < core.size() && std::isspace((unsigned char)core[i])) ++i;
		if (i >= core.size()) return in;

		char L = (char)std::toupper((unsigned char)core[i]);
		if (L < 'A' || L > 'G') return in;
		std::string root;
		root.push_back(L);
		++i;
		if (i < core.size() && (core[i] == '#' || core[i] == 'b')) {
			root.push_back(core[i]);
			++i;
		}

		// Determine octave based on chord-like names or use default
		int octave = defaultOctave;
		std::string out = root + std::to_string(octave);
		if (!velSuffix.empty()) {
			out += "-" + velSuffix;
		}

		return out;
	}


	// Parse a single note token and produce a PlayedNote.
	// If the token has a "-NN" suffix, that velocity overrides;
	// otherwise 'eventVel' is used. defaultVelocity is only used to clamp/fallback
	// when a malformed 0 is encountered (shouldn't happen with the regex).
	static bool parseNoteIntoPlayed(const juce::String& token,
		int eventVel,
		int defaultVelocity,
		PlayedNote& out,
		std::string& err)
	{
		// Normalize chord-ish tokens to a note token first
		const std::string raw = token.toStdString();
		const std::string norm = normalizeChordishToken(raw, /*defaultOctave*/ 4);

		// Now apply the standard note regex to the normalized token
		std::smatch m;
		if (!std::regex_match(norm, m, kNoteRegex)) {
			err = "Bad note token: " + raw; // report the original token for clarity
			return false;
		}

		const std::string name = m[1].str();
		const int octave = std::stoi(m[2].str());
		int useVel = eventVel;

		if (m[3].matched) {
			useVel = clampVel(std::stoi(m[3].str()));
		}
		else {
			useVel = clampVel(eventVel);
		}

		const juce::String noteName = juce::String(name) + juce::String(octave);
		auto midiOpt = parseNoteName(noteName);
		if (!midiOpt.has_value()) {
			err = "Bad pitch name: " + noteName.toStdString();
			return false;
		}

		out.midi = *midiOpt;
		out.velocity = clampVel(useVel <= 0 ? defaultVelocity : useVel);
		return true;
	}



	// Utility: canonicalize a vector of PlayedNote (sort + dedupe by midi then velocity)
	static void canonicalize(std::vector<PlayedNote>& v) {
		std::sort(v.begin(), v.end(),
			[](const PlayedNote& a, const PlayedNote& b) {
				if (a.midi != b.midi) return a.midi < b.midi;
				return a.velocity < b.velocity;
			});
		v.erase(std::unique(v.begin(), v.end(),
			[](const PlayedNote& a, const PlayedNote& b) {
				return a.midi == b.midi && a.velocity == b.velocity;
			}), v.end());
	}

	// Parse "C4" or "C4-90" using NoteUtils for pitch.
	std::pair<int, int> parseOneNoteToken(const juce::String& token, int defaultVelocity) {
		return parseNoteToken(token.toStdString(), defaultVelocity);
	}

	// Parse note field that can be string or array of strings.
	// Applies per-note suffix override; otherwise uses 'eventVel'.
	static bool parseNoteOrChord(const juce::var& v,
		int eventVel,
		int defaultVelocity,
		std::vector<PlayedNote>& out,
		std::string& err)
	{
		out.clear();

		if (v.isString()) {
			PlayedNote pn{};
			if (!parseNoteIntoPlayed(v.toString().trim(), eventVel, defaultVelocity, pn, err))
				return false;
			out.push_back(pn);
			return true;
		}

		if (auto* arr = v.getArray()) {
			out.reserve(arr->size());
			for (const auto& el : *arr) {
				if (!el.isString()) { err = "Chord element must be a string note token"; return false; }
				PlayedNote pn{};
				if (!parseNoteIntoPlayed(el.toString().trim(), eventVel, defaultVelocity, pn, err))
					return false;
				out.push_back(pn);
			}
			if (out.empty()) { err = "Chord array is empty"; return false; }
			canonicalize(out);
			return true;
		}

		err = "note(s) must be a string or an array of strings";
		return false;
	}

} // namespace

std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity)
{
	std::smatch m;
	if (!std::regex_match(token, m, kNoteRegex))
		throw std::invalid_argument("Bad note token: " + token);

	const std::string name = m[1].str();
	const int octave = std::stoi(m[2].str());
	int vel = defaultVelocity;

	if (m[3].matched)
		vel = std::stoi(m[3].str());

	vel = std::max(1, std::min(127, vel));

	const juce::String noteName = juce::String(name) + juce::String(octave);
	auto midiOpt = parseNoteName(noteName);
	if (!midiOpt.has_value())
		throw std::invalid_argument("Bad pitch name: " + noteName.toStdString());

	return { *midiOpt, vel };
}

bool parseEventJson(const std::string& json,
	int defaultVelocity,
	std::string& err,
	ParsedPhrase& out)
{
	err.clear();
	out = ParsedPhrase{};

	juce::var root = juce::JSON::parse(juce::String(json));
	if (root.isVoid()) { err = "JSON parse failed"; return false; }

	auto* obj = root.getDynamicObject();
	if (!obj) { err = "Top-level JSON must be an object"; return false; }

	const juce::var vb = obj->getProperty("b");
	const juce::var vs = obj->getProperty("s");
	const juce::var ve = obj->getProperty("e");

	if (!vb.isInt() || !vs.isInt() || ve.isVoid()) {
		err = "Object must contain integer 'b', integer 's', and array 'e'";
		return false;
	}

	const int bars = (int)vb;
	const int stepsPerBar = (int)vs;
	if (bars <= 0) { err = "'b' must be > 0"; return false; }
	if (stepsPerBar <= 0) { err = "'s' must be > 0"; return false; }

	auto* events = ve.getArray();
	if (!events) { err = "'e' must be an array"; return false; }

	const int totalSteps = bars * stepsPerBar;
	std::vector<std::vector<PlayedNote>> active(totalSteps); // active notes at each step

	// --- read events ---
	for (int i = 0; i < events->size(); ++i) {
		const juce::var& ev = (*events)[i];
		auto* arr = ev.getArray();
		if (!arr || arr->size() != 4) {
			err = "event[" + std::to_string(i) + "] must be an array of length 4";
			return false;
		}

		// [startStep, noteOrNotes, durationSteps, velocity]
		const juce::var& vStart = (*arr)[0];
		const juce::var& vNotes = (*arr)[1];
		const juce::var& vDur = (*arr)[2];
		const juce::var& vVel = (*arr)[3];

		if (!vStart.isInt()) { err = "event[" + std::to_string(i) + "]: startStep must be int"; return false; }
		if (!vDur.isInt()) { err = "event[" + std::to_string(i) + "]: durationSteps must be int"; return false; }
		if (!vVel.isInt()) { err = "event[" + std::to_string(i) + "]: velocity must be int"; return false; }

		int start = (int)vStart;
		int dur = (int)vDur;
		int evVel = clampVel((int)vVel);

		if (start < 0) start = 0;
		if (dur <= 0) continue; // ignore non-positive durations

		std::vector<PlayedNote> notes;
		std::string perr;
		if (!parseNoteOrChord(vNotes, evVel, defaultVelocity, notes, perr)) {
			err = "event[" + std::to_string(i) + "]: " + perr;
			return false;
		}

		const int endExclusive = std::min(totalSteps, start + dur);
		if (start >= totalSteps || endExclusive <= start) continue; // out of range; ignore

		for (int t = start; t < endExclusive; ++t) {
			// append notes; we’ll canonicalize later per step when comparing
			active[(size_t)t].insert(active[(size_t)t].end(), notes.begin(), notes.end());
		}
	}

	// --- synthesize ParsedPhrase ---
	out.bars.clear();
	out.bars.resize((size_t)bars);
	for (int b = 0; b < bars; ++b)
		out.bars[(size_t)b].resize((size_t)stepsPerBar, StepEvent{ StepEvent::Kind::Rest, {} });

	auto prev = std::vector<PlayedNote>{};

	for (int t = 0; t < totalSteps; ++t) {
		auto cur = std::move(active[(size_t)t]);
		canonicalize(cur);

		StepEvent se;
		if (cur.empty()) {
			se.kind = StepEvent::Kind::Rest;
		}
		else {
			if (cur == prev) {
				se.kind = StepEvent::Kind::Sustain;
			}
			else {
				se.kind = StepEvent::Kind::Notes;
				se.notes = cur;
			}
		}

		const int b = t / stepsPerBar;
		const int s = t % stepsPerBar;
		out.bars[(size_t)b][(size_t)s] = std::move(se);
		prev = std::move(cur);
	}

	return true;
}