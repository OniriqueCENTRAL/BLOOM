#pragma once
// ============================================================================
//  BLOOM : licence
//  La licence de BLOOM est celle d'ONIRIQUE CENTRAL, commune a tous les plugins
//  (dossier License/) : elle s'active depuis l'application, et sans elle un
//  bruit blanc coupe l'ecoute et remplace tout export.
//
//  BLOOM garde en revanche sa propre facon de la montrer : son etiquette en haut
//  a gauche et sa page « licence » dans la fiche. Ce fichier leur donne l'etat de
//  la licence commune, sous les noms que le reste du code utilisait deja.
// ============================================================================
#include <JuceHeader.h>
#include <functional>
#include "License/LicenseGuard.h"

namespace bloomlic
{
    struct Result
    {
        bool ok = true;
        juce::String message;
    };

    class License
    {
    public:
        // jamais detruit volontairement (pas d'objet JUCE "static" detruit au dechargement du plugin)
        static License& get()
        {
            static auto* instance = new License();
            return *instance;
        }

        void startup() { guard.refresh(); }

        /** Relit le fichier de licence ecrit par ONIRIQUE CENTRAL. */
        void refresh() { guard.refresh(); }

        onirique::license::Status status() const
        {
            if (pictured() != nullptr)
                return *pictured();
            return guard.status();
        }

        /** Pour l'outil qui photographie l'interface (tools/snapshot.cpp) : l'etat a montrer, a la place du vrai.
            Ne change que ce que l'interface affiche : le son, lui, suit la licence du processeur, pas celle-ci. */
        static void showInPictures(onirique::license::State state, const juce::String& email, int days)
        {
            auto* s = new onirique::license::Status();
            s->state = state;
            s->email = email;
            s->expiresAt = days > 0 ? juce::Time::currentTimeMillis() + (juce::int64) days * 86400000 + 3600000 : 0;
            pictured() = s;
        }

        /** Achete ou en essai : BLOOM n'est pas en mode demo. */
        bool isLicensed() const { return status().state != onirique::license::State::unlicensed; }
        bool isTrial() const { return status().state == onirique::license::State::trial; }
        int trialDaysLeft() const { return status().daysLeft(); }

        /** Change des que l'etat de la licence change (pour redessiner ce qui la montre). */
        int stateCode() const
        {
            const auto s = status();
            return (int) s.state * 1000 + s.daysLeft();
        }

        /** Le compte auquel la licence appartient. */
        juce::String getMaskedKey() const { return status().email; }

        /** Ouvre l'application ONIRIQUE CENTRAL, d'ou BLOOM s'active (ou la page pour l'installer). */
        void openCentral() { onirique::license::core::openCentral(); }

    private:
        static onirique::license::Status*& pictured()
        {
            static onirique::license::Status* status = nullptr;
            return status;
        }

        License() = default;
        onirique::license::Guard guard { "bloom" };
    };
}
