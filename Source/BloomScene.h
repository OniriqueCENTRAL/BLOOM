#pragma once

#include <JuceHeader.h>
#include <atomic>

// Reveille le pilote de la carte graphique en arriere-plan (une seule fois par session),
// pour que la premiere ouverture de la fenetre ne montre pas d'ecran noir.
void bloomPrewarmGraphics();
// Attend la fin de ce reveil s'il tourne encore (appele a la destruction du plugin).
void bloomWaitGraphicsWarmup();
// Le decor 3D est-il decode (pret a etre envoye a la carte graphique) ?
bool bloomSceneDataReady();
// "Affiche" : derniere image du monde 3D, montree pendant que la 3D demarre (pas d'ecran noir).
juce::Image bloomGetPoster();
// Enregistre l'affiche sur le disque (pour la premiere ouverture de la prochaine session).
void bloomSavePosterIfNeeded();

// La scene 3D de BLOOM (ile flottante, fleur, decor), dessinee avec OpenGL.
// Elle lit directement les reglages du plugin pour s'animer.
class BloomScene : public juce::OpenGLRenderer
{
public:
    // hoverIndex : numero du reglage survole (0 weight, 1 warmth, 2 space, 3 presence, 4 air, 5 enhance, 6 mix), -1 si aucun
    BloomScene(juce::AudioProcessorValueTreeState& apvts, juce::OpenGLContext& context,
               juce::Component& owner, std::atomic<int>& hoverIndex);
    ~BloomScene() override;

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    // Position (en pixels de l'editeur) de l'objet du monde qui correspond au reglage `index`
    // (meme numerotation que hoverIndex). Renvoie false si l'objet n'est pas visible.
    bool getAnchor(int index, float& x, float& y) const;

    // diagnostic : images par seconde et plus long intervalle entre deux images (en ms) sur la derniere seconde
    void getStats(float& fps, float& worstMs) const;

    // vrai des que la 3D a reellement ete affichee a l'ecran (quelques images completes)
    bool hasPresentedWorld() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomScene)
};
