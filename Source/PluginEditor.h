#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class BloomArcsLayer; // les arcs (definis dans PluginEditor.cpp)
class BloomScene;     // la scene 3D (BloomScene.cpp)
class BloomSheet;     // la fiche explicative (definie dans PluginEditor.cpp)
class BloomMenu;      // le menu a gemmes en haut a droite (PluginEditor.cpp)
class BloomChain;     // la chaine argentee qui pend du cadre (PluginEditor.cpp)
class BloomDemoTag;   // l'etiquette DEMO en haut a gauche (PluginEditor.cpp)

// Tout le contenu du plugin (3D + interface). Il est pose dans la fenetre BLOOMAudioProcessorEditor ci-dessous.
class BloomView : public juce::Component,
                  private juce::Timer
{
public:
    explicit BloomView(BLOOMAudioProcessor&);
    ~BloomView() override;

    // pendant le demarrage de la 3D : dessine l'interface sans OpenGL (sur la derniere image du monde)
    void paintFallback(juce::Graphics& g);
    bool isGlLive() const;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override; // le cadre de pierre passe par-dessus tout
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override; // clic sur le logo : ouvre la fiche
    void mouseExit(const juce::MouseEvent&) override;

private:
    void timerCallback() override; // relance le dessin de la scene (30 images/s)

    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void setupSwitch(juce::TextButton& button, const juce::String& help, juce::Colour onColour);

    // --- Etape 1 : cadre de pierre moussue + logo ---
    void rebuildFrameCache();
    void drawLogo(juce::Graphics& g);

    juce::Image tileHTop, tileHBottom, tileVLeft, tileVRight; // morceaux de pierre (pixel-art)
    juce::Image tileCorner[4];                                // coins : haut-gauche, haut-droite, bas-gauche, bas-droite
    juce::Image frameCache;                                   // le cadre complet, dessine une seule fois par taille
    juce::Image logoGlow;                                     // halo ambre du logo (calcule une fois)
    int         logoGlowW = -1;
    juce::Image logoGlowHi;                                   // grand halo du logo au survol
    int         logoGlowHiW = -1;
    double      perfLastTick = 0.0;                           // diagnostic
    double      perfLastAudioSummary = 0.0;
    bool        perfFirstPaint = false;
    bool        logoHover = false;                            // la souris est sur le titre
    float       logoHoverAmt = 0.0f;                          // 0 -> 1 : intensite de l'effet lumineux
    double      logoHoverLast = 0.0;
    juce::Rectangle<int> logoFxArea() const;
    juce::Path  logoPath;                                     // les lettres B L O O M

    BLOOMAudioProcessor& audioProcessor;

    juce::TooltipWindow tooltipWindow{ this, 500 }; // bulles d'aide au survol

    void attachGl();
    static bool firstOpenThisSession();
    bool glAttached = false;
    bool licenseNagPending = false;              // ouvrir la page d'activation des que possible
    double createdAt = 0.0;
    juce::Image poster;                          // derniere image du monde, montree pendant le demarrage
    bool sceneRunning = true;                     // la 3D tourne seulement quand la fenetre est visible
    std::atomic<int> hoverIndex{ -1 };            // reglage survole (pour que le monde reagisse)
    std::unique_ptr<BloomArcsLayer> arcs;
    std::unique_ptr<BloomSheet> sheet;
    std::unique_ptr<BloomMenu> menu;
    std::unique_ptr<BloomChain> chain;
    std::unique_ptr<BloomDemoTag> demoTag;
    std::unique_ptr<BloomScene> scene;            // doit etre detruite apres le contexte OpenGL ci-dessous
    juce::OpenGLContext glContext;
    juce::TextButton bypassButton{ "BYPASS" }, deltaButton{ "DELTA" }, povButton{ "FACONNER" };

    std::unique_ptr<ButtonAttachment> bypassAttachment, deltaAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomView)
};

// La fenetre du plugin : elle affiche tout de suite l'interface (dessinee sans OpenGL)
// pendant que la 3D demarre, puis la 3D prend le relais sans ecran noir ni clignotement.
class BLOOMAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit BLOOMAudioProcessorEditor(BLOOMAudioProcessor&);
    ~BLOOMAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    std::unique_ptr<BloomView> view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BLOOMAudioProcessorEditor)
};
