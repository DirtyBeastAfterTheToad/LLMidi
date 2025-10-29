#pragma once
#include <JuceHeader.h>
#include <vector>
#include <memory>
#include "MidiScheduler.h"

// A complete, immutable set of scheduled events for a sequence.
// startPPQ/endPPQ allow quick culling by the audio thread.
struct EventTimeline
{
    double startPPQ = 0.0;
    double endPPQ = 0.0;
    std::vector<ScheduledMidi> events; // sorted by ppq

    bool empty() const { return events.empty(); }
};
