#pragma once

#include "LicenseCore.h"

#include <juce_core/juce_core.h>

//==============================================================================
//  La licence ONIRIQUE vue d'un plugin JUCE : les mêmes vérifications que
//  LicenseCore.h, avec les types de JUCE.
//==============================================================================
namespace onirique::license
{

struct Status
{
    State state = State::unlicensed;
    juce::String email;        // compte auquel la licence appartient
    juce::int64 expiresAt = 0; // millisecondes depuis 1970 ; 0 = sans limite
    bool renewal = false;      // licence authentique dont le bail est passé : ouvrir ONIRIQUE CENTRAL, connecté

    Status() = default;
    Status (const core::Status& status)
        : state (status.state), email (juce::String::fromUTF8 (status.email.c_str())), expiresAt ((juce::int64) status.expiresAt),
          renewal (status.renewal) {}

    /** Seul un plugin acheté exporte proprement : en essai ou en démo, l'export produit un bruit blanc. */
    bool exportsClean() const noexcept { return state == State::licensed; }

    /** Jours entiers restants avant l'expiration (0 si sans limite ou expirée). */
    int daysLeft() const noexcept;
};

/** Empreinte de cette machine, calculée comme dans ONIRIQUE CENTRAL. */
juce::String machineId();

/** Le fichier de licence de ce plugin sur cette machine. */
juce::File licenseFile (const juce::String& productSlug);

/** Lit la licence de ce plugin et la vérifie avec la clé publique ONIRIQUE embarquée. */
Status read (const juce::String& productSlug);

/** La même vérification, avec tout en paramètre (sert aux tests). `publicKey` : 32 octets. */
Status verify (const juce::File& file, const juce::String& productSlug, const juce::String& machine,
               const unsigned char* publicKey, juce::int64 nowMs);

} // namespace onirique::license
