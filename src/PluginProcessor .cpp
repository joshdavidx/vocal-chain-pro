// JoshDavid Magic - cadena vocal todo en uno (VST3)
// Interfaz: rack de efectos
// Orden: Gate -> EQ -> Autotune -> De-esser -> Compresor -> Saturacion -> Delay -> Reverb -> Limiter -> Salida

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>
#include <algorithm>

//==============================================================================
// Datos de los medidores (el audio los escribe y la interfaz los lee)
struct MeterData
{
    std::atomic<float> grDb { 0.0f };         // reduccion del compresor (dB)
    std::atomic<float> outDb { -100.0f };     // nivel de salida (dB)
    std::atomic<float> pitchMidi { -1.0f };   // nota objetivo del autotune (-1 = sin tono)
    std::atomic<float> cents { 0.0f };        // desafinacion detectada (centesimos)
};

static bool scaleHasNote (int pitchClass, int key, int scale)
{
    static const bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
    static const bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
    const int r = (((pitchClass - key) % 12) + 12) % 12;
    if (scale == 1) return major[r];
    if (scale == 2) return minor[r];
    return true;
}

//==============================================================================
// Interfaz estilo equipo de estudio (dibujada con codigo, sin imagenes)
class HwLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HwLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff15171b));
        setColour (juce::ComboBox::textColourId, juce::Colour (0xffe8e2cf));
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff6a6d73));
        setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffe8e2cf));
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1a1c20));
        setColour (juce::PopupMenu::textColourId, juce::Colour (0xffe8e2cf));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3a3e46));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::ToggleButton::textColourId, juce::Colour (0xffe8e2cf));
        setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff5a5f68));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        const float R = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();

        // marcas alrededor
        g.setColour (juce::Colour (0xffc9ccd2));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + ((float) i / 10.0f) * (endAngle - startAngle);
            g.drawLine (cx + R * 0.93f * std::sin (a), cy - R * 0.93f * std::cos (a),
                        cx + R * std::sin (a), cy - R * std::cos (a), 1.5f);
        }

        const float kr = R * 0.80f;

        // sombra y base
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillEllipse (cx - kr, cy - kr + kr * 0.08f, kr * 2.0f, kr * 2.0f);
        g.setColour (juce::Colour (0xff17181a));
        g.fillEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);

        // anillo estriado
        g.setColour (juce::Colour (0xff7c8087));
        for (int i = 0; i < 40; ++i)
        {
            const float a = (float) i * juce::MathConstants<float>::twoPi / 40.0f;
            g.drawLine (cx + kr * 0.89f * std::sin (a), cy - kr * 0.89f * std::cos (a),
                        cx + kr * 0.99f * std::sin (a), cy - kr * 0.99f * std::cos (a), 1.6f);
        }

        // tapa
        const float cr = kr * 0.80f;
        const juce::Colour cap ((juce::uint32) (int) s.getProperties()["cap"]);
        g.setGradientFill (juce::ColourGradient (cap.brighter (0.6f), cx - cr * 0.4f, cy - cr * 0.5f,
                                                 cap.darker (0.7f), cx - cr * 0.4f + cr * 1.5f, cy - cr * 0.5f + cr * 1.5f, true));
        g.fillEllipse (cx - cr, cy - cr, cr * 2.0f, cr * 2.0f);
        g.setColour (juce::Colours::black);
        g.drawEllipse (cx - cr, cy - cr, cr * 2.0f, cr * 2.0f, 1.0f);

        // indicador
        const float angle = startAngle + pos * (endAngle - startAngle);
        juce::Path p;
        p.addRoundedRectangle (-1.8f, -cr + 2.0f, 3.6f, cr * 0.62f, 1.8f);
        g.setColour (cap.getBrightness() > 0.6f ? juce::Colour (0xff1a1a1a) : juce::Colours::white);
        g.fillPath (p, juce::AffineTransform::rotation (angle).translated (cx, cy));
    }
};

//------------------------------------------------------------------------------
class VuMeter : public juce::Component
{
public:
    explicit VuMeter (MeterData& m) : meters (m) {}

    void tick()
    {
        const float t = meters.grDb.load();
        shown += (t - shown) * 0.25f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1c1c1c));
        g.fillRoundedRectangle (b, 6.0f);
        auto face = b.reduced (3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff3eacd), face.getCentreX(), face.getBottom(),
                                                 juce::Colour (0xffd8caa0), face.getCentreX(), face.getY(), false));
        g.fillRoundedRectangle (face, 4.0f);

        g.saveState();
        g.reduceClipRegion (face.toNearestInt());

        const float px = face.getCentreX();
        const float py = face.getBottom() - 4.0f;
        const float R = face.getHeight() * 0.88f;

        for (int i = 0; i <= 10; ++i)
        {
            const float a = juce::degreesToRadians (-50.0f + 10.0f * (float) i);
            const float r1 = R * 0.80f;
            const float r2 = R * (i > 6 ? 0.95f : 0.90f);
            g.setColour (i > 6 ? juce::Colour (0xffb3261e) : juce::Colour (0xff2a2a2a));
            g.drawLine (px + r1 * std::sin (a), py - r1 * std::cos (a),
                        px + r2 * std::sin (a), py - r2 * std::cos (a), (i % 2) != 0 ? 1.0f : 2.0f);
        }

        juce::Path red;
        red.addCentredArc (px, py, R * 0.78f, R * 0.78f, 0.0f,
                           juce::degreesToRadians (20.0f), juce::degreesToRadians (50.0f), true);
        g.setColour (juce::Colour (0xffb3261e));
        g.strokePath (red, juce::PathStrokeType (3.0f));

        const float ang = juce::degreesToRadians (-45.0f + juce::jlimit (0.0f, 20.0f, shown) / 20.0f * 90.0f);
        g.setColour (juce::Colour (0xff111111));
        g.drawLine (px, py, px + R * 0.96f * std::sin (ang), py - R * 0.96f * std::cos (ang), 1.8f);
        g.fillEllipse (px - 4.0f, py - 4.0f, 8.0f, 8.0f);
        g.restoreState();

        g.setColour (juce::Colour (0xff2a2a2a));
        g.setFont (11.0f);
        g.drawText ("REDUCCION", juce::Rectangle<float> (face.getX() + 6.0f, face.getY() + 2.0f, face.getWidth() - 12.0f, 14.0f),
                    juce::Justification::centredLeft);
    }

private:
    MeterData& meters;
    float shown = 0.0f;
};

//------------------------------------------------------------------------------
class TunerDisplay : public juce::Component
{
public:
    explicit TunerDisplay (MeterData& m) : meters (m) {}

    void tick()
    {
        midi = meters.pitchMidi.load();
        cents = meters.cents.load();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        const float s = juce::jmin (b.getWidth(), b.getHeight());
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();

        g.setColour (juce::Colour (0xff0d0e11));
        g.fillEllipse (cx - s * 0.5f, cy - s * 0.5f, s, s);
        g.setColour (juce::Colour (0xff6a6d73));
        g.drawEllipse (cx - s * 0.5f, cy - s * 0.5f, s, s, 2.0f);

        const float r = s * 0.40f;
        const float a0 = juce::MathConstants<float>::pi * 1.2f;
        const float a1 = juce::MathConstants<float>::pi * 2.8f;
        const juce::PathStrokeType stroke (s * 0.07f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc (cx, cy, r, r, 0.0f, a0, a1, true);
        g.setColour (juce::Colour (0xff2c2e35));
        g.strokePath (track, stroke);

        juce::String name = "-";
        juce::String centsText;
        if (midi >= 0.0f)
        {
            static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            name = names[(((int) std::lround (midi)) % 12 + 12) % 12];

            const float pos = juce::jlimit (0.02f, 1.0f, 0.5f + cents / 100.0f);
            juce::Path v;
            v.addCentredArc (cx, cy, r, r, 0.0f, a0, a0 + pos * (a1 - a0), true);
            g.setColour (juce::Colour (0xffc86bff));
            g.strokePath (v, stroke);

            centsText = juce::String (cents >= 0.0f ? "+" : "") + juce::String (cents, 0) + " cents";
        }

        g.setColour (juce::Colour (0xffecc4ff));
        g.setFont (s * 0.38f);
        g.drawText (name, juce::Rectangle<float> (cx - s * 0.3f, cy - s * 0.28f, s * 0.6f, s * 0.42f),
                    juce::Justification::centred);
        g.setColour (juce::Colour (0xff9a8fb0));
        g.setFont (11.0f);
        g.drawText (centsText, juce::Rectangle<float> (cx - s * 0.3f, cy + s * 0.16f, s * 0.6f, 14.0f),
                    juce::Justification::centred);
    }

private:
    MeterData& meters;
    float midi = -1.0f, cents = 0.0f;
};

//------------------------------------------------------------------------------
class PianoDisplay : public juce::Component
{
public:
    PianoDisplay (MeterData& m, juce::AudioProcessorValueTreeState& s) : meters (m), apvts (s) {}

    void paint (juce::Graphics& g) override
    {
        const int key = (int) apvts.getRawParameterValue ("atKey")->load();
        const int scale = (int) apvts.getRawParameterValue ("atScale")->load();
        const float midi = meters.pitchMidi.load();
        const int active = midi >= 0.0f ? (((int) std::lround (midi)) % 12 + 12) % 12 : -1;

        auto b = getLocalBounds().toFloat();
        const float ww = b.getWidth() / 21.0f;
        static const int whitePc[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static const int blackPc[7] = { 1, 3, -1, 6, 8, 10, -1 };

        for (int i = 0; i < 21; ++i)
        {
            const int pc = whitePc[i % 7];
            juce::Rectangle<float> r (b.getX() + (float) i * ww, b.getY(), ww, b.getHeight());
            juce::Colour top = juce::Colours::white, bot = juce::Colour (0xffd5d5d5);
            if (pc == active)               { top = juce::Colour (0xfffff0ff); bot = juce::Colour (0xffff8de6); }
            else if (scaleHasNote (pc, key, scale)) { top = juce::Colour (0xfff0d4ff); bot = juce::Colour (0xffc86bff); }
            g.setGradientFill (juce::ColourGradient (top, r.getX(), r.getY(), bot, r.getX(), r.getBottom(), false));
            g.fillRect (r);
            g.setColour (juce::Colour (0xff333333));
            g.drawRect (r, 1.0f);
        }

        for (int j = 0; j < 20; ++j)
        {
            const int pc = blackPc[j % 7];
            if (pc < 0)
                continue;
            juce::Rectangle<float> r (b.getX() + (float) (j + 1) * ww - ww * 0.3f, b.getY(), ww * 0.6f, b.getHeight() * 0.6f);
            juce::Colour top (0xff4a4a4a), bot (0xff050505);
            if (pc == active)               { top = juce::Colour (0xffff8de6); bot = juce::Colour (0xffb0309a); }
            else if (scaleHasNote (pc, key, scale)) { top = juce::Colour (0xffc86bff); bot = juce::Colour (0xff6d2aa8); }
            g.setGradientFill (juce::ColourGradient (top, r.getX(), r.getY(), bot, r.getX(), r.getBottom(), false));
            g.fillRect (r);
        }
    }

private:
    MeterData& meters;
    juce::AudioProcessorValueTreeState& apvts;
};

//------------------------------------------------------------------------------
class LedMeter : public juce::Component
{
public:
    explicit LedMeter (MeterData& m) : meters (m) {}

    void tick()
    {
        const float t = meters.outDb.load();
        shown = t > shown ? t : shown - 2.0f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int n = 12;
        auto b = getLocalBounds().toFloat();
        const float h = b.getHeight() / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const float th = -48.0f + 4.0f * (float) i;
            juce::Colour c = th >= -8.0f ? juce::Colour (0xffe0453a)
                                         : (th >= -20.0f ? juce::Colour (0xffe8c43a) : juce::Colour (0xff3adf6a));
            if (shown < th)
                c = c.withAlpha (0.18f);
            g.setColour (c);
            g.fillRoundedRectangle (b.getX(), b.getBottom() - (float) (i + 1) * h + 1.0f, b.getWidth(), h - 2.0f, 1.5f);
        }
    }

private:
    MeterData& meters;
    float shown = -100.0f;
};

//------------------------------------------------------------------------------
class VocalChainEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    VocalChainEditor (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& s, MeterData& md)
        : juce::AudioProcessorEditor (p), apvts (s), meters (md),
          tuner (md), piano (md, s), vu (md), led (md)
    {
        setLookAndFeel (&laf);

        const juce::Colour silver (0xffd9dce2), black (0xff2c2d30), red (0xffd23f35), green (0xff3faa5c),
                           blue (0xff3f7fd2), cream (0xffe6d9ae), amber (0xffe0a040), purple (0xffa855e8);
        const juce::Colour metalTop (0xff3b3e43), metalBot (0xff25272b);

        auto& gate = addModule ("GATE", juce::Colour (0xff8fe3de), metalTop, metalBot, 2.0f, 0);
        addKnob (gate, "gateThresh", "UMBRAL", " dB", 1, black);

        auto& at = addModule ("AUTOTUNE", juce::Colour (0xffe0b3ff), juce::Colour (0xff3a2f4a), juce::Colour (0xff221a2e), 6.0f, 1);
        addKnob (at, "atSpeed", "VELOCIDAD", " ms", 0, purple);
        addKnob (at, "atMix",   "MEZCLA", "", 2, purple);

        auto& ds = addModule ("DE-ESSER", juce::Colour (0xffa2f0b8), metalTop, metalBot, 2.0f, 0);
        addKnob (ds, "dsFreq",   "FREC", " Hz", 0, green);
        addKnob (ds, "dsThresh", "UMBRAL", " dB", 1, green);
        addKnob (ds, "dsAmount", "REDUC", " dB", 1, green);

        auto& eq = addModule ("ECUALIZADOR", juce::Colour (0xfff5c26b), juce::Colour (0xff2c2f34), juce::Colour (0xff1a1b1e), 5.6f, 0);
        addKnob (eq, "hpFreq",   "GRAVES", " Hz", 0, black);
        addKnob (eq, "lmFreq",   "MED FREC", " Hz", 0, blue);
        addKnob (eq, "lmGain",   "MEDIOS", " dB", 1, blue);
        addKnob (eq, "presFreq", "PRES FREC", " Hz", 0, green);
        addKnob (eq, "presGain", "PRESENCIA", " dB", 1, green);
        addKnob (eq, "airGain",  "AIRE", " dB", 1, red);

        auto& comp = addModule ("COMPRESOR OPTICO", juce::Colour (0xfff0dba0), juce::Colour (0xff4a463d), juce::Colour (0xff2e2b25), 4.4f, 2);
        addKnob (comp, "cThresh",  "UMBRAL", " dB", 1, cream);
        addKnob (comp, "cRatio",   "RATIO", ":1", 1, cream);
        addKnob (comp, "cAttack",  "ATAQUE", " ms", 1, cream);
        addKnob (comp, "cRelease", "RELEASE", " ms", 0, cream);
        addKnob (comp, "cMakeup",  "GANANCIA", " dB", 1, cream);

        auto& dly = addModule ("DELAY", juce::Colour (0xff9fe3e8), juce::Colour (0xff2f4a4f), juce::Colour (0xff1d2e31), 3.0f, 4);
        addKnob (dly, "dlyTime", "TIEMPO", " ms", 0, silver);
        addKnob (dly, "dlyFb",   "FEEDBACK", "", 2, silver);
        addKnob (dly, "dlyMix",  "MEZCLA", "", 2, silver);

        auto& rev = addModule ("REVERB", juce::Colour (0xffb4bcff), juce::Colour (0xff363f5a), juce::Colour (0xff222839), 3.0f, 0);
        addKnob (rev, "revSize", "TAMANO", "", 2, silver);
        addKnob (rev, "revDamp", "AMORT", "", 2, silver);
        addKnob (rev, "revMix",  "MEZCLA", "", 2, silver);

        auto& sat = addModule ("SATURACION", juce::Colour (0xffffd9a0), juce::Colour (0xff4d3b2a), juce::Colour (0xff2e2318), 1.6f, 0);
        addKnob (sat, "satAmt", "CANTIDAD", "", 2, amber);

        auto& lim = addModule ("LIMITER", juce::Colour (0xffff9db9), metalTop, metalBot, 2.0f, 0);
        addKnob (lim, "limThresh",  "TECHO", " dB", 1, red);
        addKnob (lim, "limRelease", "RELEASE", " ms", 0, red);

        auto& out = addModule ("SALIDA", juce::Colour (0xffd9d4c3), metalTop, metalBot, 1.8f, 3);
        addKnob (out, "outGain", "VOLUMEN", " dB", 1, silver);

        // controles del autotune
        atToggle.setButtonText ("ACTIVO");
        atToggle.setColour (juce::ToggleButton::tickColourId, purple);
        keyBox.addItemList (juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
        scaleBox.addItemList (juce::StringArray { "Cromatica", "Mayor", "Menor" }, 1);
        addAndMakeVisible (atToggle);
        addAndMakeVisible (keyBox);
        addAndMakeVisible (scaleBox);
        atAtt    = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "atOn", atToggle);
        keyAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "atKey", keyBox);
        scaleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "atScale", scaleBox);

        addAndMakeVisible (tuner);
        addAndMakeVisible (piano);
        addAndMakeVisible (vu);
        addAndMakeVisible (led);

        setSize (980, 680);
        startTimerHz (30);
    }

    ~VocalChainEditor() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff121315));

        // cabecera
        auto header = getLocalBounds().removeFromTop (56).toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff34363b), 0.0f, 0.0f,
                                                 juce::Colour (0xff1b1c1f), 0.0f, 56.0f, false));
        g.fillRect (header);
        g.setColour (juce::Colour (0xff6a6d73));
        g.fillRect (0.0f, 55.0f, (float) getWidth(), 1.0f);

        g.setFont (26.0f);
        g.setColour (juce::Colours::black);
        g.drawText ("JOSHDAVID MAGIC", header.reduced (22.0f, 0.0f).translated (0.0f, 2.0f), juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xffe8e2cf));
        g.drawText ("JOSHDAVID MAGIC", header.reduced (22.0f, 0.0f), juce::Justification::centredLeft);

        g.setFont (12.0f);
        g.setColour (juce::Colour (0xff9a9ea6));
        g.drawText ("RACK VOCAL", header.reduced (46.0f, 0.0f), juce::Justification::centredRight);
        g.setColour (juce::Colour (0xff3adf6a).withAlpha (0.3f));
        g.fillEllipse ((float) getWidth() - 40.0f, 21.0f, 14.0f, 14.0f);
        g.setColour (juce::Colour (0xff3adf6a));
        g.fillEllipse ((float) getWidth() - 38.0f, 23.0f, 10.0f, 10.0f);

        for (auto& m : modules)
            drawPanel (g, *m);
    }

    void resized() override
    {
        auto area = getLocalBounds().withTrimmedTop (56).reduced (12, 10);
        const std::vector<std::vector<int>> rows = { { 0, 1, 2 }, { 3, 4 }, { 5, 6, 7, 8, 9 } };
        const int heights[3] = { 215, 215, 150 };
        const int gap = 10;

        for (size_t r = 0; r < rows.size(); ++r)
        {
            auto rowArea = area.removeFromTop (heights[r]);
            area.removeFromTop (gap);

            const auto& row = rows[r];
            float sum = 0.0f;
            for (int idx : row)
                sum += modules[(size_t) idx]->weight;

            const float availW = (float) (rowArea.getWidth() - gap * ((int) row.size() - 1));
            for (size_t k = 0; k < row.size(); ++k)
            {
                auto& m = *modules[(size_t) row[k]];
                const bool last = (k + 1 == row.size());
                const int w = last ? rowArea.getWidth() : (int) (availW * m.weight / sum);
                m.bounds = rowArea.removeFromLeft (w);
                if (! last)
                    rowArea.removeFromLeft (gap);
                layoutModule (m);
            }
        }
    }

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };

    struct Module
    {
        juce::String title;
        juce::Colour accent, bgTop, bgBot;
        float weight = 1.0f;
        int kind = 0; // 0 normal, 1 autotune, 2 compresor, 3 salida, 4 delay
        std::vector<std::unique_ptr<Knob>> knobs;
        juce::Rectangle<int> bounds;
    };

    HwLookAndFeel laf;
    juce::AudioProcessorValueTreeState& apvts;
    MeterData& meters;
    std::vector<std::unique_ptr<Module>> modules;

    juce::ToggleButton atToggle;
    juce::ComboBox keyBox, scaleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> atAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> keyAtt, scaleAtt;

    TunerDisplay tuner;
    PianoDisplay piano;
    VuMeter vu;
    LedMeter led;

    void timerCallback() override
    {
        tuner.tick();
        vu.tick();
        led.tick();
        piano.repaint();
    }

    Module& addModule (const juce::String& title, juce::Colour accent, juce::Colour top, juce::Colour bot,
                       float weight, int kind)
    {
        modules.push_back (std::make_unique<Module>());
        auto& m = *modules.back();
        m.title = title;
        m.accent = accent;
        m.bgTop = top;
        m.bgBot = bot;
        m.weight = weight;
        m.kind = kind;
        return m;
    }

    void addKnob (Module& m, const juce::String& id, const juce::String& text,
                  const juce::String& suffix, int decimals, juce::Colour cap)
    {
        auto k = std::make_unique<Knob>();

        k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
        k->slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                       juce::MathConstants<float>::pi * 2.8f, true);
        k->slider.getProperties().set ("cap", (int) cap.getARGB());
        k->slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xfff1ead4));
        k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);

        k->label.setText (text, juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setFont (juce::Font (juce::FontOptions (11.0f)));
        k->label.setColour (juce::Label::textColourId, juce::Colour (0xffd9d4c3));

        addAndMakeVisible (k->slider);
        addAndMakeVisible (k->label);

        k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, k->slider);

        k->slider.textFromValueFunction = [suffix, decimals] (double v)
        {
            return juce::String (v, decimals) + suffix;
        };
        k->slider.valueFromTextFunction = [] (const juce::String& t)
        {
            return t.getDoubleValue();
        };
        k->slider.updateText();

        m.knobs.push_back (std::move (k));
    }

    static void placeKnob (Knob& k, juce::Rectangle<int> cell)
    {
        k.label.setBounds (cell.removeFromTop (14));
        k.slider.setBounds (cell);
    }

    static void placeRow (Module& m, juce::Rectangle<int> b)
    {
        const int n = (int) m.knobs.size();
        if (n == 0)
            return;
        const int cw = b.getWidth() / n;
        for (auto& k : m.knobs)
            placeKnob (*k, b.removeFromLeft (cw));
    }

    void layoutModule (Module& m)
    {
        auto b = m.bounds.reduced (10, 6);
        b.removeFromTop (22);

        if (m.kind == 1)
        {
            auto strip = b.removeFromTop (26);
            const int w = strip.getWidth() / 3;
            atToggle.setBounds (strip.removeFromLeft (w).reduced (3, 0));
            keyBox.setBounds (strip.removeFromLeft (w).reduced (3, 0));
            scaleBox.setBounds (strip.reduced (3, 0));
            b.removeFromTop (4);

            auto pianoArea = b.removeFromBottom (46);
            piano.setBounds (pianoArea.withSizeKeepingCentre (juce::jmin (pianoArea.getWidth(), 462), 46));
            b.removeFromBottom (4);

            const int ts = juce::jmin (b.getHeight(), 112);
            const int side = (b.getWidth() - ts) / 2;
            auto left = b.removeFromLeft (side);
            auto mid = b.removeFromLeft (ts);
            auto right = b;
            tuner.setBounds (mid.withSizeKeepingCentre (ts, ts));
            placeKnob (*m.knobs[0], left.withSizeKeepingCentre (juce::jmin (110, left.getWidth()), left.getHeight()));
            placeKnob (*m.knobs[1], right.withSizeKeepingCentre (juce::jmin (110, right.getWidth()), right.getHeight()));
            return;
        }

        if (m.kind == 2)
        {
            auto vuArea = b.removeFromTop (88);
            vu.setBounds (vuArea.withSizeKeepingCentre (juce::jmin (190, vuArea.getWidth()), 84));
            b.removeFromTop (2);
        }

        if (m.kind == 3)
        {
            auto ledArea = b.removeFromRight (26);
            led.setBounds (ledArea.reduced (2, 8));
        }

        placeRow (m, b);
    }

    static void drawScrew (juce::Graphics& g, float x, float y)
    {
        g.setColour (juce::Colour (0xff9a9ea4));
        g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        g.setColour (juce::Colour (0xff2a2a2a));
        g.drawEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.0f);
        g.drawLine (x - 2.5f, y + 1.5f, x + 2.5f, y - 1.5f, 1.2f);
    }

    static void drawReel (juce::Graphics& g, float x, float y, float s)
    {
        g.setColour (juce::Colour (0xff9aa0a8));
        g.fillEllipse (x, y, s, s);
        g.setColour (juce::Colour (0xff222222));
        g.drawEllipse (x, y, s, s, 2.0f);
        const float cx = x + s * 0.5f, cy = y + s * 0.5f;
        for (int i = 0; i < 3; ++i)
        {
            const float a = (float) i * juce::MathConstants<float>::twoPi / 3.0f;
            g.drawLine (cx, cy, cx + s * 0.4f * std::sin (a), cy - s * 0.4f * std::cos (a), 3.0f);
        }
        g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
    }

    static void drawPanel (juce::Graphics& g, const Module& m)
    {
        auto r = m.bounds.toFloat();
        g.setGradientFill (juce::ColourGradient (m.bgTop, r.getX(), r.getY(), m.bgBot, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colour (0xff6a6d73));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawLine (r.getX() + 8.0f, r.getY() + 1.5f, r.getRight() - 8.0f, r.getY() + 1.5f, 1.0f);

        drawScrew (g, r.getX() + 9.0f, r.getY() + 9.0f);
        drawScrew (g, r.getRight() - 9.0f, r.getY() + 9.0f);
        drawScrew (g, r.getX() + 9.0f, r.getBottom() - 9.0f);
        drawScrew (g, r.getRight() - 9.0f, r.getBottom() - 9.0f);

        g.setFont (13.0f);
        auto titleRect = juce::Rectangle<float> (r.getX(), r.getY() + 6.0f, r.getWidth(), 18.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawText (m.title, titleRect.translated (0.0f, 1.0f), juce::Justification::centred);
        g.setColour (m.accent);
        g.drawText (m.title, titleRect, juce::Justification::centred);

        if (m.kind == 4)
        {
            drawReel (g, r.getRight() - 82.0f, r.getY() + 5.0f, 30.0f);
            drawReel (g, r.getRight() - 46.0f, r.getY() + 5.0f, 30.0f);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChainEditor)
};

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
    const juce::String getName() const override { return "JoshDavid Magic"; }
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
        return new VocalChainEditor (*this, apvts, meters);
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

        float peak = 0.0f;
        for (int c = 0; c < numCh; ++c)
            peak = juce::jmax (peak, buffer.getMagnitude (c, 0, n));
        meters.outDb.store (juce::Decibels::gainToDecibels (peak, -100.0f));
    }

private:
    //==============================================================================
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    juce::AudioProcessorValueTreeState apvts;
    double sampleRate = 44100.0;
    MeterData meters;
    float lastNote = -1.0f, lastCents = 0.0f;

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
        lastNote = -1.0f;
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
        lastNote = bestNote;
        lastCents = (midi - bestNote) * 100.0f;
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
        {
            meters.pitchMidi.store (-1.0f);
            return;
        }

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
                meters.pitchMidi.store (lastNote);
                meters.cents.store (lastCents);
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
        meters.grDb.store (compGr);
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
