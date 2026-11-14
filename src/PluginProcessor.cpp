// VocalChain - cadena vocal todo en uno (VST3)
// Orden: Gate -> EQ -> Autotune -> De-esser -> Compresor -> Saturacion -> Delay -> Reverb -> Limiter -> Salida

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>
#include <vector>
#include <algorithm>

class VocalChainProcessor : public juce::AudioProcessor
{
public:
    VocalChainProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                    .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", createLayout())
    {
    }

    //==============================================================================
    const juce::String getName() const override { return "VocalChain"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new juce::GenericAudioProcessorEditor (*this);
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto& in  = layouts.getMainInputChannelSet();
        const auto& out = layouts.getMainOutputChannelSet();
        if (in != out) return false;
        return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    }

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        if (xml != nullptr)
            copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
        if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    //==============================================================================
    void prepareToPlay (double sr, int samplesPerBlock) override
    {
        sampleRate = sr;

        // Autotune: ventana del pitch shifter (40 ms) y buffers
        winSamples = (int) (0.040 * sr);
        const int shiftSize = juce::nextPowerOfTwo (2 * winSamples + 8);
        shiftMask = shiftSize - 1;
        for (auto& b : shiftBuf) b.assign ((size_t) shiftSize, 0.0f);
        writePos = 0;
        shiftPhase = 0.0f;
        ratioTarget = 1.0f;
        ratioSmooth = 1.0f;

        minLag = juce::jmax (2, (int) (sr / 800.0));
        maxLag = (int) (sr / 70.0);
        const int needed = analysisWin + maxLag + 4;
        const int detSize = juce::nextPowerOfTwo (needed);
        detMask = detSize - 1;
        detectBuf.assign ((size_t) detSize, 0.0f);
        detectPos = 0;
        hopCounter = 0;
        linearBuf.assign ((size_t) (analysisWin + maxLag + 2), 0.0f);
        corrBuf.assign ((size_t) (maxLag + 3), 0.0f);

        // Delay
        const int dSize = juce::nextPowerOfTwo ((int) sr + 8);
        dlyMask = dSize - 1;
        for (auto& b : dlyBuf) b.assign ((size_t) dSize, 0.0f);
        dlyPos = 0;
        dlyLp[0] = dlyLp[1] = 0.0f;

        // Reverb
        reverb.setSampleRate (sr);
        reverb.reset();

        // Limiter
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sr;
        spec.maximumBlockSize = (juce::uint32) juce::jmax (1, samplesPerBlock);
        spec.numChannels = (juce::uint32) juce::jmax (1, getTotalNumOutputChannels());
        limiter.prepare (spec);

        // Filtros
        invalidateEqCache();
        updateEq();
        for (int c = 0; c < 2; ++c)
        {
            hp[c].reset();
            lowMid[c].reset();
            pres[c].reset();
            air[c].reset();
            dsBand[c].reset();
        }

        gateEnv = 0.0f;
        gateGain = 1.0f;
        dsEnv = 0.0f;
        dsGr = 0.0f;
        compGr = 0.0f;
    }

    void releaseResources() override {}

    //==============================================================================
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (buffer.getNumChannels(), 2);
        if (numCh <= 0 || n <= 0)
            return;

        for (int c = numCh; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, n);

        updateEq();

        runGate (buffer, numCh, n);
        runEq (buffer, numCh, n);
        runAutotune (buffer, numCh, n);
        runDeEsser (buffer, numCh, n);
        runCompressor (buffer, numCh, n);
        runSaturation (buffer, numCh, n);
        runDelay (buffer, numCh, n);
        runReverb (buffer, numCh, n);
        runLimiter (buffer, numCh, n);

        const float outG = juce::Decibels::decibelsToGain (get ("outGain"));
        for (int c = 0; c < numCh; ++c)
            buffer.applyGain (c, 0, n, outG);
    }

private:
    //==============================================================================
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    juce::AudioProcessorValueTreeState apvts;
    double sampleRate = 44100.0;

    float get (const juce::String& id) const { return apvts.getRawParameterValue (id)->load(); }

    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        auto addF = [&layout] (const juce::String& id, const juce::String& name,
                               float lo, float hi, float def, float step = 0.01f)
        {
            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 }, name,
                juce::NormalisableRange<float> (lo, hi, step), def));
        };

        // Gate
        addF ("gateThresh", "Gate - Umbral (dB)", -90.0f, -20.0f, -60.0f, 0.1f);

        // EQ
        addF ("hpFreq",   "EQ - Corte de graves (Hz)", 20.0f, 400.0f, 80.0f, 1.0f);
        addF ("lmFreq",   "EQ - Medios graves frec (Hz)", 100.0f, 800.0f, 300.0f, 1.0f);
        addF ("lmGain",   "EQ - Medios graves (dB)", -12.0f, 12.0f, 0.0f, 0.1f);
        addF ("presFreq", "EQ - Presencia frec (Hz)", 1000.0f, 6000.0f, 3000.0f, 1.0f);
        addF ("presGain", "EQ - Presencia (dB)", -12.0f, 12.0f, 0.0f, 0.1f);
        addF ("airGain",  "EQ - Aire 10 kHz (dB)", -12.0f, 12.0f, 0.0f, 0.1f);

        // Autotune
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { "atOn", 1 }, "Autotune - Activado", true));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { "atKey", 1 }, "Autotune - Tonalidad",
            juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { "atScale", 1 }, "Autotune - Escala",
            juce::StringArray { "Cromatica", "Mayor", "Menor" }, 0));
        addF ("atSpeed", "Autotune - Velocidad (ms, 0 = robot)", 0.0f, 400.0f, 25.0f, 1.0f);
        addF ("atMix",   "Autotune - Mezcla", 0.0f, 1.0f, 1.0f, 0.01f);

        // De-esser
        addF ("dsFreq",   "De-esser - Frecuencia (Hz)", 4000.0f, 9000.0f, 6500.0f, 10.0f);
        addF ("dsThresh", "De-esser - Umbral (dB)", -60.0f, -10.0f, -32.0f, 0.1f);
        addF ("dsAmount", "De-esser - Reduccion max (dB)", 0.0f, 12.0f, 6.0f, 0.1f);

        // Compresor
        addF ("cThresh",  "Compresor - Umbral (dB)", -40.0f, 0.0f, -18.0f, 0.1f);
        addF ("cRatio",   "Compresor - Ratio", 1.0f, 10.0f, 3.0f, 0.1f);
        addF ("cAttack",  "Compresor - Ataque (ms)", 1.0f, 80.0f, 10.0f, 0.1f);
        addF ("cRelease", "Compresor - Release (ms)", 20.0f, 800.0f, 150.0f, 1.0f);
        addF ("cMakeup",  "Compresor - Ganancia (dB)", 0.0f, 18.0f, 3.0f, 0.1f);

        // Saturacion
        addF ("satAmt", "Saturacion", 0.0f, 1.0f, 0.2f, 0.01f);

        // Delay
        addF ("dlyTime", "Delay - Tiempo (ms)", 20.0f, 1000.0f, 250.0f, 1.0f);
        addF ("dlyFb",   "Delay - Feedback", 0.0f, 0.9f, 0.3f, 0.01f);
        addF ("dlyMix",  "Delay - Mezcla", 0.0f, 1.0f, 0.0f, 0.01f);

        // Reverb
        addF ("revSize", "Reverb - Tamano", 0.0f, 1.0f, 0.5f, 0.01f);
        addF ("revDamp", "Reverb - Amortiguacion", 0.0f, 1.0f, 0.5f, 0.01f);
        addF ("revMix",  "Reverb - Mezcla", 0.0f, 1.0f, 0.12f, 0.01f);

        // Limiter
        addF ("limThresh",  "Limiter - Techo (dB)", -12.0f, 0.0f, -1.0f, 0.1f);
        addF ("limRelease", "Limiter - Release (ms)", 10.0f, 500.0f, 100.0f, 1.0f);

        // Salida
        addF ("outGain", "Salida (dB)", -12.0f, 12.0f, 0.0f, 0.1f);

        return layout;
    }

    //==============================================================================
    // EQ
    Filter hp[2], lowMid[2], pres[2], air[2], dsBand[2];
    float cHp = 0, cLmF = 0, cLmG = 0, cPrF = 0, cPrG = 0, cAir = 0, cDsF = 0;

    void invalidateEqCache()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        cHp = cLmF = cLmG = cPrF = cPrG = cAir = cDsF = nan;
    }

    void updateEq()
    {
        const double sr = sampleRate;

        const float hpF = get ("hpFreq");
        if (hpF != cHp)
        {
            cHp = hpF;
            auto c = Coeffs::makeHighPass (sr, hpF, 0.7071f);
            hp[0].coefficients = c;
            hp[1].coefficients = c;
        }

        const float lmF = get ("lmFreq"), lmG = get ("lmGain");
        if (lmF != cLmF || lmG != cLmG)
        {
            cLmF = lmF; cLmG = lmG;
            auto c = Coeffs::makePeakFilter (sr, lmF, 1.0f, juce::Decibels::decibelsToGain (lmG));
            lowMid[0].coefficients = c;
            lowMid[1].coefficients = c;
        }

        const float prF = get ("presFreq"), prG = get ("presGain");
        if (prF != cPrF || prG != cPrG)
        {
            cPrF = prF; cPrG = prG;
            auto c = Coeffs::makePeakFilter (sr, prF, 0.9f, juce::Decibels::decibelsToGain (prG));
            pres[0].coefficients = c;
            pres[1].coefficients = c;
        }

        const float airG = get ("airGain");
        if (airG != cAir)
        {
            cAir = airG;
            auto c = Coeffs::makeHighShelf (sr, 10000.0f, 0.7071f, juce::Decibels::decibelsToGain (airG));
            air[0].coefficients = c;
            air[1].coefficients = c;
        }

        const float dsF = get ("dsFreq");
        if (dsF != cDsF)
        {
            cDsF = dsF;
            auto c = Coeffs::makeHighPass (sr, dsF, 0.7071f);
            dsBand[0].coefficients = c;
            dsBand[1].coefficients = c;
        }
    }

    void runEq (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        for (int c = 0; c < numCh; ++c)
        {
            float* x = buf.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                float s = x[i];
                s = hp[c].processSample (s);
                s = lowMid[c].processSample (s);
                s = pres[c].processSample (s);
                s = air[c].processSample (s);
                x[i] = s;
            }
        }
    }

    //==============================================================================
    // Gate
    float gateEnv = 0.0f, gateGain = 1.0f;

    void runGate (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float thr = juce::Decibels::decibelsToGain (get ("gateThresh"));
        const float sr = (float) sampleRate;
        const float envRel = std::exp (-1.0f / (0.050f * sr));
        const float openCoef = std::exp (-1.0f / (0.003f * sr));
        const float closeCoef = std::exp (-1.0f / (0.120f * sr));
        const float floorGain = 0.01f; // -40 dB

        for (int i = 0; i < n; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < numCh; ++c)
                level = juce::jmax (level, std::abs (buf.getReadPointer (c)[i]));

            gateEnv = level > gateEnv ? level : gateEnv * envRel;

            const float target = gateEnv < thr ? floorGain : 1.0f;
            const float coef = target > gateGain ? openCoef : closeCoef;
            gateGain = target + (gateGain - target) * coef;

            for (int c = 0; c < numCh; ++c)
                buf.getWritePointer (c)[i] *= gateGain;
        }
    }

    //==============================================================================
    // Autotune (deteccion de tono por autocorrelacion + pitch shifter de dos lineas de retardo)
    static constexpr int analysisWin = 1024;
    static constexpr int hopSize = 1024;

    std::vector<float> shiftBuf[2];
    int shiftMask = 0, writePos = 0, winSamples = 1764;
    float shiftPhase = 0.0f;

    std::vector<float> detectBuf, linearBuf, corrBuf;
    int detMask = 0, detectPos = 0, hopCounter = 0;
    int minLag = 55, maxLag = 630;
    float ratioTarget = 1.0f, ratioSmooth = 1.0f;

    static bool noteAllowed (int semitonesFromKey, int scale)
    {
        static const bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
        static const bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
        const int r = ((semitonesFromKey % 12) + 12) % 12;
        if (scale == 1) return major[r];
        if (scale == 2) return minor[r];
        return true;
    }

    void updatePitch (int key, int scale)
    {
        const int total = analysisWin + maxLag;

        // copiar el audio mas reciente a un buffer lineal
        for (int k = 0; k < total; ++k)
            linearBuf[(size_t) k] = detectBuf[(size_t) ((detectPos - total + k) & detMask)];

        const float* x = linearBuf.data();
        const float* ref = x + maxLag; // ultimas analysisWin muestras

        float e0 = 0.0f;
        for (int j = 0; j < analysisWin; ++j)
            e0 += ref[j] * ref[j];

        const float rms = std::sqrt (e0 / (float) analysisWin);
        if (rms < 0.01f)
        {
            ratioTarget = 1.0f;
            return;
        }

        float best = -1.0f;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const float* lg = x + (maxLag - lag);
            float cc = 0.0f, e1 = 0.0f;
            for (int j = 0; j < analysisWin; ++j)
            {
                cc += ref[j] * lg[j];
                e1 += lg[j] * lg[j];
            }
            const float v = cc / std::sqrt (e0 * e1 + 1.0e-9f);
            corrBuf[(size_t) lag] = v;
            best = juce::jmax (best, v);
        }

        if (best < 0.8f)
        {
            ratioTarget = 1.0f; // sin tono claro (consonantes, ruido)
            return;
        }

        // primer maximo local cercano al mejor (evita errores de octava)
        int pick = -1;
        for (int lag = minLag + 1; lag < maxLag; ++lag)
        {
            const float v = corrBuf[(size_t) lag];
            if (v >= 0.92f * best && v >= corrBuf[(size_t) (lag - 1)] && v >= corrBuf[(size_t) (lag + 1)])
            {
                pick = lag;
                break;
            }
        }
        if (pick < 0)
        {
            ratioTarget = 1.0f;
            return;
        }

        const float a = corrBuf[(size_t) (pick - 1)];
        const float b = corrBuf[(size_t) pick];
        const float c = corrBuf[(size_t) (pick + 1)];
        const float denom = a - 2.0f * b + c;
        const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;
        const float lagF = (float) pick + juce::jlimit (-1.0f, 1.0f, shift);
        const float freq = (float) sampleRate / lagF;

        const float midi = 69.0f + 12.0f * std::log2 (freq / 440.0f);
        const int center = (int) std::lround (midi);
        float bestNote = (float) center;
        float bestDist = 1.0e9f;
        for (int m = center - 3; m <= center + 3; ++m)
        {
            if (! noteAllowed (m - key, scale))
                continue;
            const float d = std::abs ((float) m - midi);
            if (d < bestDist)
            {
                bestDist = d;
                bestNote = (float) m;
            }
        }

        const float targetFreq = 440.0f * std::pow (2.0f, (bestNote - 69.0f) / 12.0f);
        ratioTarget = juce::jlimit (0.8f, 1.25f, targetFreq / freq);
    }

    float readDelayed (int ch, float delaySamples) const
    {
        const float pos = (float) writePos - delaySamples;
        const float fl = std::floor (pos);
        const int i0 = (int) fl;
        const float frac = pos - fl;
        const float s0 = shiftBuf[ch][(size_t) (i0 & shiftMask)];
        const float s1 = shiftBuf[ch][(size_t) ((i0 + 1) & shiftMask)];
        return s0 + (s1 - s0) * frac;
    }

    void runAutotune (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float mix = get ("atMix");
        if (get ("atOn") < 0.5f || mix <= 0.0f)
            return;

        const int key = (int) get ("atKey");
        const int scale = (int) get ("atScale");
        const float speedMs = get ("atSpeed");
        const float alpha = speedMs < 0.5f ? 1.0f
                                           : 1.0f - std::exp (-1.0f / (0.001f * speedMs * (float) sampleRate));
        const float W = (float) winSamples;
        const float pi = juce::MathConstants<float>::pi;

        for (int i = 0; i < n; ++i)
        {
            float in[2] = { 0.0f, 0.0f };
            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                in[c] = buf.getReadPointer (c)[i];
                mono += in[c];
            }
            mono /= (float) numCh;

            detectBuf[(size_t) detectPos] = mono;
            detectPos = (detectPos + 1) & detMask;
            if (++hopCounter >= hopSize)
            {
                hopCounter = 0;
                updatePitch (key, scale);
            }

            ratioSmooth += (ratioTarget - ratioSmooth) * alpha;

            for (int c = 0; c < numCh; ++c)
                shiftBuf[c][(size_t) writePos] = in[c];

            const float p1 = shiftPhase;
            float p2 = shiftPhase + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;

            const float s1 = std::sin (pi * p1);
            const float s2 = std::sin (pi * p2);
            const float w1 = s1 * s1;
            const float w2 = s2 * s2;
            const float d1 = p1 * W;
            const float d2 = p2 * W;

            for (int c = 0; c < numCh; ++c)
            {
                const float wet = w1 * readDelayed (c, d1) + w2 * readDelayed (c, d2);
                buf.getWritePointer (c)[i] = in[c] * (1.0f - mix) + wet * mix;
            }

            writePos = (writePos + 1) & shiftMask;

            shiftPhase += (1.0f - ratioSmooth) / W;
            while (shiftPhase >= 1.0f) shiftPhase -= 1.0f;
            while (shiftPhase < 0.0f) shiftPhase += 1.0f;
        }
    }

    //==============================================================================
    // De-esser
    float dsEnv = 0.0f, dsGr = 0.0f;

    void runDeEsser (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float maxRed = get ("dsAmount");
        const float thrDb = get ("dsThresh");
        const float sr = (float) sampleRate;
        const float envAtk = std::exp (-1.0f / (0.0005f * sr));
        const float envRel = std::exp (-1.0f / (0.030f * sr));
        const float grAtk = std::exp (-1.0f / (0.001f * sr));
        const float grRel = std::exp (-1.0f / (0.040f * sr));

        for (int i = 0; i < n; ++i)
        {
            float band[2] = { 0.0f, 0.0f };
            float level = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                band[c] = dsBand[c].processSample (buf.getReadPointer (c)[i]);
                level = juce::jmax (level, std::abs (band[c]));
            }

            const float coefE = level > dsEnv ? envAtk : envRel;
            dsEnv = level + (dsEnv - level) * coefE;

            const float levelDb = 20.0f * std::log10 (dsEnv + 1.0e-9f);
            const float over = levelDb - thrDb;
            const float targetGr = juce::jlimit (0.0f, maxRed, over * 0.8f);
            const float coefG = targetGr > dsGr ? grAtk : grRel;
            dsGr = targetGr + (dsGr - targetGr) * coefG;

            const float g = juce::Decibels::decibelsToGain (-dsGr);
            for (int c = 0; c < numCh; ++c)
            {
                float* x = buf.getWritePointer (c);
                x[i] = x[i] - band[c] * (1.0f - g);
            }
        }
    }

    //==============================================================================
    // Compresor (estilo optico: release que depende del nivel de reduccion)
    float compGr = 0.0f;

    void runCompressor (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float thr = get ("cThresh");
        const float ratio = get ("cRatio");
        const float atkMs = get ("cAttack");
        const float relMs = get ("cRelease");
        const float makeup = get ("cMakeup");
        const float sr = (float) sampleRate;
        const float knee = 6.0f;
        const float slope = 1.0f / ratio - 1.0f;
        const float atkCoef = std::exp (-1.0f / (0.001f * atkMs * sr));

        for (int i = 0; i < n; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < numCh; ++c)
                level = juce::jmax (level, std::abs (buf.getReadPointer (c)[i]));

            const float lvDb = 20.0f * std::log10 (level + 1.0e-6f);
            const float over = lvDb - thr;

            float gc = 0.0f;
            if (2.0f * over > knee)
                gc = slope * over;
            else if (2.0f * over > -knee)
            {
                const float t = over + knee * 0.5f;
                gc = slope * t * t / (2.0f * knee);
            }

            const float targetGr = -gc;
            if (targetGr > compGr)
            {
                compGr = targetGr + (compGr - targetGr) * atkCoef;
            }
            else
            {
                const float relCoef = std::exp (-1.0f / (0.001f * relMs * (1.0f + compGr / 8.0f) * sr));
                compGr = targetGr + (compGr - targetGr) * relCoef;
            }

            const float g = juce::Decibels::decibelsToGain (makeup - compGr);
            for (int c = 0; c < numCh; ++c)
                buf.getWritePointer (c)[i] *= g;
        }
    }

    //==============================================================================
    // Saturacion suave
    void runSaturation (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float amt = get ("satAmt");
        if (amt <= 0.001f)
            return;

        const float drive = 1.0f + 4.0f * amt;
        for (int c = 0; c < numCh; ++c)
        {
            float* x = buf.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float wet = std::tanh (drive * x[i]) / drive;
                x[i] = x[i] + (wet - x[i]) * amt;
            }
        }
    }

    //==============================================================================
    // Delay
    std::vector<float> dlyBuf[2];
    int dlyMask = 0, dlyPos = 0;
    float dlyLp[2] = { 0.0f, 0.0f };

    void runDelay (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float mix = get ("dlyMix");
        const float fb = get ("dlyFb");
        const int dly = juce::jlimit (1, dlyMask, (int) (get ("dlyTime") * 0.001 * sampleRate));

        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < numCh; ++c)
            {
                float* x = buf.getWritePointer (c);
                const float in = x[i];
                const float echo = dlyBuf[c][(size_t) ((dlyPos - dly) & dlyMask)];
                dlyLp[c] += (echo - dlyLp[c]) * 0.35f;
                dlyBuf[c][(size_t) dlyPos] = in + dlyLp[c] * fb;
                x[i] = in + echo * mix;
            }
            dlyPos = (dlyPos + 1) & dlyMask;
        }
    }

    //==============================================================================
    // Reverb
    juce::Reverb reverb;

    void runReverb (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        const float mix = get ("revMix");

        juce::Reverb::Parameters p;
        p.roomSize = get ("revSize");
        p.damping = get ("revDamp");
        p.wetLevel = mix * 0.4f;
        p.dryLevel = 0.5f; // en juce::Reverb 0.5 equivale a ganancia 1
        p.width = 1.0f;
        p.freezeMode = 0.0f;
        reverb.setParameters (p);

        if (numCh >= 2)
            reverb.processStereo (buf.getWritePointer (0), buf.getWritePointer (1), n);
        else
            reverb.processMono (buf.getWritePointer (0), n);
    }

    //==============================================================================
    // Limiter
    juce::dsp::Limiter<float> limiter;

    void runLimiter (juce::AudioBuffer<float>& buf, int numCh, int n)
    {
        limiter.setThreshold (get ("limThresh"));
        limiter.setRelease (get ("limRelease"));

        juce::dsp::AudioBlock<float> block (buf);
        auto sub = block.getSubsetChannelBlock (0, (size_t) numCh).getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (sub);
        limiter.process (ctx);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChainProcessor)
};

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalChainProcessor();
}
