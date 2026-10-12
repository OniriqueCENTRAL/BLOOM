#pragma once
// ============================================================================
//  BLOOM : licence
//  La licence de BLOOM est celle d'ONIRIQUE CENTRAL, commune a tous les plugins
//  (dossier License/ : activation depuis l'application, pastille et page
//  d'activation dans le plugin, bruit blanc sans licence).
//
//  BLOOM avait sa propre licence, avec sa page d'activation et son etiquette
//  DEMO. Ce fichier garde ce que le reste du code en attend, et repond toujours
//  « active » : la page et l'etiquette d'origine ne s'affichent plus, celles de
//  la licence commune prennent leur place.
// ============================================================================
#include <JuceHeader.h>
#include <functional>

namespace bloomlic
{
    inline constexpr const char* kBuyUrl = "https://onirique.store/boutique";

    struct Result
    {
        bool ok = true;
        juce::String message;
    };

    class License
    {
    public:
        static License& get()
        {
            static License instance;
            return instance;
        }

        void startup() {}
        bool isLicensed() const { return true; }
        juce::String getMaskedKey() const { return {}; }
        void activate(const juce::String&, std::function<void(Result)> done) { if (done) done({}); }
    };
}
