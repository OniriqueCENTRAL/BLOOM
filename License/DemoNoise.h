#pragma once

#include <atomic>
#include <cstdint>

namespace onirique::license
{

enum class State
{
    unlicensed, // pas de licence valable sur cette machine : bruit blanc à l'export, et par salves à l'écoute
    trial,      // essai gratuit en cours : écoute intacte, bruit blanc à l'export
    licensed    // plugin acheté ou abonnement actif
};

//==============================================================================
/** Le bruit blanc du mode démo, à poser une fois le son du plugin calculé.

    - plugin acheté : rien ;
    - essai gratuit : l'écoute en temps réel reste intacte, un export (rendu hors temps réel)
      ne contient que du bruit ;
    - pas de licence, ou essai terminé : l'export est du bruit, et l'écoute en temps réel
      est coupée par deux secondes de bruit toutes les trente secondes. */
class DemoNoise
{
public:
    /** L'état lu dans la licence. Depuis n'importe quel thread. */
    void setState (State newState) noexcept { state.store ((int) newState, std::memory_order_relaxed); }

    /** Thread audio. Rend vrai quand le bloc a été remplacé par du bruit. */
    template <typename Sample>
    bool process (Sample* const* channels, int numChannels, int numSamples, double sampleRate, bool offline) noexcept
    {
        const auto current = (State) state.load (std::memory_order_relaxed);
        if (current == State::licensed)
            return false;

        bool noisy = offline;

        if (! offline && current == State::unlicensed)
        {
            const auto period = (std::int64_t) (sampleRate * periodSeconds);
            noisy = period > 0 && position % period >= period - (std::int64_t) (sampleRate * burstSeconds);
            position += numSamples;
        }

        if (! noisy)
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

    static constexpr double periodSeconds = 30.0, burstSeconds = 2.0;

private:
    std::atomic<int> state { (int) State::unlicensed };
    std::int64_t position = 0;
    std::uint32_t seed = 0x9e3779b9u;
};

} // namespace onirique::license
