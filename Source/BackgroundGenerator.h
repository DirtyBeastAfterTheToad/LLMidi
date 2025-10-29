#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "SequenceModel.h"
#include "MidiScheduler.h"
#include "Timeline.h"

// Builds timelines off the audio thread and exposes the latest safely.
class BackgroundGenerator : private juce::Thread
{
public:
    BackgroundGenerator()
        : juce::Thread("LLMidi-Generator")
    {
    }

    ~BackgroundGenerator() override
    {
        signalThreadShouldExit();
        notifyWorkAvailable();
        stopThread(2000);
    }

    // Request a new build. Safe to call from the message thread or editor.
    // For now this just stores the parameters and wakes the thread.
    void requestBuild(const llmidi::Sequence& seq, double startBarPPQ, double beatsPerBar)
    {
        const juce::ScopedLock sl(requestLock);
        pendingSeq = seq;
        pendingStartBarPPQ = startBarPPQ;
        pendingBeatsPerBar = beatsPerBar;
        hasPending.store(true);
        notifyWorkAvailable();
    }

    std::shared_ptr<const EventTimeline> getCurrentTimeline() const
    {
        return std::atomic_load_explicit(&currentTimeline, std::memory_order_acquire);
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            // Wait for work
            workEvent.wait(500);
            if (threadShouldExit()) break;

            bool doWork = false;
            llmidi::Sequence localSeq;
            double localStartPPQ = 0.0;

            {
                const juce::ScopedLock sl(requestLock);
                if (hasPending.load())
                {
                    localSeq = pendingSeq;
                    localStartPPQ = pendingStartBarPPQ;
                    hasPending.store(false);
                    doWork = true;
                }
            }
            double localBeatsPerBar = 4.0;
            {
                const juce::ScopedLock sl(requestLock);
                if (hasPending.load())
                {
                    localSeq = pendingSeq;
                    localStartPPQ = pendingStartBarPPQ;
                    localBeatsPerBar = pendingBeatsPerBar;
                    hasPending.store(false);
                    doWork = true;
                }
            }
            if (!doWork)
                continue;

            // Build a fresh timeline
            auto mutableTimeline = std::make_shared<EventTimeline>();
            MidiScheduler sched;
            sched.buildFromSequence(localSeq, localStartPPQ, localBeatsPerBar);

            // Copy events into the timeline
            std::vector<const ScheduledMidi*> ptrs;
            sched.getEventsInRange(localStartPPQ, 1.0e12, ptrs);
            mutableTimeline->events.reserve(ptrs.size());
            for (auto* e : ptrs) mutableTimeline->events.push_back(*e);

            mutableTimeline->startPPQ = localStartPPQ;
            mutableTimeline->endPPQ = mutableTimeline->events.empty() ? localStartPPQ
                : mutableTimeline->events.back().ppq;

            // Publish atomically as const
            std::shared_ptr<const EventTimeline> timeline = mutableTimeline;
            std::atomic_store_explicit(&currentTimeline, timeline, std::memory_order_release);
        }
    }

private:
    void notifyWorkAvailable()
    {
        workEvent.signal();
        if (!isThreadRunning())
            startThread();
    }
    double pendingBeatsPerBar = 4.0;
    // Work request storage
    juce::CriticalSection requestLock;
    llmidi::Sequence pendingSeq;
    double pendingStartBarPPQ = 0.0;
    std::atomic<bool> hasPending{ false };

    // Published timeline (immutable)
    std::shared_ptr<const EventTimeline> currentTimeline { nullptr };

    // Waker
    juce::WaitableEvent workEvent;
};
