#include "LlmGenAdapter.h"
#include <sstream>

ParsedSummary summarize(const ParsedPhrase& p) {
	ParsedSummary s;
	s.bars = p.barsCount();
	s.stepsPerBar = p.stepsPerBar();

	std::ostringstream prev;

	int previewLimitSteps = 12;
	int previewed = 0;

	for (int b = 0; b < s.bars; ++b) {
		if (b > 0) prev << " | ";

		for (int st = 0; st < s.stepsPerBar; ++st) {
			const auto& step = p.bars[b][st];

			if (step.kind == StepEvent::Kind::Rest) {
				s.totalRests++;
				if (previewed < previewLimitSteps) { prev << "."; previewed++; }
			}
			else if (step.kind == StepEvent::Kind::Sustain) {
				s.totalSustains++;
				if (previewed < previewLimitSteps) { prev << "-"; previewed++; }
			}
			else {
				s.totalPlayableSteps++;
				s.totalPlayableNotes += static_cast<int>(step.notes.size());
				if (previewed < previewLimitSteps) {
					if (step.notes.size() == 1) prev << "N";
					else                         prev << "C" << step.notes.size();
					previewed++;
				}
			}

			if (st != s.stepsPerBar - 1 && previewed < previewLimitSteps) prev << " ";
		}
	}

	s.shortPreview = prev.str();
	return s;
}