#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BloomScene.h"
#include "BloomLicense.h"

// Met a jour un filtre SANS allocation memoire (important : on est sur le fil audio)
template <typename FilterType>
static void assignArray(FilterType& filter, const std::array<float, 6>& c)
{
    if (filter.state == nullptr)
        filter.state = new juce::dsp::IIR::Coefficients<float>(c);
    else
        *filter.state = c;
}

// tanh rapide (approximation de Pade, tres proche du vrai tanh, ~10x plus rapide)
static inline float fastTanh(float x)
{
    x = juce::jlimit(-3.0f, 3.0f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Petit outil : met a jour un filtre avec de nouveaux reglages
template <typename FilterType>
static void assignCoefficients(FilterType& filter, juce::dsp::IIR::Coefficients<float>::Ptr newCoeffs)
{
    if (filter.state == nullptr)
        filter.state = newCoeffs;
    else
        *filter.state = *newCoeffs;
}

// ================================================
//  BandDetector
// ================================================
void BandDetector::prepare(const juce::dsp::ProcessSpec& spec, int blockSize, float lowHz, float highHz,
    float attackMs, float releaseMs)
{
    assignCoefficients(highPass, juce::dsp::IIR::Coefficients<float>::makeHighPass(spec.sampleRate, lowHz, 0.707f));
    assignCoefficients(lowPass, juce::dsp::IIR::Coefficients<float>::makeLowPass(spec.sampleRate, highHz, 0.707f));
    highPass.prepare(spec);
    highPass.reset();
    lowPass.prepare(spec);
    lowPass.reset();

    scratch.setSize((int)spec.numChannels, blockSize);
    bandRms.assign(spec.numChannels, 0.0f);
    lastBandDb = -100.0f;
    envelope = 0.0f;

    const float blockSeconds = (float)blockSize / (float)spec.sampleRate;
    attackCoef = 1.0f - std::exp(-blockSeconds / (attackMs / 1000.0f));
    releaseCoef = 1.0f - std::exp(-blockSeconds / (releaseMs / 1000.0f));
}

float BandDetector::measureRelativeDb(const juce::AudioBuffer<float>& buffer, int start, int length, int numChannels)
{
    const int chs = juce::jmin(numChannels, scratch.getNumChannels());
    float fullPower = 0.0f, bandPower = 0.0f;

    for (int ch = 0; ch < chs; ++ch)
    {
        scratch.copyFrom(ch, 0, buffer, ch, start, length);
        const float r = buffer.getRMSLevel(ch, start, length);
        fullPower += r * r;
    }

    juce::dsp::AudioBlock<float> block = juce::dsp::AudioBlock<float>(scratch)
        .getSubBlock(0, (size_t)length)
        .getSubsetChannelBlock(0, (size_t)chs);
    juce::dsp::ProcessContextReplacing<float> context(block);
    highPass.process(context);
    lowPass.process(context);

    for (int ch = 0; ch < chs; ++ch)
    {
        const float r = scratch.getRMSLevel(ch, 0, length);
        bandRms[(size_t)ch] = r;
        bandPower += r * r;
    }

    const float n = (float)juce::jmax(1, chs);
    lastBandDb = juce::Decibels::gainToDecibels(std::sqrt(bandPower / n));
    const float fullDb = juce::Decibels::gainToDecibels(std::sqrt(fullPower / n));

    return (fullDb < -80.0f) ? -100.0f : lastBandDb - fullDb;
}

float BandDetector::follow(float target)
{
    const float coef = target > envelope ? attackCoef : releaseCoef;
    envelope += (target - envelope) * coef;
    return envelope;
}

// ================================================
//  BLOOMAudioProcessor
// ================================================
BLOOMAudioProcessor::BLOOMAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    parameters(*this, nullptr, "BLOOM", createParameters())
{
    // reveille la carte graphique en arriere-plan des le chargement du plugin,
    // pour que la fenetre s'ouvre directement sans ecran noir
    bloomPrewarmGraphics();

    // licence : lue sur le disque (et reverifiee en ligne de temps en temps)
    bloomlic::License::get().startup();
}

BLOOMAudioProcessor::~BLOOMAudioProcessor()
{
    bloomWaitGraphicsWarmup();
}

// ===== Les reglages du plugin, avec des valeurs lisibles =====
juce::AudioProcessorValueTreeState::ParameterLayout BLOOMAudioProcessor::createParameters()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    using TextFunction = std::function<juce::String(float, int)>;

    auto addKnob = [&layout](const char* id, const char* name, float defaultValue, TextFunction toText)
        {
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{ id, 1 }, name,
                juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), defaultValue,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction(toText)));
        };

    auto dbText = [](float db) -> juce::String
        {
            if (std::abs(db) < 0.05f)
                return "0 dB";
            return juce::String(db > 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
        };

    auto percentText = [](float value, int) { return juce::String(juce::roundToInt(value)) + " %"; };

    addKnob("weight", "Weight", 0.0f, [dbText](float v, int)
        { return dbText(std::pow(v / 100.0f, weightCurve) * weightMaxBoostDb); });

    addKnob("space", "Space", 0.0f, [dbText](float v, int)
        { return dbText(-std::pow(v / 100.0f, toneCurve) * spaceMaxCutDb); });

    addKnob("presence", "Presence", 0.0f, [dbText](float v, int)
        { return dbText(std::pow(v / 100.0f, toneCurve) * presenceMaxBoostDb); });

    addKnob("air", "Air", 0.0f, [dbText](float v, int)
        { return dbText(std::pow(v / 100.0f, toneCurve) * airMaxBoostDb); });

    addKnob("warmth", "Warmth", 0.0f, percentText);
    addKnob("enhance", "Enhance", 0.0f, percentText);
    addKnob("mix", "Mix", 100.0f, percentText);

    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{ "bypass", 1 }, "Bypass", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{ "delta", 1 }, "Delta", false));

    return layout;
}

juce::AudioProcessorParameter* BLOOMAudioProcessor::getBypassParameter() const
{
    return parameters.getParameter("bypass");
}

float BLOOMAudioProcessor::getRaw(const char* id) const
{
    return parameters.getRawParameterValue(id)->load() / 100.0f; // de 0 a 1
}

bool BLOOMAudioProcessor::getSwitch(const char* id) const
{
    return parameters.getRawParameterValue(id)->load() > 0.5f;
}

// ===== Mise a jour de tous les filtres =====
void BLOOMAudioProcessor::updateFilters(float weightAmount, float spaceAmount, float presenceAmount, float airAmount)
{
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    auto dbToGain = [](float db) { return juce::Decibels::decibelsToGain(db); };
    auto changed = [](float& last, float now) { if (std::abs(now - last) < 1.0e-5f) return false; last = now; return true; };

    // on ne recalcule un filtre que si son reglage a bouge (et sans jamais allouer de memoire)
    if (changed(lastWeightAmt, weightAmount))
    {
        assignArray(weightFilter, AC::makeLowShelf(currentSampleRate, weightFrequency, 0.7f, dbToGain(weightAmount * weightMaxBoostDb)));
        assignArray(subCleanFilter, AC::makeHighPass(currentSampleRate, juce::jmap(weightAmount, 10.0f, 28.0f), 0.707f));
    }
    if (changed(lastSpaceAmt, spaceAmount))
        assignArray(spaceFilter, AC::makePeakFilter(currentSampleRate, spaceFrequency, spaceQ, dbToGain(-spaceAmount * spaceMaxCutDb)));
    if (changed(lastPresenceAmt, presenceAmount))
        assignArray(presenceFilter, AC::makePeakFilter(currentSampleRate, presenceFrequency, presenceQ, dbToGain(presenceAmount * presenceMaxBoostDb)));
    if (changed(lastAirAmt, airAmount))
        assignArray(airFilter, AC::makeHighShelf(currentSampleRate, airFrequency, airShelfQ, dbToGain(airAmount * airMaxBoostDb)));
}

// ===== AIR : fabrique de la brillance a partir de la zone 4-9 kHz =====
void BLOOMAudioProcessor::applyAirExciter(juce::AudioBuffer<float>& buffer, int start, int length,
    int numChannels, float airAmount)
{
    auto& scratch = airDetector.scratch; // contient deja la zone 4-9 kHz (mesuree plus tot)
    const int chs = juce::jmin(numChannels, scratch.getNumChannels());

    for (int ch = 0; ch < chs; ++ch)
    {
        auto* data = scratch.getWritePointer(ch);
        const float norm = 1.0f / (airDetector.bandRms[(size_t)ch] + 1.0e-5f);

        for (int i = 0; i < length; ++i)
            data[i] = data[i] * data[i] * norm; // harmoniques propres, une octave au-dessus
    }

    juce::dsp::AudioBlock<float> block = juce::dsp::AudioBlock<float>(scratch)
        .getSubBlock(0, (size_t)length)
        .getSubsetChannelBlock(0, (size_t)chs);
    juce::dsp::ProcessContextReplacing<float> context(block);
    airHarmonics.process(context); // on ne garde que la brillance au-dessus de 8 kHz

    if (airAmount > 0.0001f)
        for (int ch = 0; ch < chs; ++ch)
            buffer.addFrom(ch, start, scratch, ch, 0, length, airAmount * airExciteMix);
}

// ===== WARMTH : saturation douce, seulement sur le grave =====
void BLOOMAudioProcessor::applyWarmth(juce::AudioBuffer<float>& buffer, int start, int length, float warmthAmount)
{
    const int numChannels = juce::jmin(buffer.getNumChannels(), warmthBuffer.getNumChannels());

    for (int ch = 0; ch < numChannels; ++ch)
        warmthBuffer.copyFrom(ch, 0, buffer, ch, start, length);

    juce::dsp::AudioBlock<float> warmthBlock = juce::dsp::AudioBlock<float>(warmthBuffer)
        .getSubBlock(0, (size_t)length)
        .getSubsetChannelBlock(0, (size_t)numChannels);
    juce::dsp::ProcessContextReplacing<float> context(warmthBlock);

    warmthLowBand.process(context);

    const float drive = juce::jmap(warmthAmount, 1.0f, warmthMaxDrive);
    const float bias = warmthAsymmetry;
    const float tanhBias = std::tanh(drive * bias);
    const float slope = drive * (1.0f - tanhBias * tanhBias);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = warmthBuffer.getWritePointer(ch);

        for (int i = 0; i < length; ++i)
        {
            const float x = data[i];
            const float saturated = (fastTanh(drive * (x + bias)) - tanhBias) / slope;
            data[i] = saturated - x;
        }
    }

    warmthHarmonicsHigh.process(context);
    warmthHarmonicsLow.process(context);

    const float mix = warmthAmount * warmthMix;

    for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, start, warmthBuffer, ch, 0, length, mix);
}

// ===== Stereo : grave recentre (WEIGHT) + mix elargi sans risque en mono (SPACE) =====
void BLOOMAudioProcessor::applyStereo(juce::AudioBuffer<float>& buffer, int start, int length,
    float weightAmount, float spaceAmount)
{
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);

    // 1) La stereo est-elle saine ? (1 = parfaitement compatible mono, 0 ou moins = dangereux)
    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
    for (int i = start; i < start + length; ++i)
    {
        sumLR += (double)left[i] * right[i];
        sumLL += (double)left[i] * left[i];
        sumRR += (double)right[i] * right[i];
    }

    if (sumLL > 1.0e-9 && sumRR > 1.0e-9)
    {
        const float correlation = (float)(sumLR / std::sqrt(sumLL * sumRR));
        const float target = juce::jlimit(0.0f, 1.0f,
            (correlation - spaceMonoMinCorr) / (spaceMonoSafeCorr - spaceMonoMinCorr));
        const float coef = target < spaceSafety ? spaceSafetyDownCoef : spaceSafetyUpCoef;
        spaceSafety += (target - spaceSafety) * coef;
    }

    const float width = spaceAmount * spaceMaxWidth * spaceSafety;

    // 2) On recentre le grave et on elargit le reste
    for (int i = start; i < start + length; ++i)
    {
        const float mid = (left[i] + right[i]) * 0.5f;
        const float side = (left[i] - right[i]) * 0.5f;

        const float sideLow = monoSideFilter.processSample(side);
        const float sideHigh = spaceSideFilter.processSample(side);

        float newSide = side;
        newSide -= weightAmount * sideLow;
        newSide += width * sideHigh;

        left[i] = mid + newSide;
        right[i] = mid - newSide;
    }
}

// ===== ENHANCE : saturation bande + clipper doux, en suréchantillonnage x4 =====
void BLOOMAudioProcessor::applyEnhanceSaturation(juce::dsp::AudioBlock<float>& block, float startAmount, float endAmount)
{
    auto upBlock = oversampling->processSamplesUp(block);

    const int numUp = (int)upBlock.getNumSamples();
    const int numCh = (int)upBlock.getNumChannels();

    if (startAmount <= 0.0001f && endAmount <= 0.0001f)
    {
        oversampling->processSamplesDown(block);
        return;
    }

    // valeurs au debut et a la fin du bloc, puis interpolation lineaire (au lieu de pow/tanh a chaque echantillon)
    auto consts = [this](float e, float& gain, float& drive, float& tb, float& slope)
    {
        gain = juce::Decibels::decibelsToGain(e * (enhanceGlueMakeupDb + enhancePushDb));
        drive = juce::jmap(e, 1.0f, enhanceMaxDrive);
        tb = std::tanh(drive * enhanceTapeBias);
        slope = drive * (1.0f - tb * tb);
    };
    float g0, d0, t0, s0, g1, d1, t1, s1;
    consts(startAmount, g0, d0, t0, s0);
    consts(endAmount, g1, d1, t1, s1);
    const float invCeil = 1.0f / enhanceCeiling;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* data = upBlock.getChannelPointer((size_t)ch);

        for (int i = 0; i < numUp; ++i)
        {
            const float f = (float)i / (float)numUp;
            const float e = startAmount + f * (endAmount - startAmount);

            if (e <= 0.0001f)
                continue;

            const float gain = g0 + f * (g1 - g0), drive = d0 + f * (d1 - d0);
            const float tanhBias = t0 + f * (t1 - t0), slope = s0 + f * (s1 - s0);

            float x = data[i] * gain;
            const float tape = (fastTanh(drive * (x + enhanceTapeBias)) - tanhBias) / slope;
            x += e * enhanceTapeMix * (tape - x);

            const float clipped = enhanceCeiling * fastTanh(x * invCeil);
            x += e * (clipped - x);

            data[i] = x;
        }
    }

    oversampling->processSamplesDown(block);
}

// ===== Retarde le son d'origine pour qu'il reste cale avec le son traite =====
void BLOOMAudioProcessor::delayDrySignal(int numSamples, int numChannels)
{
    if (dryLatency <= 0)
        return;

    int pos = dryDelayPos;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& ring = dryDelay[(size_t)ch];
        auto* data = dryBuffer.getWritePointer(ch);
        pos = dryDelayPos;

        for (int i = 0; i < numSamples; ++i)
        {
            const float delayed = ring[(size_t)pos];
            ring[(size_t)pos] = data[i];
            data[i] = delayed;
            pos = (pos + 1) % dryLatency;
        }
    }

    dryDelayPos = pos;
}

// ===== MIX, BYPASS et DELTA =====
void BLOOMAudioProcessor::applyMixBypassDelta(juce::AudioBuffer<float>& buffer, int numSamples, int numChannels)
{
    smoothedMix.setTargetValue(getSwitch("bypass") ? 0.0f : getRaw("mix"));
    smoothedDelta.setTargetValue(getSwitch("delta") ? 1.0f : 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = smoothedMix.getNextValue();
        const float delta = smoothedDelta.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float wet = buffer.getSample(ch, i);
            const float dry = dryBuffer.getSample(ch, i);

            const float mixed = dry + mix * (wet - dry); // MIX (et BYPASS = mix a 0)
            buffer.setSample(ch, i, mixed - delta * dry); // DELTA : on retire l'original
        }
    }
}

// ===== LIMITEUR DE SECURITE : rien ne depasse jamais le plafond =====
void BLOOMAudioProcessor::applySafetyLimiter(juce::AudioBuffer<float>& buffer, int numChannels)
{
    const int numSamples = buffer.getNumSamples();
    const int historySize = (int)limiterGainHistory.size();

    for (int i = 0; i < numSamples; ++i)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            peak = juce::jmax(peak, std::abs(buffer.getSample(ch, i)));

        const float required = peak > limiterCeiling ? limiterCeiling / peak : 1.0f;
        limiterGainHistory[(size_t)limiterHistoryPos] = required;
        limiterHistoryPos = (limiterHistoryPos + 1) % historySize;

        float targetGain = 1.0f;
        for (float g : limiterGainHistory)
            targetGain = juce::jmin(targetGain, g);

        const float coef = targetGain < limiterEnvelope ? limiterAttackCoef : limiterReleaseCoef;
        limiterEnvelope += (targetGain - limiterEnvelope) * coef;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& delay = limiterDelay[(size_t)ch];
            const float delayed = delay[(size_t)limiterDelayPos];
            delay[(size_t)limiterDelayPos] = buffer.getSample(ch, i);

            const float out = juce::jlimit(-limiterCeiling, limiterCeiling, delayed * limiterEnvelope);
            buffer.setSample(ch, i, out);
        }

        limiterDelayPos = (limiterDelayPos + 1) % limiterLookahead;
    }
}

void BLOOMAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = (juce::uint32)getTotalNumOutputChannels();

    // Mouvements fluides
    auto initSmooth = [sampleRate](juce::SmoothedValue<float>& v, float now)
        {
            v.reset(sampleRate, smoothingSeconds);
            v.setCurrentAndTargetValue(now);
        };
    initSmooth(smoothedWeight, getRaw("weight"));
    initSmooth(smoothedSpace, getRaw("space"));
    initSmooth(smoothedPresence, getRaw("presence"));
    initSmooth(smoothedAir, getRaw("air"));
    initSmooth(smoothedWarmth, getRaw("warmth"));
    initSmooth(smoothedEnhance, getRaw("enhance"));
    initSmooth(smoothedMix, getSwitch("bypass") ? 0.0f : getRaw("mix"));
    initSmooth(smoothedDelta, getSwitch("delta") ? 1.0f : 0.0f);

    // Filtres de tonalite (on force le recalcul)
    lastWeightAmt = lastSpaceAmt = lastPresenceAmt = lastAirAmt = -1.0f;
    updateFilters(std::pow(getRaw("weight"), weightCurve),
        std::pow(getRaw("space"), toneCurve),
        std::pow(getRaw("presence"), toneCurve),
        std::pow(getRaw("air"), toneCurve));

    for (auto* f : { &weightFilter, &subCleanFilter, &spaceFilter, &presenceFilter, &airFilter })
    {
        f->prepare(spec);
        f->reset();
    }

    // Detecteurs (WEIGHT intelligent, PRESENCE et AIR dynamiques)
    weightDetector.prepare(spec, subBlockSize, 20.0f, 120.0f, 3.0f, 80.0f);
    presenceDetector.prepare(spec, subBlockSize, 1800.0f, 4500.0f, 5.0f, 100.0f);
    airDetector.prepare(spec, subBlockSize, airExciteLow, airExciteHigh, 5.0f, 80.0f);

    const float blockSeconds = (float)subBlockSize / (float)sampleRate;
    weightFastAttackCoef = 1.0f - std::exp(-blockSeconds / 0.001f);
    weightFastReleaseCoef = 1.0f - std::exp(-blockSeconds / 0.030f);
    weightSlowCoef = 1.0f - std::exp(-blockSeconds / 0.150f);
    weightFastDb = weightSlowDb = -100.0f;

    spaceSafety = 1.0f;
    spaceSafetyDownCoef = 1.0f - std::exp(-blockSeconds / 0.010f);
    spaceSafetyUpCoef = 1.0f - std::exp(-blockSeconds / 0.200f);

    // Filtres stereo
    juce::dsp::ProcessSpec monoSpec = spec;
    monoSpec.numChannels = 1;

    monoSideFilter.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass(
        sampleRate, monoBassFrequency, 0.707f);
    monoSideFilter.prepare(monoSpec);
    monoSideFilter.reset();

    spaceSideFilter.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(
        sampleRate, spaceWidthFrequency, 0.707f);
    spaceSideFilter.prepare(monoSpec);
    spaceSideFilter.reset();

    // AIR, WARMTH et ENHANCE
    assignCoefficients(airHarmonics,
        juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, airHarmonicsFrom, 0.707f));
    assignCoefficients(warmthLowBand,
        juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, warmthBandFrequency, 0.707f));
    assignCoefficients(warmthHarmonicsHigh,
        juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 60.0f, 0.707f));
    assignCoefficients(warmthHarmonicsLow,
        juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, warmthSmoothness, 0.707f));
    assignCoefficients(enhanceDcBlock,
        juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 12.0f, 0.707f));

    for (auto* f : { &airHarmonics, &warmthLowBand, &warmthHarmonicsHigh, &warmthHarmonicsLow, &enhanceDcBlock })
    {
        f->prepare(spec);
        f->reset();
    }

    warmthBuffer.setSize((int)spec.numChannels, subBlockSize);

    // ENHANCE : compression de colle
    glueCompressor.prepare(spec);
    glueCompressor.setAttack(enhanceGlueAttackMs);
    glueCompressor.setRelease(enhanceGlueReleaseMs);
    glueCompressor.setThreshold(0.0f);
    glueCompressor.setRatio(1.0f);
    glueCompressor.reset();

    // ENHANCE : suréchantillonnage x4 (2 etapes de x2)
    oversampling = std::make_unique<juce::dsp::Oversampling<float>>(
        spec.numChannels, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    // marge de securite : certains hotes envoient des blocs plus gros que annonce
    const int safeBlock = juce::jmax(samplesPerBlock, 16384);
    oversampling->initProcessing((size_t)safeBlock);
    oversampling->reset();

    // MIX, BYPASS, DELTA : le son d'origine retarde de la meme latence que le son traite
    dryLatency = (int)std::round(oversampling->getLatencyInSamples());
    dryBuffer.setSize((int)spec.numChannels, safeBlock);
    dryDelay.assign(spec.numChannels, std::vector<float>((size_t)juce::jmax(1, dryLatency), 0.0f));
    dryDelayPos = 0;

    // LIMITEUR DE SECURITE
    limiterLookahead = juce::jmax(1, (int)std::round(sampleRate * limiterLookaheadMs / 1000.0));
    limiterDelay.assign(spec.numChannels, std::vector<float>((size_t)limiterLookahead, 0.0f));
    limiterGainHistory.assign((size_t)limiterLookahead + 1, 1.0f);
    limiterDelayPos = 0;
    limiterHistoryPos = 0;
    limiterEnvelope = 1.0f;
    limiterCeiling = juce::Decibels::decibelsToGain(limiterCeilingDb);
    limiterAttackCoef = 1.0f - std::exp(-5.0f / (float)limiterLookahead);
    limiterReleaseCoef = 1.0f - std::exp(-1.0f / (float)(sampleRate * limiterReleaseMs / 1000.0));

    // On previent FL Studio de la latence totale, pour qu'il la compense
    setLatencySamples(dryLatency + limiterLookahead);

    // Compensation du volume
    outputGain.prepare(spec);
    outputGain.setRampDurationSeconds(smoothingSeconds);
    outputGain.reset();

    license.refresh();   // les DAW repassent par ici juste avant un export
}

void BLOOMAudioProcessor::releaseResources() {}

bool BLOOMAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    return mainOut == layouts.getMainInputChannelSet();
}

// ===== Le traitement du son =====
void BLOOMAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto perfStart = juce::Time::getHighResolutionTicks(); // diagnostic : duree du bloc
    struct PerfEnd
    {
        BLOOMAudioProcessor& self; juce::int64 start; int n; double sr;
        ~PerfEnd()
        {
            const juce::int64 busy = juce::Time::getHighResolutionTicks() - start;
            const double secs = juce::Time::highResolutionTicksToSeconds(busy);
            const double avail = juce::jmax(1.0e-6, (double) n / sr);
            const float load = (float) (secs / avail);
            self.perfBusyTicks.fetch_add(busy);
            self.perfAvailTicks.fetch_add((juce::int64) (avail * (double) juce::Time::getHighResolutionTicksPerSecond()));
            self.perfBlocks.fetch_add(1);
            if (load > 1.0f) self.perfOverBlocks.fetch_add(1);
            self.perfLastBlockSize.store(n);
            float prev = self.maxBlockLoad.load();
            while (load > prev && !self.maxBlockLoad.compare_exchange_weak(prev, load)) {}
        }
    } perfEnd { *this, perfStart, buffer.getNumSamples(), currentSampleRate };

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), getTotalNumOutputChannels());
    const bool isStereo = numChannels >= 2;

    // 0) On garde une copie du son d'origine (pour MIX, BYPASS et DELTA)
    if (dryBuffer.getNumSamples() < numSamples)
        dryBuffer.setSize(dryBuffer.getNumChannels(), numSamples, false, false, true);

    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    delayDrySignal(numSamples, numChannels);

    smoothedWeight.setTargetValue(getRaw("weight"));
    smoothedSpace.setTargetValue(getRaw("space"));
    smoothedPresence.setTargetValue(getRaw("presence"));
    smoothedAir.setTargetValue(getRaw("air"));
    smoothedWarmth.setTargetValue(getRaw("warmth"));
    smoothedEnhance.setTargetValue(getRaw("enhance"));

    juce::dsp::AudioBlock<float> fullBlock = juce::dsp::AudioBlock<float>(buffer)
        .getSubsetChannelBlock(0, (size_t)numChannels);

    // On traite le son par petits morceaux pour que tout bouge en douceur
    for (int start = 0; start < numSamples; start += subBlockSize)
    {
        const int length = juce::jmin(subBlockSize, numSamples - start);

        const float weightKnob = std::pow(smoothedWeight.skip(length), weightCurve);
        const float spaceAmount = std::pow(smoothedSpace.skip(length), toneCurve);
        const float presenceKnob = std::pow(smoothedPresence.skip(length), toneCurve);
        const float airKnob = std::pow(smoothedAir.skip(length), toneCurve);
        const float warmthAmount = smoothedWarmth.skip(length);

        // WEIGHT intelligent : se retire un peu quand le kick frappe
        weightDetector.measureRelativeDb(buffer, start, length, numChannels);
        const float lowDb = weightDetector.lastBandDb;
        weightFastDb += (lowDb - weightFastDb) * (lowDb > weightFastDb ? weightFastAttackCoef : weightFastReleaseCoef);
        weightSlowDb += (lowDb - weightSlowDb) * weightSlowCoef;
        const float punchTarget = (weightSlowDb < -60.0f) ? 0.0f
            : juce::jlimit(0.0f, 1.0f, (weightFastDb - weightSlowDb - weightPunchStartDb) / weightPunchRangeDb);
        const float weightAmount = weightKnob * (1.0f - weightPunchDepth * weightDetector.follow(punchTarget));

        // PRESENCE dynamique : se calme quand ca devient criard
        const float presenceRelDb = presenceDetector.measureRelativeDb(buffer, start, length, numChannels);
        const float presenceTarget = juce::jlimit(0.0f, 1.0f,
            (presenceRelDb - presenceDynamicStartDb) / (presenceDynamicFullDb - presenceDynamicStartDb));
        const float presenceAmount = presenceKnob * (1.0f - presenceDynamicDepth * presenceDetector.follow(presenceTarget));

        // AIR dynamique : se retire sur les sons sifflants
        const float airRelDb = airDetector.measureRelativeDb(buffer, start, length, numChannels);
        const float airTarget = juce::jlimit(0.0f, 1.0f,
            (airRelDb - airDynamicStartDb) / (airDynamicFullDb - airDynamicStartDb));
        const float airAmount = airKnob * (1.0f - airDynamicDepth * airDetector.follow(airTarget));

        updateFilters(weightAmount, spaceAmount, presenceAmount, airAmount);

        auto block = fullBlock.getSubBlock((size_t)start, (size_t)length);
        juce::dsp::ProcessContextReplacing<float> context(block);

        subCleanFilter.process(context);  // 1) nettoyage de l'infra-grave
        weightFilter.process(context);    // 2) WEIGHT
        spaceFilter.process(context);     // 3) SPACE (creux)
        presenceFilter.process(context);  // 4) PRESENCE
        airFilter.process(context);       // 5) AIR (ouverture)

        applyAirExciter(buffer, start, length, numChannels, airAmount); // 6) AIR (brillance fabriquee)
        applyWarmth(buffer, start, length, warmthAmount);               // 7) WARMTH

        if (isStereo)
            applyStereo(buffer, start, length, weightAmount, spaceAmount); // 8) stereo
    }

    // 9) Compensation du volume : chaque bouton corrige sa part
    const float w = std::pow(smoothedWeight.getCurrentValue(), weightCurve);
    const float s = std::pow(smoothedSpace.getCurrentValue(), toneCurve);
    const float p = std::pow(smoothedPresence.getCurrentValue(), toneCurve);
    const float a = std::pow(smoothedAir.getCurrentValue(), toneCurve);

    const float compensationDb = -w * weightMaxBoostDb * weightCompensation
        + s * spaceMaxCutDb * spaceCompensation
        - p * presenceMaxBoostDb * presenceCompensation
        - a * airMaxBoostDb * airCompensation;

    outputGain.setGainDecibels(compensationDb);

    juce::dsp::ProcessContextReplacing<float> fullContext(fullBlock);
    outputGain.process(fullContext);

    // 10) ENHANCE : colle, puis saturation et clipper
    const float enhanceStart = std::pow(smoothedEnhance.getCurrentValue(), toneCurve);
    const float enhanceEnd = std::pow(smoothedEnhance.skip(numSamples), toneCurve);

    glueCompressor.setThreshold(-enhanceEnd * enhanceGlueMaxDb);
    glueCompressor.setRatio(1.0f + enhanceEnd * (enhanceGlueRatio - 1.0f));
    glueCompressor.process(fullContext);

    applyEnhanceSaturation(fullBlock, enhanceStart, enhanceEnd);
    enhanceDcBlock.process(fullContext);

    // 11) MIX, BYPASS et DELTA
    applyMixBypassDelta(buffer, numSamples, numChannels);

    // 12) LIMITEUR DE SECURITE
    applySafetyLimiter(buffer, numChannels);

    // 13) LICENCE : sans elle, le bloc est remplace par du bruit (a l'export, et par salves a l'ecoute)
    license.process(buffer, getSampleRate(), isNonRealtime());
}

// ===== Le reste (infos pour FL Studio) =====
juce::AudioProcessorEditor* BLOOMAudioProcessor::createEditor() { return new BLOOMAudioProcessorEditor(*this); }
bool BLOOMAudioProcessor::hasEditor() const { return true; }

const juce::String BLOOMAudioProcessor::getName() const { return JucePlugin_Name; }
bool BLOOMAudioProcessor::acceptsMidi() const { return false; }
bool BLOOMAudioProcessor::producesMidi() const { return false; }
bool BLOOMAudioProcessor::isMidiEffect() const { return false; }
double BLOOMAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int BLOOMAudioProcessor::getNumPrograms() { return 1; }
int BLOOMAudioProcessor::getCurrentProgram() { return 0; }
void BLOOMAudioProcessor::setCurrentProgram(int) {}
const juce::String BLOOMAudioProcessor::getProgramName(int) { return {}; }
void BLOOMAudioProcessor::changeProgramName(int, const juce::String&) {}

// Sauvegarde de la position des boutons dans ton projet FL Studio
void BLOOMAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void BLOOMAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BLOOMAudioProcessor();
}