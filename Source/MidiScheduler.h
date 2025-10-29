#pragma once
#include <JuceHeader.h>
#include <vector>
#include <optional>
#include "SequenceModel.h"

// Simple scheduled MIDI event with absolute PPQ time
struct ScheduledMidi
{
    double ppq = 0.0;      // absolute PPQ position where event should occur
    int type = 0;          // 0 = NoteOn, 1 = NoteOff
    int channel = 0;       // 0..15
    int pitch = 60;        // 0..127
    int velocity = 100;    // 1..127 (NoteOff uses 0 or small value)
};

// Builds a flat list of NoteOn/NoteOff events from a Sequence.
// Assumes a constant tempo during playback window.
class MidiScheduler
{
public:
    void clear();

    // Expand a sequence to absolute PPQ events starting at startBarPPQ.
    // startBarPPQ should be the PPQ position of the bar where bar 0 of the sequence begins.
    void buildFromSequence(const llmidi::Sequence& seq,
        double startBarPPQ,
        double beatsPerBar);

    // Returns a view of events that fall within [ppqStart, ppqEnd).
    // Returned events are copied into out (with sample offsets computed by caller).
    void getEventsInRange(double ppqStart, double ppqEnd,
        std::vector<const ScheduledMidi*>& out) const;

    bool empty() const { return events.empty(); }
    double firstPPQ() const { return events.empty() ? 0.0 : events.front().ppq; }
    double lastPPQ() const { return events.empty() ? 0.0 : events.back().ppq; }

private:
    std::vector<ScheduledMidi> events; // sorted by ppq
};

