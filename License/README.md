# Licence ONIRIQUE dans un plugin

Ce dossier est le même dans tous les plugins ONIRIQUE (avec ou sans JUCE, Windows et Mac). Il se copie tel
quel à la racine du dépôt du plugin, sous le nom `License/`, et ne se modifie qu'ici, dans
`plugin-license/` du dépôt ONIRIQUE CENTRAL.

Ce qu'il fait : il lit le fichier de licence qu'ONIRIQUE CENTRAL écrit sur la machine, vérifie sa
signature avec la clé publique du serveur, et en déduit trois états.

| État | Écoute | Export | Interface |
|---|---|---|---|
| Acheté, ou abonnement actif | intacte | intact | témoin « ACTIVÉ » |
| Essai gratuit en cours | intacte | bruit blanc | pastille « ESSAI · 5 J · ACTIVATION » |
| Pas de licence, ou essai terminé | 2 s de bruit toutes les 30 s | bruit blanc | pastille « MODE DÉMO · ACTIVATION », page d'activation à l'ouverture |

**Un plugin payant sans ce dossier est refusé à la publication** (panel admin et publication automatique).

## Brancher un plugin

`CMakeLists.txt`, après `juce_add_plugin` (le projet doit déclarer le langage C) :

```cmake
include(License/OniriqueLicense.cmake)
onirique_add_license(MonPlugin)
```

`PluginProcessor.h` — un membre public, avec l'identifiant du plugin dans ONIRIQUE CENTRAL :

```cpp
#include "License/LicenseGuard.h"

onirique::license::Guard license { "identifiant-du-plugin" };
```

`PluginProcessor.cpp` :

```cpp
void prepareToPlay (...)  { license.refresh(); ... }

void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    ...                                                             // le son du plugin
    license.process (buffer, getSampleRate(), isNonRealtime());     // en dernier, sur toutes les sorties
}
```

`PluginEditor` — un membre déclaré en dernier, créé et ajouté en dernier, placé dans `resized()` :

```cpp
#include "License/LicenseOverlay.h"

std::unique_ptr<onirique::license::Overlay> licenseOverlay;

// constructeur, après tous les autres composants
licenseOverlay = std::make_unique<onirique::license::Overlay> ("NOM DU PLUGIN", processor.license);
addAndMakeVisible (*licenseOverlay);
resized();

// resized() : zoom de l'interface (1 si elle ne se met pas à l'échelle), puis l'emplacement libre
// choisi pour la pastille, à la taille de base de l'interface
if (licenseOverlay != nullptr)
    licenseOverlay->fit (getLocalBounds(), scale, { 500, 10, 248, 26 });
```

## Plugin sans JUCE

La vérification et le bruit sont dans `LicenseCore.h`, qui ne dépend d'aucune bibliothèque (C++17) :

```cmake
include(License/OniriqueLicense.cmake)
onirique_add_license_core(MonPlugin)
```

```cpp
#include "License/LicenseCore.h"

onirique::license::core::Guard license { "identifiant-du-plugin" };   // membre du processeur

license.refresh();                                                    // à la préparation, hors thread audio
license.process (sorties, nombreDeCanaux, nombreDEchantillons, frequence, exportEnCours);   // en fin de traitement
```

La pastille et la page de la licence se dessinent avec l'interface du plugin, à partir de
`license.status()` (état, compte, jours restants) et de `core::openCentral()` pour le bouton. RAW
(page web) et TAMA-VOX (VSTGUI) en sont deux exemples.

## Vérifier

Le mode démo se force sur une machine qui a une licence avec la variable d'environnement
`ONIRIQUE_DEMO=1` (elle ne peut que retirer la licence, jamais en donner une).

`node tools/license-test.mjs <OniriqueLicenseTest>` (dépôt ONIRIQUE CENTRAL) rejoue la vérification sur des
licences signées comme par le serveur : valide, essai, expirée, autre machine, falsifiée.

`OniriqueLicenseTest`, dans le dépôt d'ONIRIQUE TUNER (`cmake --build build --target OniriqueLicenseTest`) :

```
OniriqueLicenseTest export <plugin.vst3>             son à l'écoute, BRUIT BLANC à l'export sans licence
OniriqueLicenseTest shot <plugin.vst3> <image.png>   une image de l'interface (Windows)
OniriqueLicenseTest noise                            le calendrier du bruit, état par état
```
