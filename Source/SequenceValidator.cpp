#include "SequenceValidator.h"

bool validate(const Sequence& seq, juce::String& errorOut)
{
    if (seq.bars <= 0) {
        errorOut = "Sequence must have at least 1 bar.";
        return false;
    }

    if (seq.stepsPerBar != 4 && seq.stepsPerBar != 8) {
        errorOut = "stepsPerBar must be 4 or 8.";
        return false;
    }

    if ((int)seq.data.size() != seq.bars) {
        errorOut = "Sequence data size does not match bars.";
        return false;
    }

    for (int b = 0; b < seq.bars; ++b)
    {
        const auto& bar = seq.data[(size_t)b];

        if ((int)bar.steps.size() != seq.stepsPerBar) {
            errorOut = "Bar " + juce::String(b + 1) + " has wrong number of steps.";
            return false;
        }

        for (int s = 0; s < seq.stepsPerBar; ++s)
        {
            const auto& step = bar.steps[(size_t)s];

            if (s == 0 && step.isSustain()) {
                errorOut = "Bar " + juce::String(b + 1) + " cannot start with a sustain '-'.";
                return false;
            }

            if (step.isNote() || step.isChord())
            {
                if (step.notes.empty()) {
                    errorOut = "Note/Chord step has no notes.";
                    return false;
                }

                for (const auto& n : step.notes)
                {
                    if (n.midi < 0 || n.midi > 127) {
                        errorOut = "MIDI note out of range (0..127).";
                        return false;
                    }
                    if (n.velocity < 1 || n.velocity > 127) {
                        errorOut = "Velocity out of range (1..127).";
                        return false;
                    }
                }
            }
            else if (!step.notes.empty()) {
                errorOut = "Rest/Sustain step must not contain notes.";
                return false;
            }
        }
    }

    return true;
}
