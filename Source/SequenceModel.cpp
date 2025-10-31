#include "SequenceModel.h"

    static int baseSemitone(const juce::String& letter, const juce::String& accidental)
    {
        // Sharps as canonical; flats are mapped.
        // C=0 C#=1 D=2 D#=3 E=4 F=5 F#=6 G=7 G#=8 A=9 A#=10 B=11
        int base = 0;
        if (letter == "C") base = 0;
        else if (letter == "D") base = 2;
        else if (letter == "E") base = 4;
        else if (letter == "F") base = 5;
        else if (letter == "G") base = 7;
        else if (letter == "A") base = 9;
        else if (letter == "B") base = 11;

        if (accidental == "#") base += 1;
        else if (accidental == "b") base -= 1;

        base = (base + 12) % 12;
        return base;
    }

    std::optional<int> parseNoteName(const juce::String& name)
    {
        // Accept formats like A#3, Bb2, C4, F#-1
        // MIDI standard: C-1 = 0, C0 = 12, C4 = 60
        if (name.isEmpty()) return std::nullopt;

        juce::String letter, accidental, octaveStr;
        // Parse letter
        letter = name.substring(0, 1).toUpperCase();
        if (letter < "A" || letter > "G") return std::nullopt;

        int idx = 1;
        if (idx < name.length())
        {
            juce::String c = name.substring(idx, idx + 1);
            if (c == "#" || c == "b") { accidental = c; ++idx; }
        }
        // Remainder is octave (can be negative)
        octaveStr = name.substring(idx).trim();
        if (octaveStr.isEmpty()) return std::nullopt;

        bool ok = true;
        int octave = octaveStr.getIntValue(); // handles negatives
        // Validate integer parse (heuristic)
        juce::String check = juce::String(octave);
        if (!octaveStr.startsWith(check) && !octaveStr.endsWith(check))
            ok = false;
        if (!ok) return std::nullopt;

        const int semitone = baseSemitone(letter, accidental);
        const int midi = 12 * (octave + 1) + semitone;
        if (midi < 0 || midi > 127) return std::nullopt;
        return midi;
    }

    juce::String midiToName(int midi)
    {
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        midi = juce::jlimit(0, 127, midi);
        int octave = midi / 12 - 1;
        int pc = midi % 12;
        return juce::String(names[pc]) + juce::String(octave);
    }

    bool validate(const Sequence& seq, juce::String& errorOut)
    {
        if (seq.bars <= 0) { errorOut = "Sequence must have at least 1 bar."; return false; }
        if (seq.stepsPerBar != 4 && seq.stepsPerBar != 8)
        {
            errorOut = "stepsPerBar must be 4 or 8."; return false;
        }

        if ((int)seq.data.size() != seq.bars)
        {
            errorOut = "Sequence data size does not match bars."; return false;
        }

        for (int b = 0; b < seq.bars; ++b)
        {
            const auto& bar = seq.data[(size_t)b];
            if ((int)bar.steps.size() != seq.stepsPerBar)
            {
                errorOut = "Bar " + juce::String(b + 1) + " has wrong number of steps.";
                return false;
            }

            for (int s = 0; s < seq.stepsPerBar; ++s)
            {
                const auto& step = bar.steps[(size_t)s];

                // Optional rule: disallow starting a bar with sustain, as there's nothing to sustain from
                if (s == 0 && step.isSustain())
                {
                    errorOut = "Bar " + juce::String(b + 1) + " cannot start with a sustain '-'.";
                    return false;
                }

                if (step.isNote() || step.isChord())
                {
                    if (step.notes.empty())
                    {
                        errorOut = "Note/Chord step has no notes."; return false;
                    }

                    for (const auto& n : step.notes)
                    {
                        if (n.midi < 0 || n.midi > 127)
                        {
                            errorOut = "MIDI note out of range (0..127)."; return false;
                        }
                        if (n.velocity < 1 || n.velocity > 127)
                        {
                            errorOut = "Velocity out of range (1..127)."; return false;
                        }
                    }
                }
                else
                {
                    // Rest or sustain must not carry notes
                    if (!step.notes.empty())
                    {
                        errorOut = "Rest/Sustain step must not contain notes."; return false;
                    }
                }
            }
        }
        return true;
    }
