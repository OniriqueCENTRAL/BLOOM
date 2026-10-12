#include "LicenseOverlay.h"

namespace onirique::license
{

namespace palette = theme::palette;

static juce::String fr (const char* utf8) { return juce::String::fromUTF8 (utf8); }

//==============================================================================
/** La pastille de l'en-tête : voyant, état de la licence, invitation à activer. */
class Overlay::Pill final : public juce::Button
{
public:
    Pill() : juce::Button ("Activation") { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    /** `isActive` : plugin acheté. La pastille devient un simple témoin, vert et discret. */
    void setLabel (const juce::String& newLabel, bool isActive)
    {
        if (label != newLabel || active != isActive)
        {
            label = newLabel;
            active = isActive;
            setTitle (newLabel);
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        const auto tint = active ? palette::active : palette::ember;
        g.setColour (active ? juce::Colours::black.withAlpha (down ? 0.6f : over ? 0.5f : 0.38f)
                            : tint.withAlpha (down ? 0.42f : over ? 0.3f : 0.18f));
        g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
        g.setColour (tint.withAlpha (active ? (over ? 0.6f : 0.35f) : 0.75f));
        g.drawRoundedRectangle (area, area.getHeight() * 0.5f, 1.0f);

        const juce::Point<float> led (area.getX() + 14.0f, area.getCentreY());
        g.setColour (tint.withAlpha (0.35f));
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (led));
        g.setColour (active ? palette::active : palette::emberHot);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (led));

        g.setColour (active ? palette::bone.withAlpha (0.85f) : palette::bone);
        g.setFont (fonts->monoBoldFont (10.0f, 0.14f));
        g.drawText (label, getLocalBounds().withTrimmedLeft (24).withTrimmedRight (8), juce::Justification::centred, false);
    }

private:
    theme::Fonts fonts;
    juce::String label;
    bool active = false;
};

//==============================================================================
/** Bouton de la page d'activation : plein (action principale) ou en contour. */
class Overlay::Action final : public juce::Button
{
public:
    Action (const juce::String& text, bool isPrimary) : juce::Button (text), primary (isPrimary)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = area.getHeight() * 0.5f;

        if (primary)
        {
            g.setColour (down ? palette::ember.darker (0.15f) : over ? palette::emberHot : palette::ember);
            g.fillRoundedRectangle (area, radius);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRoundedRectangle (area, radius);
            g.setColour (palette::rose.withAlpha (over ? 0.5f : 0.24f));
            g.drawRoundedRectangle (area, radius, 1.0f);
        }

        g.setColour (primary ? juce::Colours::white : palette::mist);
        g.setFont (fonts->monoBoldFont (10.5f, 0.14f));
        g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
    }

private:
    theme::Fonts fonts;
    bool primary;
};

//==============================================================================
Overlay::Overlay (juce::String productName, Guard& licenseGuard)
    : product (std::move (productName)), guard (licenseGuard)
{
    pill = std::make_unique<Pill>();
    openCentral = std::make_unique<Action> ("OUVRIR ONIRIQUE CENTRAL", true);
    recheck = std::make_unique<Action> (fr ("REVÉRIFIER"), false);
    dismiss = std::make_unique<Action> (fr ("CONTINUER EN DÉMO"), false);

    pill->onClick = [this] { setPageOpen (! pageOpen); };
    dismiss->onClick = [this] { setPageOpen (false); };

    recheck->onClick = [this]
    {
        guard.refresh();
        update();
    };

    openCentral->onClick = [] { core::openCentral(); };

    addChildComponent (*pill);
    addChildComponent (*openCentral);
    addChildComponent (*recheck);
    addChildComponent (*dismiss);

    setInterceptsMouseClicks (true, true);
    update();
    startTimer (1000);
}

Overlay::~Overlay() { stopTimer(); }

void Overlay::fit (juce::Rectangle<int> editorBounds, float scale, juce::Rectangle<int> pillArea)
{
    scale = juce::jmax (0.1f, scale);
    pillBounds = pillArea;
    setTransform (juce::AffineTransform::scale (scale));
    setBounds (0, 0, juce::roundToInt ((float) editorBounds.getWidth() / scale), juce::roundToInt ((float) editorBounds.getHeight() / scale));
    resized();
}

juce::Rectangle<int> Overlay::cardBounds() const
{
    return juce::Rectangle<int> (560, 360).withCentre (getLocalBounds().getCentre());
}

void Overlay::setPageOpen (bool shouldBeOpen)
{
    pageOpen = shouldBeOpen;
    openCentral->setVisible (pageOpen);
    recheck->setVisible (pageOpen);
    dismiss->setVisible (pageOpen);
    repaint();
}

void Overlay::update()
{
    const auto previous = status.state;
    status = guard.status();

    const bool licensed = status.state == State::licensed;
    pill->setVisible (true);
    pill->setLabel (licensed ? fr ("ACTIVÉ")
                             : status.state == State::trial ? fr ("ESSAI · ") + juce::String (status.daysLeft()) + fr (" J · ACTIVATION")
                                                            : fr ("MODE DÉMO · ACTIVATION"),
                    licensed);
    dismiss->setName (status.state == State::unlicensed ? fr ("CONTINUER EN DÉMO") : juce::String ("FERMER"));

    // Non activé : la page s'ouvre d'elle-même à la première ouverture de l'interface.
    if (status.state == State::unlicensed && ! openedOnce)
    {
        openedOnce = true;
        setPageOpen (true);
    }

    if (previous != status.state)
    {
        // Le plugin vient d'être activé : la page d'activation n'a plus lieu d'être.
        if (licensed && pageOpen)
            setPageOpen (false);

        resized();
        repaint();
    }
}

void Overlay::resized()
{
    // Plugin acheté : le témoin « ACTIVÉ » est plus court, au milieu de l'emplacement de la pastille.
    pill->setBounds (status.state == State::licensed ? pillBounds.withSizeKeepingCentre (104, pillBounds.getHeight()) : pillBounds);

    const auto card = cardBounds().reduced (34, 0);
    const int y = cardBounds().getBottom() - 34 - 38;
    openCentral->setBounds (card.getX(), y, 220, 38);
    recheck->setBounds (card.getX() + 232, y, 112, 38);
    dismiss->setBounds (card.getX() + 356, y, 136, 38);
}

bool Overlay::hitTest (int x, int y)
{
    // Page ouverte : tout le plugin est derrière le voile. Sinon, seule la pastille réagit.
    return pageOpen || pill->getBounds().contains (x, y);
}

void Overlay::paint (juce::Graphics& g)
{
    if (! pageOpen)
        return;

    g.fillAll (juce::Colour (0xff080405).withAlpha (0.84f));

    const auto card = cardBounds().toFloat();
    g.setGradientFill ({ palette::deep2, card.getX(), card.getY(), palette::deep, card.getX(), card.getBottom(), false });
    g.fillRoundedRectangle (card, 18.0f);
    g.setColour (palette::rose.withAlpha (0.24f));
    g.drawRoundedRectangle (card.reduced (0.5f), 18.0f, 1.0f);

    const bool trial = status.state == State::trial;
    const bool licensed = status.state == State::licensed;
    const auto inner = cardBounds().reduced (34, 0);
    const auto account = status.email.isNotEmpty() ? fr (" sur le compte ") + status.email : juce::String();

    theme::drawEyebrow (g, *fonts, licensed ? "LICENCE" : trial ? "ESSAI GRATUIT" : fr ("MODE DÉMO"), (float) inner.getX(), card.getY() + 42.0f);

    g.setColour (palette::mist);
    g.setFont (fonts->displayFont (25.0f, 0.02f));
    const auto title = licensed ? fr ("Plugin activé")
                     : trial ? juce::String (status.daysLeft()) + (status.daysLeft() > 1 ? " jours restants" : " jour restant")
                             : juce::String ("Activation requise");
    g.drawText (title, inner.withY ((int) card.getY() + 62).withHeight (36), juce::Justification::centredLeft, false);

    const auto text = licensed
        ? product + fr (" est activé sur ce PC") + account + fr (".\n\n")
              + (status.expiresAt > 0
                     ? fr ("Licence d'abonnement : ONIRIQUE CENTRAL la renouvelle à chaque lancement tant que l'abonnement court.\n\n")
                     : juce::String())
              + fr ("Vos licences et vos machines activées se gèrent dans ONIRIQUE CENTRAL.")
        : trial
        ? fr ("Essai de ") + product + fr (" en cours") + account + fr (".\n\n")
              + fr ("Le plugin fonctionne normalement en temps réel. Tout export reste remplacé par un bruit blanc tant que le plugin n'est pas acheté.\n\n")
              + fr ("Achetez-le sur onirique.store ou dans ONIRIQUE CENTRAL : l'achat est pris en compte ici en quelques secondes.")
        : product + fr (" n'est pas activé sur ce PC.\n\n")
              + fr ("Sans licence, l'écoute est coupée par un bruit blanc toutes les trente secondes et tout export est remplacé par ce bruit.\n\n")
              + fr ("Ouvrez ONIRIQUE CENTRAL, connectez-vous à votre compte, puis activez le plugin ou lancez son essai gratuit de 7 jours. ")
              + fr ("L'activation est prise en compte ici en quelques secondes.");

    g.setColour (palette::haze.brighter (0.25f));
    g.setFont (fonts->bodyFont (14.5f));
    g.drawFittedText (text, inner.withY ((int) card.getY() + 110).withHeight (150), juce::Justification::topLeft, 9, 1.0f);
}

} // namespace onirique::license
