#include "LlmSequenceParser.h"
#include <regex>
#include <stdexcept>
#include <cctype>
#include <algorithm>

// Light JSON dependency via JUCE is fine in your project; if you want to avoid it,
// you can switch to a single-header JSON later. Keeping SRP: this file only parses.
#include <JuceHeader.h>


int nameToSemitone(const std::string& name) {
    // Accept C, C#, Db, D, D#, Eb, ... B
    if (name == "C")  return 0;
    else if (name == "C#") return 1;
    else if (name == "Db") return 1;
    else if (name == "D")  return 2;
    else if (name == "D#") return 3;
    else if (name == "Eb") return 3;
    else if (name == "E")  return 4;
    else if (name == "F")  return 5;
    else if (name == "F#") return 6;
    else if (name == "Gb") return 6;
    else if (name == "G")  return 7;
    else if (name == "G#") return 8;
    else if (name == "Ab") return 8;
    else if (name == "A")  return 9;
    else if (name == "A#") return 10;
    else if (name == "Bb") return 10;
    else if (name == "B")  return 11;
    throw std::invalid_argument("Bad pitch name: " + name);
}

// MIDI: C-1 = 0, C0 = 12, C4 = 60
int noteToMidi(const std::string& name, int octave) {
    const int semi = nameToSemitone(name);
    const int midi = 12 * (octave + 1) + semi;
    if (midi < 0 || midi > 127) throw std::out_of_range("MIDI out of range: " + std::to_string(midi));
    return midi;
}

std::string toStdString(const juce::String& s) {
    return s.toStdString();
}



// Matches  A#3       or  A#3-72
// group1: name (A..G with optional #/b)
// group2: octave (may be negative)
// group3: optional velocity 1..3 digits (we clamp later)
static const std::regex kNoteRegex(R"(^([A-G][#b]?)(-?\d+)(?:-([0-9]{1,3}))?$)");

std::pair<int, int> parseNoteToken(const std::string& token, int defaultVelocity) {
    std::smatch m;
    if (!std::regex_match(token, m, kNoteRegex))
        throw std::invalid_argument("Bad note token: " + token);

    const std::string name = m[1].str();
    const int octave = std::stoi(m[2].str());
    int vel = defaultVelocity;

    if (m[3].matched) {
        vel = std::stoi(m[3].str());
    }

    vel = std::max(1, std::min(127, vel));

    const int midi = noteToMidi(name, octave);
    return { midi, vel };
}

static StepEvent parseStepVar(const juce::var& v, int defaultVel, std::string& err) {
    StepEvent se;

    if (v.isString()) {
        const std::string tok = toStdString(v.toString().trim());
        if (tok == ".") {
            se.kind = StepEvent::Kind::Rest;
            return se;
        }
        if (tok == "-") {
            se.kind = StepEvent::Kind::Sustain;
            return se;
        }

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

    if (auto* arr = v.getArray()) {
        // chord
        StepEvent chord;
        chord.kind = StepEvent::Kind::Notes;

        if (arr->isEmpty()) {
            // empty chord -> treat as rest (or you could error)
            chord.kind = StepEvent::Kind::Rest;
            return chord;
        }

        for (const auto& el : *arr) {
            if (!el.isString()) {
                err = "Chord element is not a string";
                return {};
            }
            try {
                auto [midi, vel] = parseNoteToken(toStdString(el.toString().trim()), defaultVel);
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

bool parseJsonBars(const std::string& json,
    int defaultVelocity,
    std::string& err,
    ParsedPhrase& out)
{
    err.clear();
    out = ParsedPhrase{};

    juce::var root = juce::JSON::parse(juce::String(json));
    if (root.isVoid()) {
        err = "JSON parse failed";
        return false;
    }

    auto* bars = root.getArray();
    if (!bars) {
        err = "Top-level must be an array of bars";
        return false;
    }
    if (bars->isEmpty()) {
        err = "No bars";
        return false;
    }

    out.bars.reserve(bars->size());

    int expectedSteps = -1;

    for (const auto& barVar : *bars) {
        auto* steps = barVar.getArray();
        if (!steps) {
            err = "Bar is not an array of steps";
            return false;
        }
        if (steps->isEmpty()) {
            err = "Bar has zero steps";
            return false;
        }

        if (expectedSteps < 0) expectedSteps = steps->size();
        if (steps->size() != expectedSteps) {
            err = "Inconsistent steps per bar (found a bar with different length)";
            return false;
        }

        std::vector<StepEvent> bar;
        bar.reserve(steps->size());

        for (const auto& stepVar : *steps) {
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

