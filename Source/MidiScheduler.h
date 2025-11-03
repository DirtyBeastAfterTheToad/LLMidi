#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include "SequenceModel.h"

struct ScheduledMidi
{
	double ppq = 0.0;  // absolute PPQ position
	int type = 0;      // 0 = NoteOn, 1 = NoteOff
	int channel = 0;   // 0..15
	int pitch = 60;    // 0..127
	int velocity = 100;// 1..127 (NoteOff uses 0)
};

class MidiScheduler
{
public:
	void clear();

	void buildFromSequence(const Sequence& seq,
		double startBarPPQ,
		double beatsPerBar);

	void getEventsInRange(double ppqStart, double ppqEnd,
		std::vector<const ScheduledMidi*>& out) const;

	bool   empty()    const { return events.empty(); }
	double firstPPQ() const { return events.empty() ? 0.0 : events.front().ppq; }
	double lastPPQ()  const { return events.empty() ? 0.0 : events.back().ppq; }

private:
	std::vector<ScheduledMidi> events; // sorted by ppq
};
