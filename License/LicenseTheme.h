#pragma once

#include <OniriqueLicenseData.h>
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
//  Ce qu'il faut de la charte ONIRIQUE pour dessiner la pastille et la page
//  d'activation, quel que soit le plugin : couleurs, polices, étoile.
//==============================================================================
namespace onirique::license::theme
{

namespace palette
{
    inline const juce::Colour deep     { 0xff1a080a };
    inline const juce::Colour deep2    { 0xff2a0a0e };
    inline const juce::Colour mist     { 0xfff6eeee };
    inline const juce::Colour haze     { 0xffa88f92 };
    inline const juce::Colour ember    { 0xffe23444 };
    inline const juce::Colour emberHot { 0xffff5a68 };
    inline const juce::Colour bone     { 0xffffecee };
    inline const juce::Colour rose     { 0xffffd2d6 };
    inline const juce::Colour active   { 0xff6fdc9c }; // le témoin « ACTIVÉ »
}

/** Unbounded (titres), Hanken Grotesk (texte), JetBrains Mono (étiquettes). */
struct FontSet
{
    FontSet()
    {
        display = juce::Typeface::createSystemTypefaceFor (OniriqueLicenseData::Unbounded800_ttf, OniriqueLicenseData::Unbounded800_ttfSize);
        body = juce::Typeface::createSystemTypefaceFor (OniriqueLicenseData::HankenGrotesk600_ttf, OniriqueLicenseData::HankenGrotesk600_ttfSize);
        mono = juce::Typeface::createSystemTypefaceFor (OniriqueLicenseData::JetBrainsMono500_ttf, OniriqueLicenseData::JetBrainsMono500_ttfSize);
        monoBold = juce::Typeface::createSystemTypefaceFor (OniriqueLicenseData::JetBrainsMono600_ttf, OniriqueLicenseData::JetBrainsMono600_ttfSize);
    }

    static juce::Font make (const juce::Typeface::Ptr& face, float height, float tracking)
    {
        return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (tracking);
    }

    juce::Font displayFont (float height, float tracking = 0.0f) const  { return make (display, height, tracking); }
    juce::Font bodyFont (float height, float tracking = 0.0f) const     { return make (body, height, tracking); }
    juce::Font monoFont (float height, float tracking = 0.0f) const     { return make (mono, height, tracking); }
    juce::Font monoBoldFont (float height, float tracking = 0.0f) const { return make (monoBold, height, tracking); }

    juce::Typeface::Ptr display, body, mono, monoBold;
};

using Fonts = juce::SharedResourcePointer<FontSet>;

/** Étoile à quatre branches du logo. */
inline juce::Path starPath (juce::Point<float> centre, float size)
{
    static constexpr float points[8][2] = {
        { 50, 0 }, { 56, 44 }, { 100, 50 }, { 56, 56 }, { 50, 100 }, { 44, 56 }, { 0, 50 }, { 44, 44 }
    };

    juce::Path p;
    for (int i = 0; i < 8; ++i)
    {
        const juce::Point<float> pt (centre.x + (points[i][0] - 50.0f) * size / 100.0f,
                                     centre.y + (points[i][1] - 50.0f) * size / 100.0f);
        if (i == 0)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
    }

    p.closeSubPath();
    return p;
}

/** Petite étiquette en capitales espacées, précédée de l'étoile. */
inline void drawEyebrow (juce::Graphics& g, const FontSet& fonts, const juce::String& text, float x, float y)
{
    g.setColour (palette::ember.withAlpha (0.55f));
    g.fillPath (starPath ({ x + 5.0f, y }, 15.0f));
    g.setColour (palette::mist);
    g.fillPath (starPath ({ x + 5.0f, y }, 10.0f));
    g.setColour (palette::haze);
    g.setFont (fonts.monoFont (10.5f, 0.16f));
    g.drawText (text.toUpperCase(), juce::Rectangle<float> (x + 17.0f, y - 8.0f, 240.0f, 16.0f), juce::Justification::centredLeft, false);
}

} // namespace onirique::license::theme
