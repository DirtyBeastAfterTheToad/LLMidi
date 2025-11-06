#pragma once
#include <JuceHeader.h>

class StatusDot : public juce::Component, private juce::Timer
{
public:
    void setColour(juce::Colour newColour) { colour = newColour; repaint(); }

    void setLoading(bool shouldPulse)
    {
        if (loading == shouldPulse) return;
        loading = shouldPulse;
        if (loading) startTimerHz(60); else stopTimer();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float d = std::min(b.getWidth(), b.getHeight()) - 2.0f; 
        const float x = b.getCentreX() - d * 0.5f;
        const float y = b.getCentreY() - d * 0.5f;
        juce::Rectangle<float> circle(x, y, d, d);

        g.setColour(colour);
        g.fillEllipse(circle);
        g.setColour(colour.darker(0.6f));
        g.drawEllipse(circle, 1.0f);

        if (loading)
        {
            const float a = 0.35f + 0.25f * std::sin(phase);
            juce::Colour glow = colour.withAlpha(a);

            const float halo = d * (1.0f + 0.25f + 0.05f * std::sin(phase * 0.7f));
            juce::Rectangle<float> haloR(b.getCentreX() - halo * 0.5f,
                b.getCentreY() - halo * 0.5f, halo, halo);
            g.setColour(glow);
            g.fillEllipse(haloR);
        }
    }

private:
    void timerCallback() override { phase += 0.20f; repaint(); }

    juce::Colour colour{ juce::Colours::darkred };
    bool  loading = false;
    float phase = 0.0f;
};
