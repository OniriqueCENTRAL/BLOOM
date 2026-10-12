#pragma once

#include "LicenseGuard.h"
#include "LicenseTheme.h"

namespace onirique::license
{

//==============================================================================
/** Ce que le plugin montre de sa licence, par-dessus son interface :

    - une pastille : « MODE DÉMO · ACTIVATION », « ESSAI · 5 J · ACTIVATION », ou le témoin
      « ACTIVÉ » quand le plugin est acheté ;
    - la page de la licence, ouverte d'office quand le plugin n'est pas activé, puis à la
      demande en cliquant la pastille.

    S'ajoute en dernier à l'interface
    (addAndMakeVisible), puis se place avec fit() dans resized() ; ne capte la souris que sur
    la pastille et sur la page ouverte. */
class Overlay final : public juce::Component,
                      private juce::Timer
{
public:
    Overlay (juce::String productName, Guard& guard);
    ~Overlay() override;

    /** Couvre toute l'interface. `scale` : le zoom de l'interface (1 = taille de base) ;
        `pillArea` : l'emplacement de la pastille (248 x 26 conseillé), à la taille de base. */
    void fit (juce::Rectangle<int> editorBounds, float scale, juce::Rectangle<int> pillArea);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool hitTest (int x, int y) override;

private:
    class Pill;
    class Action;

    void timerCallback() override { update(); }
    void update();
    void setPageOpen (bool shouldBeOpen);
    juce::Rectangle<int> cardBounds() const;

    theme::Fonts fonts;
    juce::String product;
    Guard& guard;

    Status status;
    bool pageOpen = false;
    bool openedOnce = false;
    juce::Rectangle<int> pillBounds;

    std::unique_ptr<Pill> pill;
    std::unique_ptr<Action> openCentral, recheck, dismiss;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Overlay)
};

} // namespace onirique::license
