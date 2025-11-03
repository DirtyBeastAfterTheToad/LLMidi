#include "NoteUtils.h"
#include <algorithm>
#include <string>

namespace {

    int baseSemitone(const juce::String& letter, const juce::String& accidental)
    {
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

} 

std::optional<int> parseNoteName(const juce::String& name)
{
    if (name.isEmpty()) return std::nullopt;

    juce::String letter = name.substring(0, 1).toUpperCase();
    if (letter < "A" || letter > "G") return std::nullopt;

    juce::String accidental, octaveStr;
    int idx = 1;

    if (idx < name.length())
    {
        auto c = name.substring(idx, idx + 1);
        if (c == "#" || c == "b") { accidental = c; ++idx; }
    }

    octaveStr = name.substring(idx).trim();
    if (octaveStr.isEmpty()) return std::nullopt;

    int octave = octaveStr.getIntValue();
    const int semitone = baseSemitone(letter, accidental);
    const int midi = 12 * (octave + 1) + semitone;

    if (midi < 0 || midi > 127) return std::nullopt;
    return midi;
}

juce::String midiToName(int midi)
{
    static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    midi = juce::jlimit(0, 127, midi);
    const int octave = midi / 12 - 1;
    const int pc = midi % 12;
    return juce::String(names[pc]) + juce::String(octave);
}
