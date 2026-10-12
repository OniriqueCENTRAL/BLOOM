// Renders the editor to a PNG without a host and without its 3D scene (the interface as it is drawn while OpenGL
// starts), and puts the plugin through what a host does to it.
//
//   bloom_snapshot out.png [--licence]      --licence leaves the licence page where it is
//   bloom_snapshot --selftest
#include <JuceHeader.h>
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct Director final : juce::Timer
{
    Director (juce::File destination, bool showLicence) : file (std::move (destination)), licence (showLicence)
    {
        processor.reset (createPluginFilter());
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);
        editor.reset (processor->createEditorIfNeeded());
        startTimer (30);
    }

    void timerCallback() override
    {
        if (++ticks < 30)
            return;

        stopTimer();

        // The licence page covers a plugin that is not activated on this machine: closed, to see the interface.
        if (! licence)
            if (auto* view = editor->getChildComponent (0))
                for (auto* child : view->getChildren())
                    if (dynamic_cast<onirique::license::Overlay*> (child) != nullptr)
                        child->setVisible (false);

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        file.deleteFile();

        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (image, *stream);

        juce::MessageManager::getInstance()->stopDispatchLoop();
    }

    juce::File file;
    std::unique_ptr<juce::AudioProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    int ticks = 0;
    bool licence = false;
};

/** Returns the number of things found wrong. */
int selfTest()
{
    int failures = 0;

    auto check = [&failures] (bool ok, const juce::String& what)
    {
        std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
        failures += ok ? 0 : 1;
    };

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
        processor->setPlayConfigDetails (2, 2, rate, kBlock);
        processor->prepareToPlay (rate, kBlock);

        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        bool finite = true;
        float peak = 0.0f;
        double phase = 0.0;

        for (int block = 0; block < 200; ++block)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                phase += 220.0 / rate;
                const auto v = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * phase);
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }

            processor->processBlock (buffer, midi);

            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kBlock; ++i)
                {
                    finite = finite && std::isfinite (buffer.getSample (c, i));
                    peak = juce::jmax (peak, std::abs (buffer.getSample (c, i)));
                }
        }

        check (finite && peak > 0.01f && peak < 4.0f, juce::String (rate / 1000.0, 1) + " kHz: a tone goes through, finite (peak " + juce::String (peak, 3)
                                                          + ", latency " + juce::String (processor->getLatencySamples()) + ")");

        juce::MemoryBlock state;
        processor->getStateInformation (state);
        std::unique_ptr<juce::AudioProcessor> second (createPluginFilter());
        second->setStateInformation (state.getData(), (int) state.getSize());
        juce::MemoryBlock again;
        second->getStateInformation (again);
        check (state.getSize() > 0 && state == again, "the saved state is restored as it was");
    }

    std::printf (failures == 0 ? "all good\n" : "%d failure(s)\n", failures);
    return failures;
}
}

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args (argv + 1, argc - 1);

    if (args.contains ("--selftest"))
        return selfTest();

    if (args.isEmpty())
    {
        std::printf ("usage: bloom_snapshot out.png [--licence]\n");
        return 1;
    }

    Director director (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]), args.contains ("--licence"));
    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}
