#pragma once
// ============================================================================
//  BLOOM : petit journal de performances (diagnostic temporaire)
//  Note les temps de demarrage et les "a-coups" (images en retard), puis ecrit
//  le tout dans  Bureau\BLOOM\BLOOM_perf.txt  quand on ferme la fenetre du plugin.
//  A retirer (ou desactiver avec kEnabled = false) avant la version finale.
// ============================================================================
#include <JuceHeader.h>

#define BLOOM_PERF_CAT2(a, b) a##b
#define BLOOM_PERF_CAT(a, b) BLOOM_PERF_CAT2(a, b)
#define BLOOM_PERF_SCOPE(...) bloomperf::Scope BLOOM_PERF_CAT(bloomPerfScope_, __LINE__) { __VA_ARGS__ }

namespace bloomperf
{
    inline constexpr bool kEnabled = false;   // journal de diagnostic coupe (version finale)

    struct Log
    {
        juce::CriticalSection lock;
        juce::StringArray lines;
        double t0 = juce::Time::getMillisecondCounterHiRes();
    };

    // jamais detruit volontairement (pas d'objet JUCE "static" detruit au dechargement du plugin)
    inline Log& log()
    {
        static auto* l = new Log();
        return *l;
    }

    inline void resetClock()
    {
        if (!kEnabled) return;
        auto& l = log();
        const juce::ScopedLock sl(l.lock);
        l.t0 = juce::Time::getMillisecondCounterHiRes();
        l.lines.add("---- ouverture de la fenetre " + juce::Time::getCurrentTime().toString(true, true, true, true));
    }

    inline void mark(const juce::String& what)
    {
        if (!kEnabled) return;
        auto& l = log();
        const double now = juce::Time::getMillisecondCounterHiRes();
        const juce::ScopedLock sl(l.lock);
        if (l.lines.size() < 2000)
            l.lines.add(juce::String(now - l.t0, 1).paddedLeft(' ', 9) + " ms  " + what);
    }

    // mesure la duree d'un bout de code et la note si elle depasse `limitMs`
    struct Scope
    {
        const char* what; double limitMs; double t0;
        Scope(const char* w, double lim = 25.0) : what(w), limitMs(lim), t0(kEnabled ? juce::Time::getMillisecondCounterHiRes() : 0.0) {}
        ~Scope()
        {
            if (!kEnabled) return;
            const double d = juce::Time::getMillisecondCounterHiRes() - t0;
            if (d > limitMs)
                mark(juce::String(what) + " a pris " + juce::String(d, 1) + " ms");
        }
    };

    inline void flush()
    {
        if (!kEnabled) return;
        auto& l = log();
        juce::StringArray copy;
        {
            const juce::ScopedLock sl(l.lock);
            copy = l.lines;
            l.lines.clear();
        }
        if (copy.isEmpty()) return;
        auto dir = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("BLOOM");
        if (!dir.isDirectory())
            dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        dir.getChildFile("BLOOM_perf.txt").appendText(copy.joinIntoString("\n") + "\n");
    }
}
