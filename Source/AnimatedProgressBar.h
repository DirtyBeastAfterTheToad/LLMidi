#pragma once
#include <JuceHeader.h>

class AnimatedProgressBar : public juce::Component, private juce::Timer
{
public:
    void setProgress(double p) { progress = juce::jlimit(0.0, 1.0, p); repaint(); }

    void setActive(bool on)
    {
        if (active == on) return;
        active = on;
        if (active) startTimerHz(60); else stopTimer();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float radius = 6.0f;

        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillRoundedRectangle(r, radius);
        g.setColour(juce::Colours::grey);
        g.drawRoundedRectangle(r, radius, 1.0f);

        auto filled = r.removeFromLeft((float)(getWidth() * progress)).toFloat();
        filled.setHeight((float)getHeight());
        g.setColour(juce::Colours::limegreen.withAlpha(0.9f));
        g.fillRoundedRectangle(filled, radius);

        if (active && progress > 0.0)
        {
            const float w = (float)getWidth();
            const float h = (float)getHeight();
            const float stripeW = 28.0f;
            const float dx = std::fmod(phase, stripeW);

            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(filled.getSmallestIntegerContainer());

            for (float x = -stripeW + dx; x < w; x += stripeW)
            {
                juce::Path p;
                p.startNewSubPath(x, 0.0f);
                p.lineTo(x + stripeW * 0.5f, 0.0f);
                p.lineTo(x + stripeW, h);
                p.lineTo(x + stripeW * 0.5f, h);
                p.closeSubPath();

                g.fillPath(p);
            }
        }
    }

private:
    void timerCallback() override { phase += 1.5f; repaint(); }

    double progress = 0.0;
    bool   active = false;
    float  phase = 0.0f;
};
