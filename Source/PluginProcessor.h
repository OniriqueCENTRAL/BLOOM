#pragma once

#include <JuceHeader.h>
#include "BloomLicense.h"
#include "License/LicenseGuard.h"

using BloomFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
    juce::dsp::IIR::Coefficients<float>>;

// ================================================
//  Petit detecteur : mesure le niveau d'une zone de frequences
// ================================================
struct BandDetector
{
    BloomFilter highPass, lowPass;
    juce::AudioBuffer<float> scratch;   // copie filtree de la zone
    std::vector<float> bandRms;         // niveau de la zone, canal par canal
    float lastBandDb = -100.0f;         // niveau de la zone (en dB)
    float envelope = 0.0f;
    float attackCoef = 1.0f, releaseCoef = 1.0f;

    void prepare(const juce::dsp::ProcessSpec& spec, int blockSize, float lowHz, float highHz,
        float attackMs, float releaseMs);

    // Renvoie le niveau de la zone PAR RAPPORT au niveau global (en dB)
    float measureRelativeDb(const juce::AudioBuffer<float>& buffer, int start, int length, int numChannels);

    // Suit une valeur en douceur (vite pour monter, lentement pour redescendre)
    float follow(float target);
};

class BLOOMAudioProcessor : public juce::AudioProcessor
{
public:
    BLOOMAudioProcessor();
    ~BLOOMAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Le bypass de FL Studio pilote notre bouton BYPASS
    juce::AudioProcessorParameter* getBypassParameter() const override;

    // diagnostic : plus forte charge d'un bloc audio (1 = tout le temps disponible), lue par l'editeur
    std::atomic<float> maxBlockLoad { 0.0f };
    std::atomic<juce::int64> perfBusyTicks { 0 }, perfAvailTicks { 0 };
    std::atomic<int> perfBlocks { 0 }, perfOverBlocks { 0 }, perfLastBlockSize { 0 };

    // Tous les reglages du plugin
    juce::AudioProcessorValueTreeState parameters;

    // ================================================
    //  REGLAGES DE WEIGHT
    // ================================================
    static constexpr float weightMaxBoostDb = 10.0f;  // grave ajoute a fond (en dB)
    static constexpr float weightFrequency = 80.0f;  // zone de grave visee (en Hz)
    static constexpr float weightCompensation = 0.45f;  // 0 = aucune, 1 = totale
    static constexpr float weightCurve = 1.5f;   // 1 = lineaire, plus haut = debut plus doux
    static constexpr float monoBassFrequency = 100.0f; // en dessous, le grave se recentre avec WEIGHT

    static constexpr float weightPunchStartDb = 3.0f;   // a partir de quel coup de kick WEIGHT se retire
    static constexpr float weightPunchRangeDb = 6.0f;   // plus petit = reagit plus fort
    static constexpr float weightPunchDepth = 0.35f;  // de combien WEIGHT se retire sur le kick (0 = jamais)

    // ================================================
    //  REGLAGES DE SPACE
    // ================================================
    static constexpr float spaceMaxCutDb = 5.0f;   // creux dans la zone boueuse a fond (en dB)
    static constexpr float spaceFrequency = 300.0f; // centre de la zone boueuse (en Hz)
    static constexpr float spaceQ = 0.9f;   // largeur du creux
    static constexpr float spaceMaxWidth = 0.5f;   // elargissement stereo a fond (0 = aucun)
    static constexpr float spaceWidthFrequency = 300.0f; // au-dessus, la stereo s'elargit
    static constexpr float spaceCompensation = 0.3f;   // remonte un peu le volume perdu

    static constexpr float spaceMonoMinCorr = 0.0f;   // stereo dangereuse en mono : plus d'elargissement
    static constexpr float spaceMonoSafeCorr = 0.5f;   // stereo saine : elargissement complet autorise

    // ================================================
    //  REGLAGES DE PRESENCE
    // ================================================
    static constexpr float presenceMaxBoostDb = 6.0f;    // a fond (en dB)
    static constexpr float presenceFrequency = 2500.0f; // zone visee (en Hz)
    static constexpr float presenceQ = 0.8f;    // largeur de la bosse
    static constexpr float presenceCompensation = 0.4f;

    static constexpr float presenceDynamicStartDb = -10.0f; // zone deja forte : PRESENCE commence a se retirer
    static constexpr float presenceDynamicFullDb = -4.0f;  // zone criarde : retrait maximum
    static constexpr float presenceDynamicDepth = 0.6f;   // de combien PRESENCE peut se retirer

    // ================================================
    //  REGLAGES DE AIR (brillance soyeuse et dynamique)
    // ================================================
    static constexpr float airMaxBoostDb = 7.0f;     // ouverture du haut a fond (en dB)
    static constexpr float airFrequency = 12000.0f; // au-dessus, l'air s'ouvre (en Hz)
    static constexpr float airShelfQ = 0.5f;     // pente du filtre (plus petit = plus soyeux)
    static constexpr float airCompensation = 0.2f;

    static constexpr float airExciteLow = 4000.0f;  // zone d'ou viennent les harmoniques (debut)
    static constexpr float airExciteHigh = 9000.0f;  // zone d'ou viennent les harmoniques (fin)
    static constexpr float airHarmonicsFrom = 8000.0f;  // on ne garde que les harmoniques au-dessus
    static constexpr float airExciteMix = 0.35f;    // quantite de brillance fabriquee a fond

    static constexpr float airDynamicStartDb = -14.0f;   // aigus deja presents : AIR commence a se retirer
    static constexpr float airDynamicFullDb = -6.0f;    // aigus tres agressifs : retrait maximum
    static constexpr float airDynamicDepth = 0.7f;     // de combien AIR peut se retirer

    // Courbe commune a SPACE, PRESENCE, AIR et ENHANCE
    static constexpr float toneCurve = 1.5f;

    // ================================================
    //  REGLAGES DE WARMTH (la saturation douce du grave)
    // ================================================
    static constexpr float warmthBandFrequency = 120.0f;
    static constexpr float warmthMaxDrive = 2.5f;
    static constexpr float warmthMix = 0.5f;
    static constexpr float warmthAsymmetry = 0.25f;
    static constexpr float warmthSmoothness = 1000.0f;

    // ================================================
    //  REGLAGES DE ENHANCE (chaleur et densite de toute la prod)
    // ================================================
    static constexpr float enhanceGlueMaxDb = 12.0f;
    static constexpr float enhanceGlueRatio = 2.0f;
    static constexpr float enhanceGlueAttackMs = 30.0f;
    static constexpr float enhanceGlueReleaseMs = 150.0f;
    static constexpr float enhanceGlueMakeupDb = 3.0f;
    static constexpr float enhanceMaxDrive = 3.0f;
    static constexpr float enhanceTapeMix = 0.6f;
    static constexpr float enhanceTapeBias = 0.1f;
    static constexpr float enhancePushDb = 2.5f;
    static constexpr float enhanceCeiling = 0.98f;

    // ================================================
    //  REGLAGES DU LIMITEUR DE SECURITE (toujours actif)
    // ================================================
    static constexpr float limiterCeilingDb = -0.3f;  // plafond absolu en sortie de BLOOM (en dB)
    static constexpr float limiterLookaheadMs = 1.5f;   // anticipation des pics (en ms)
    static constexpr float limiterReleaseMs = 100.0f; // retour au volume normal apres un pic (en ms)

    // Fluidite du mouvement des boutons
    static constexpr double smoothingSeconds = 0.05;
    static constexpr int    subBlockSize = 32;

    // Licence ONIRIQUE CENTRAL, commune a tous les plugins : sans elle, bruit blanc a l'export et par salves a l'ecoute.
    onirique::license::Guard license { "bloom" };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void updateFilters(float weightAmount, float spaceAmount, float presenceAmount, float airAmount);
    void applyAirExciter(juce::AudioBuffer<float>& buffer, int start, int length, int numChannels, float airAmount);
    void applyWarmth(juce::AudioBuffer<float>& buffer, int start, int length, float warmthAmount);
    void applyStereo(juce::AudioBuffer<float>& buffer, int start, int length, float weightAmount, float spaceAmount);
    void applyEnhanceSaturation(juce::dsp::AudioBlock<float>& block, float startAmount, float endAmount);
    void delayDrySignal(int numSamples, int numChannels);
    void applyMixBypassDelta(juce::AudioBuffer<float>& buffer, int numSamples, int numChannels);
    void applySafetyLimiter(juce::AudioBuffer<float>& buffer, int numChannels);
    float getRaw(const char* id) const;
    bool getSwitch(const char* id) const;

    // WEIGHT
    BloomFilter weightFilter;
    BloomFilter subCleanFilter;
    juce::dsp::IIR::Filter<float> monoSideFilter;
    BandDetector weightDetector;
    float weightFastDb = -100.0f, weightSlowDb = -100.0f;
    float weightFastAttackCoef = 1.0f, weightFastReleaseCoef = 1.0f, weightSlowCoef = 1.0f;

    // SPACE, PRESENCE, AIR
    BloomFilter spaceFilter;
    BloomFilter presenceFilter;
    BloomFilter airFilter;
    juce::dsp::IIR::Filter<float> spaceSideFilter;
    float spaceSafety = 1.0f, spaceSafetyDownCoef = 1.0f, spaceSafetyUpCoef = 1.0f;
    BandDetector presenceDetector;
    BandDetector airDetector;
    BloomFilter airHarmonics;

    // WARMTH
    BloomFilter warmthLowBand;
    BloomFilter warmthHarmonicsHigh;
    BloomFilter warmthHarmonicsLow;
    juce::AudioBuffer<float> warmthBuffer;

    // ENHANCE
    juce::dsp::Compressor<float> glueCompressor;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    BloomFilter enhanceDcBlock;

    // MIX, BYPASS, DELTA : le son d'origine, retarde pour rester cale
    juce::AudioBuffer<float> dryBuffer;
    std::vector<std::vector<float>> dryDelay;
    int dryLatency = 0;
    int dryDelayPos = 0;

    // LIMITEUR DE SECURITE
    std::vector<std::vector<float>> limiterDelay;
    std::vector<float> limiterGainHistory;
    int limiterDelayPos = 0;
    int limiterHistoryPos = 0;
    int limiterLookahead = 1;
    float limiterEnvelope = 1.0f;
    float limiterAttackCoef = 1.0f;
    float limiterReleaseCoef = 1.0f;
    float limiterCeiling = 1.0f;

    // Mouvements fluides
    juce::SmoothedValue<float> smoothedWeight, smoothedSpace, smoothedPresence,
        smoothedAir, smoothedWarmth, smoothedEnhance,
        smoothedMix, smoothedDelta;

    juce::dsp::Gain<float> outputGain; // compensation automatique du volume

    float lastWeightAmt = -1.0f, lastSpaceAmt = -1.0f, lastPresenceAmt = -1.0f, lastAirAmt = -1.0f; // dernier reglage des filtres

    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BLOOMAudioProcessor)
};