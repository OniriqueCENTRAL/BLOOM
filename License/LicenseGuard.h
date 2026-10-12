#pragma once

#include "License.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace onirique::license
{

//==============================================================================
/** Tout ce qu'un plugin JUCE doit garder de sa licence : un membre de l'AudioProcessor.

        onirique::license::Guard license { "identifiant-du-plugin" };

    - prepareToPlay : license.refresh();   (beaucoup de DAW le rappellent juste avant un export)
    - fin de processBlock : license.process (buffer, getSampleRate(), isNonRealtime());

    La licence est relue toutes les cinq secondes : une activation faite dans ONIRIQUE CENTRAL
    est prise en compte sans recharger le plugin. */
class Guard final
{
public:
    explicit Guard (const juce::String& productSlug) : guard (productSlug.toStdString()) {}

    /** État de la licence sur cette machine (acheté, essai, démo). */
    Status status() const { return guard.status(); }

    /** Relit le fichier de licence écrit par ONIRIQUE CENTRAL. Jamais depuis le thread audio. */
    void refresh() { guard.refresh(); }

    /** Thread audio, une fois le son du plugin calculé. Rend vrai quand le bloc a été remplacé par du bruit. */
    template <typename Sample>
    bool process (juce::AudioBuffer<Sample>& buffer, double sampleRate, bool offline) noexcept
    {
        return guard.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), sampleRate, offline);
    }

private:
    core::Guard guard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Guard)
};

} // namespace onirique::license
