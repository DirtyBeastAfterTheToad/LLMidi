#include "MidiScheduler.h"
#include <JuceHeader.h>

namespace {
	inline void addNote(std::vector<ScheduledMidi>& evts,
		double onPPQ, double offPPQ,
		int channel, int pitch, int velocity)
	{
		evts.push_back(ScheduledMidi{ onPPQ, 0, channel, pitch, velocity });
		evts.push_back(ScheduledMidi{ offPPQ, 1, channel, pitch, 0 });
	}
}

void MidiScheduler::clear()
{
	events.clear();
}

void MidiScheduler::buildFromSequence(const Sequence& seq,
	double startBarPPQ,
	double beatsPerBar)
{
	clear();

	const int bars = (int)seq.data.size();
	if (bars <= 0) return;

	const double beatsPerStep = beatsPerBar / (double)seq.stepsPerBar;
	const int channel = juce::jlimit(0, 15, (int)seq.midiChannel);

	for (int b = 0; b < bars; ++b)
	{
		const auto& bar = seq.data[(size_t)b];

		for (int s = 0; s < seq.stepsPerBar; ++s)
		{
			const auto& step = bar.steps[(size_t)s];
			const double stepStartPPQ = startBarPPQ
				+ (double)b * beatsPerBar
				+ (double)s * beatsPerStep;

			if (step.isRest() || step.isSustain())
				continue;

			// Count consecutive sustains to extend duration
			int sustainCount = 0;
			{
				int bb = b;
				int ss = s + 1;
				while (bb < bars)
				{
					if (ss >= seq.stepsPerBar)
					{
						++bb; ss = 0;
						if (bb >= bars) break;
					}

					if (seq.data[(size_t)bb].steps[(size_t)ss].isSustain())
					{
						++sustainCount;
						++ss;
					}
					else break;
				}
			}

			const int totalSteps = 1 + sustainCount;
			const double stepEndPPQ = stepStartPPQ + beatsPerStep * (double)totalSteps;
			if (stepEndPPQ <= stepStartPPQ)
				continue;
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

	std::sort(events.begin(), events.end(),
		[](const ScheduledMidi& a, const ScheduledMidi& b)
		{
			if (a.ppq < b.ppq) return true;
			if (a.ppq > b.ppq) return false;
			// same timestamp: put NoteOff first
			return a.type > b.type; // 1 (off) before 0 (on)
		});
}

void MidiScheduler::getEventsInRange(double ppqStart, double ppqEnd,
	std::vector<const ScheduledMidi*>& out) const
{
	out.clear();
	if (events.empty()) return;

	for (const auto& e : events)
	{
		if (e.ppq >= ppqEnd) break;
		if (e.ppq >= ppqStart)
			out.push_back(&e);
	}
}
