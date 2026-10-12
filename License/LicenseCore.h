#pragma once

#include "DemoNoise.h"

#include <cstdint>
#include <memory>
#include <string>

//==============================================================================
//  Licence ONIRIQUE : lecture et vérification du fichier écrit par l'application
//  ONIRIQUE CENTRAL quand le plugin est activé sur cette machine
//  (C:\ProgramData\Onirique\Licenses\<plugin>.lic sous Windows,
//  /Users/Shared/Onirique/Licenses/<plugin>.lic sur Mac).
//
//  Le fichier est signé par le serveur (Ed25519). Le plugin n'embarque que la
//  clé publique : il peut vérifier une licence, pas en fabriquer une. Tout se
//  fait hors ligne.
//
//  Ce fichier ne dépend d'aucune bibliothèque : c'est le même code dans tous les
//  plugins ONIRIQUE, qu'ils soient écrits avec JUCE ou non (voir README.md).
//==============================================================================
namespace onirique::license::core
{

struct Status
{
    State state = State::unlicensed;
    std::string email;          // compte auquel la licence appartient (UTF-8)
    std::int64_t expiresAt = 0; // millisecondes depuis 1970 ; 0 = sans limite

    /** Jours entiers restants avant l'expiration (0 si sans limite ou expirée). */
    int daysLeft() const noexcept;
};

/** Millisecondes depuis 1970, à l'horloge de la machine. */
std::int64_t nowMs() noexcept;

/** Empreinte de cette machine, calculée comme dans ONIRIQUE CENTRAL : 40 premiers caractères
    hexadécimaux de SHA-256 ("onirique:" + identifiant de la machine en minuscules). L'identifiant
    est le MachineGuid de Windows, l'IOPlatformUUID d'un Mac. Vide si la machine n'en donne pas. */
std::string machineId();

/** Le fichier de licence de ce plugin sur cette machine (chemin en UTF-8). */
std::string licensePath (const std::string& productSlug);

/** Lit la licence de ce plugin et la vérifie avec la clé publique ONIRIQUE embarquée. */
Status read (const std::string& productSlug);

/** La même vérification, avec tout en paramètre (sert aux tests). `fileContents` : le contenu du
    fichier de licence ; `publicKey` : 32 octets. */
Status verify (const std::string& fileContents, const std::string& productSlug, const std::string& machine,
               const unsigned char* publicKey, std::int64_t now);

/** Ouvre ONIRIQUE CENTRAL si l'application est installée, sinon sa page de téléchargement. */
void openCentral();

//==============================================================================
/** Tout ce qu'un plugin doit garder de sa licence : un membre de son processeur.

    La licence est lue à la création, puis toutes les cinq secondes par un fil d'arrière-plan
    commun à toutes les instances du plugin : une activation faite dans ONIRIQUE CENTRAL est prise
    en compte sans recharger le plugin, même pendant un export. */
class Guard
{
public:
    explicit Guard (const std::string& productSlug);

    /** État de la licence sur cette machine (acheté, essai, démo). */
    Status status() const;

    /** Relit le fichier de licence tout de suite. Jamais depuis le thread audio. */
    void refresh();

    /** Thread audio, une fois le son du plugin calculé, sur toutes les sorties. `Sample` : float ou
        double. Rend vrai quand le bloc a été remplacé par du bruit. */
    template <typename Sample>
    bool process (Sample* const* channels, int numChannels, int numSamples, double sampleRate, bool offline) noexcept
    {
        noise.setState (currentState());
        return noise.process (channels, numChannels, numSamples, sampleRate, offline);
    }

private:
    struct Watch;

    State currentState() const noexcept;

    std::shared_ptr<Watch> watch;
    DemoNoise noise;
};

} // namespace onirique::license::core
