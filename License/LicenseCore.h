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
//  Une licence vaut pour une machine (son empreinte est dans le fichier signé),
//  et porte un bail : ONIRIQUE CENTRAL la renouvelle à chaque ouverture, en
//  ligne. Hors ligne, elle reste valable jusqu'à la fin du bail (90 jours) ;
//  une licence remboursée ou retirée cesse donc de valoir, même sans connexion.
//  L'heure prise en compte ne recule jamais : ni avant la date du fichier, ni
//  avant la plus récente déjà vue sur cette machine (une horloge remise en
//  arrière ne prolonge ni un essai, ni un bail).
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
    bool renewal = false;       // licence authentique, mais son bail est passé : ouvrir ONIRIQUE CENTRAL, connecté

    /** Jours entiers restants avant l'expiration (0 si sans limite ou expirée). */
    int daysLeft() const noexcept;
};

/** Millisecondes depuis 1970, à l'horloge de la machine. */
std::int64_t nowMs() noexcept;

/** Empreinte de cette machine, calculée comme dans ONIRIQUE CENTRAL : 40 premiers caractères
    hexadécimaux de SHA-256 ("onirique2:" + plusieurs identifiants en minuscules, séparés par |).
    Windows : MachineGuid | HardwareConfig\LastConfig (l'identifiant de la carte mère) | numéro de
    série du disque système ; Mac : IOPlatformUUID | IOPlatformSerialNumber. Vide si la machine
    n'en donne aucun. */
std::string machineId();

/** L'ancienne empreinte (MachineGuid ou IOPlatformUUID seul), celle des licences écrites avant le
    bail : encore acceptée jusqu'au 31 janvier 2027, le temps que chacun ouvre ONIRIQUE CENTRAL. */
std::string legacyMachineId();

/** Le fichier de licence de ce plugin sur cette machine (chemin en UTF-8). */
std::string licensePath (const std::string& productSlug);

/** Lit la licence de ce plugin et la vérifie avec la clé publique ONIRIQUE embarquée. */
Status read (const std::string& productSlug);

/** La même vérification, avec tout en paramètre (sert aux tests). `fileContents` : le contenu du
    fichier de licence ; `publicKey` : 32 octets ; `legacyMachine` : l'ancienne empreinte (ou vide) ;
    `now` : l'heure de confiance (jamais en arrière, voir trustedNow). */
Status verify (const std::string& fileContents, const std::string& productSlug, const std::string& machine,
               const std::string& legacyMachine, const unsigned char* publicKey, std::int64_t now);

Status verify (const std::string& fileContents, const std::string& productSlug, const std::string& machine,
               const unsigned char* publicKey, std::int64_t now);

/** L'heure de la machine, sans jamais revenir avant la plus récente déjà vue ici. */
std::int64_t trustedNow() noexcept;

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
        noise.setState (sealed() ? State::unlicensed : currentState());
        return noise.process (channels, numChannels, numSamples, sampleRate, offline);
    }

    /** Vrai quand la contre-vérification a trouvé un plugin qui se dit activé sans licence authentique. */
    bool sealed() const noexcept;

private:
    struct Watch;

    State currentState() const noexcept;

    std::shared_ptr<Watch> watch;
    DemoNoise noise;
};

} // namespace onirique::license::core
