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

	// Converts a JUCE var (step) into StepEvent.
	StepEvent parseStepVar(const juce::var& v, int defaultVel, std::string& err)
	{
		StepEvent se;

		if (v.isString())
		{
			const std::string tok = v.toString().trim().toStdString();
			if (tok == ".") { se.kind = StepEvent::Kind::Rest;    return se; }
			if (tok == "-") { se.kind = StepEvent::Kind::Sustain; return se; }

			try {
				auto [midi, vel] = parseNoteToken(tok, defaultVel);
				se.kind = StepEvent::Kind::Notes;
				se.notes.push_back({ midi, vel });
				return se;
			}
			catch (const std::exception& e) {
				err = e.what();
				return {};
			}
		}

		if (auto* arr = v.getArray())
		{
			StepEvent chord;
			chord.kind = StepEvent::Kind::Notes;

			if (arr->isEmpty())
			{
				chord.kind = StepEvent::Kind::Rest;
				return chord;
			}

			for (const auto& el : *arr)
			{
				if (!el.isString()) { err = "Chord element is not a string"; return {}; }

				try {
					auto [midi, vel] = parseNoteToken(el.toString().trim().toStdString(), defaultVel);
					chord.notes.push_back({ midi, vel });
				}
				catch (const std::exception& e) {
					err = e.what();
					return {};
				}
			}
			return chord;
		}

		err = "Step must be string or array";
		return {};
	}

}

std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity)
{
	std::smatch m;
	if (!std::regex_match(token, m, kNoteRegex))
		throw std::invalid_argument("Bad note token: " + token);

	const std::string name = m[1].str();   // e.g., "A#"
	const int octave = std::stoi(m[2].str());
	int vel = defaultVelocity;

	if (m[3].matched)
		vel = std::stoi(m[3].str());

	vel = std::max(1, std::min(127, vel));

	// Reuse shared helper (keeps pitch-name logic in one place)
	const juce::String noteName = juce::String(name) + juce::String(octave);
	auto midiOpt = parseNoteName(noteName);
	if (!midiOpt.has_value())
		throw std::invalid_argument("Bad pitch name: " + noteName.toStdString());

	return { *midiOpt, vel };
}

bool parseJsonBars(const std::string& json,
	int defaultVelocity,
	std::string& err,
	ParsedPhrase& out)
{
	err.clear();
	out = ParsedPhrase{};

	juce::var root = juce::JSON::parse(juce::String(json));
	if (root.isVoid()) { err = "JSON parse failed"; return false; }

	auto* bars = root.getArray();
	if (!bars) { err = "Top-level must be an array of bars"; return false; }
	if (bars->isEmpty()) { err = "No bars"; return false; }

	out.bars.reserve(bars->size());
	int expectedSteps = -1;

	for (const auto& barVar : *bars)
	{
		auto* steps = barVar.getArray();
		if (!steps) { err = "Bar is not an array of steps"; return false; }
		if (steps->isEmpty()) { err = "Bar has zero steps"; return false; }

		if (expectedSteps < 0) expectedSteps = steps->size();
		if (steps->size() != expectedSteps) {
			err = "Inconsistent steps per bar (found a bar with different length)";
			return false;
		}

		std::vector<StepEvent> bar;
		bar.reserve(steps->size());

		for (const auto& stepVar : *steps)
		{
			std::string stepErr;
			StepEvent se = parseStepVar(stepVar, defaultVelocity, stepErr);
			if (!stepErr.empty()) {
				err = "Step parse error: " + stepErr;
				return false;
			}
			bar.push_back(std::move(se));
		}

		out.bars.push_back(std::move(bar));
	}

	return true;
}
