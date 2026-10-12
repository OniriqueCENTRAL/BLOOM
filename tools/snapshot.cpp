// Renders the editor to a PNG without a host and without its 3D scene (the interface as it is drawn while OpenGL
// starts), and puts the plugin through what a host does to it.
//
//   bloom_snapshot out.png [--state demo|trial|active] [--page] [--ticks n]
//       --state   what the interface shows of the licence (demo by default), whatever this machine has
//       --page    with the licence page open, as a click on the tag in the top left corner opens it
//       --ticks   timer ticks of 30 ms before the picture
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
    Director (juce::File destination, int ticksToWait, bool openLicencePage)
        : file (std::move (destination)), wait (ticksToWait), page (openLicencePage)
    {
        processor.reset (createPluginFilter());
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);
        editor.reset (processor->createEditorIfNeeded());
        startTimer (30);
    }

    void timerCallback() override
    {
        // A click on the tag in the top left corner, as the mouse would make it: it opens the licence page.
        if (page && ticks == 8)
            if (auto* view = editor->getChildComponent (0))
                if (auto* tag = view->getComponentAt (juce::Point<int> (60, 44)))
                {
                    const auto at = tag->getLocalBounds().getCentre().toFloat();
                    const auto now = juce::Time::getCurrentTime();
                    tag->mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                                      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, tag, tag, now, at, now, 1, false));
                }

        if (++ticks < wait)
            return;

        stopTimer();

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        file.deleteFile();

        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (image, *stream);

        juce::MessageManager::getInstance()->stopDispatchLoop();
    }

    juce::File file;
    std::unique_ptr<juce::AudioProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    int ticks = 0, wait = 30;
    bool page = false;
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
        std::printf ("usage: bloom_snapshot out.png [--state demo|trial|active] [--page] [--ticks n]\n");
        return 1;
    }

    // What the interface shows of the licence, whatever this machine has: demo, a trial with five days left, or bought.
    using State = onirique::license::State;
    const auto stateAt = args.indexOf ("--state");
    const auto state = stateAt >= 0 && stateAt + 1 < args.size() ? args[stateAt + 1] : juce::String ("demo");
    bloomlic::License::showInPictures (state == "active" ? State::licensed : state == "trial" ? State::trial : State::unlicensed,
                                       state == "demo" ? juce::String() : juce::String ("prenom.nom-compose@exemple-de-domaine.com"), state == "trial" ? 5 : 0);

    const auto at = args.indexOf ("--ticks");
    const auto page = args.contains ("--page");
    Director director (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]),
                       at >= 0 && at + 1 < args.size() ? juce::jmax (1, args[at + 1].getIntValue()) : (page ? 60 : 30), page);
    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}
