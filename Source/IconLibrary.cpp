#include "IconLibrary.h"

std::unique_ptr<juce::Drawable> IconLibrary::makeGearDrawable(juce::Colour c)
{
	juce::Path gear;
	const float R = 6.6f;
	const float r = 4.0f;
	const float hub = 2.2f;
	const int   N = 12;
	const float cx = 8.0f, cy = 8.0f;

	auto polar = [&](float radius, float angRad)
		{
			return juce::Point<float>(cx + radius * std::cos(angRad),
				cy + radius * std::sin(angRad));
		};

	for (int i = 0; i < N; ++i)
	{
		float a0 = juce::MathConstants<float>::twoPi * (i + 0.00f) / N;
		float a1 = juce::MathConstants<float>::twoPi * (i + 0.33f) / N;
		float a2 = juce::MathConstants<float>::twoPi * (i + 0.66f) / N;

		if (i == 0) gear.startNewSubPath(polar(r, a0));
		gear.lineTo(polar(r, a0));
		gear.lineTo(polar(R, a1));
		gear.lineTo(polar(r, a2));
	}
	gear.closeSubPath();

	auto tooth = std::make_unique<juce::DrawablePath>();
	tooth->setPath(gear);
	tooth->setFill(c);
	tooth->setStrokeFill(c);
	tooth->setStrokeThickness(0.0f);

	juce::Path hubPath;
	hubPath.addEllipse(cx - hub, cy - hub, 2 * hub, 2 * hub);
	auto hubDrawable = std::make_unique<juce::DrawablePath>();
	hubDrawable->setPath(hubPath);
	hubDrawable->setFill(juce::Colours::transparentBlack);
	hubDrawable->setStrokeFill(c.withAlpha(0.9f));
	hubDrawable->setStrokeThickness(1.2f);

	auto group = std::make_unique<juce::DrawableComposite>();
	group->addAndMakeVisible(tooth.release());
	group->addAndMakeVisible(hubDrawable.release());
	group->setTransformToFit(juce::Rectangle<float>(0, 0, 16, 16), true);
	return group;
}
