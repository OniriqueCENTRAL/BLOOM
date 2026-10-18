#pragma once

#include <atomic>
#include <cstdint>

namespace onirique::license
{

enum class State
{
    unlicensed, // pas de licence valable sur cette machine : écoute intacte, bruit blanc à l'export
    trial,      // essai gratuit en cours : écoute intacte, bruit blanc à l'export
    licensed    // plugin acheté ou abonnement actif
};

//==============================================================================
/** Le bruit blanc du mode démo, à poser une fois le son du plugin calculé.

    - plugin acheté : rien ;
    - essai gratuit, pas de licence ou essai terminé : l'écoute en temps réel dans le DAW reste
      intacte, un export (rendu hors temps réel) ne contient que du bruit. */
class DemoNoise
{
public:
    /** L'état lu dans la licence. Depuis n'importe quel thread. */
    void setState (State newState) noexcept { state.store ((int) newState, std::memory_order_relaxed); }

    /** Thread audio. Rend vrai quand le bloc a été remplacé par du bruit. */
    template <typename Sample>
    bool process (Sample* const* channels, int numChannels, int numSamples, double /* sampleRate : plus utile, gardé pour les appels */, bool offline) noexcept
    {
        if (! offline || (State) state.load (std::memory_order_relaxed) == State::licensed)
            return false;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (channels[ch] == nullptr)
                continue;

            for (int i = 0; i < numSamples; ++i)
            {
                // xorshift32 : assez pour un bruit blanc, et rien à allouer
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                channels[ch][i] = (Sample) (((double) seed / 4294967295.0 * 2.0 - 1.0) * 0.25);
            }
        }

        return true;
    }

private:
    std::atomic<int> state { (int) State::unlicensed };
    std::uint32_t seed = 0x9e3779b9u;
};

} // namespace onirique::license
