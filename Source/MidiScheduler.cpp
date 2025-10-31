#include "MidiScheduler.h"

void MidiScheduler::clear()
{
    events.clear();
}

static void addNote(std::vector<ScheduledMidi>& evts,
    double onPPQ, double offPPQ,
    int channel, int pitch, int velocity)
{
    ScheduledMidi on{ onPPQ, 0, channel, pitch, velocity };
    ScheduledMidi off{ offPPQ, 1, channel, pitch, 0 };
    evts.push_back(on);
    evts.push_back(off);
}

// Build events from bars/steps. Each step has length = 1 step.
// Sustains extend the previous note or chord by +1 step per sustain.
void MidiScheduler::buildFromSequence(const Sequence& seq,
    double startBarPPQ,
    double beatsPerBar)
{
    clear();

    const int bars = (int)seq.data.size();
    if (bars <= 0) return;

    const double beatsPerStep = beatsPerBar / (double)seq.stepsPerBar;

    for (int b = 0; b < bars; ++b)
    {
        const auto& bar = seq.data[(size_t)b];

        for (int s = 0; s < seq.stepsPerBar; ++s)
        {
            const auto& step = bar.steps[(size_t)s];
            const double stepStartPPQ = startBarPPQ
                + (double)b * beatsPerBar
                + (double)s * beatsPerStep;

            if (step.isRest())
                continue;

            if (step.isSustain())
                continue; // will be handled by duration extension from previous note(s)

            // Determine sustain-run length
            int sustainCount = 0;
            {
                int bb = b;
                int ss = s + 1;
                while (bb < bars)
                {
                    if (ss >= seq.stepsPerBar)
                    {
                        // move to next bar
                        ++bb;
                        ss = 0;
                        if (bb >= bars) break;
                    }

                    if (seq.data[(size_t)bb].steps[(size_t)ss].isSustain())
                    {
                        ++sustainCount;
                        ++ss;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            const int totalSteps = 1 + sustainCount;
            const double stepEndPPQ = stepStartPPQ + beatsPerStep * (double)totalSteps;

            const int channel = juce::jlimit(0, 15, (int)seq.midiChannel);

            if (step.isNote() || step.isChord())
            {
                for (const auto& n : step.notes)
                {
                    const int pitch = juce::jlimit(0, 127, n.midi);
                    const int vel = juce::jlimit(1, 127, (int)n.velocity);
                    addNote(events, stepStartPPQ, stepEndPPQ, channel, pitch, vel);
                }
            }
        }
    }

    // Sort by time to be safe
    std::sort(events.begin(), events.end(),
        [](const ScheduledMidi& a, const ScheduledMidi& b)
        { return a.ppq < b.ppq || (a.ppq == b.ppq && a.type < b.type); });
}

void MidiScheduler::getEventsInRange(double ppqStart, double ppqEnd,
    std::vector<const ScheduledMidi*>& out) const
{
    out.clear();
    if (events.empty()) return;

    // Linear scan is fine for MVP; can be optimized with an index.
    for (const auto& e : events)
    {
        if (e.ppq >= ppqEnd) break;
        if (e.ppq >= ppqStart)
            out.push_back(&e);
    }
}
