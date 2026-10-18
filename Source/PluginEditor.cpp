#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BloomScene.h"
#include "BloomFonts.h"
#include "BloomPerf.h"
#include "BloomLicense.h"
#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <unistd.h>
#endif

#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>

//==============================================================================
// Outils de dessin (utilises seulement dans ce fichier)
//==============================================================================
namespace
{
    juce::Colour rgb(int r, int g, int b)
    {
        return juce::Colour((juce::uint8) juce::jlimit(0, 255, r),
                            (juce::uint8) juce::jlimit(0, 255, g),
                            (juce::uint8) juce::jlimit(0, 255, b));
    }

    // Fabrique un petit morceau de pierre avec de la mousse violette (meme principe que le prototype web)
    juce::Image makeStoneTile(int w, int h, int rowH, float mossTop, int blobs, int seed)
    {
        static const int ST[5][3] = { { 52, 48, 60 }, { 44, 41, 52 }, { 62, 58, 72 }, { 36, 33, 44 }, { 50, 46, 58 } };
        static const int MO[5][3] = { { 70, 42, 100 }, { 88, 55, 126 }, { 52, 30, 78 }, { 112, 76, 156 }, { 38, 22, 60 } };

        std::vector<juce::Colour> pix((size_t) (w * h), rgb(14, 12, 18)); // joints sombres
        std::vector<char> mortar((size_t) (w * h), 1);
        juce::Random rn(seed);

        // Les pierres, rangee par rangee, decalees comme des briques
        for (int r = 0; r * rowH < h; ++r)
        {
            const int y0 = r * rowH;
            int x = -rn.nextInt(6) - ((r % 2) != 0 ? 4 : 0);

            while (x < w)
            {
                const int wd = 5 + rn.nextInt(5);
                const int* c = ST[rn.nextInt(5)];

                for (int yy = 0; yy < rowH - 1; ++yy)
                {
                    for (int xx = 0; xx < wd - 1; ++xx)
                    {
                        const int X = x + xx;
                        const int Y = y0 + yy;
                        if (X < 0 || X >= w || Y >= h)
                            continue;

                        int add = rn.nextInt(15) - 7;
                        if (yy == 0 || xx == 0)      add += 16;   // reflet en haut et a gauche
                        if (yy == rowH - 2)          add -= 12;   // ombre en bas

                        const size_t i = (size_t) (Y * w + X);
                        pix[i] = rgb(c[0] + add, c[1] + add, c[2] + add);
                        mortar[i] = 0;
                    }
                }
                x += wd;
            }
        }

        // Les touffes de mousse, surtout en haut
        for (int b = 0; b < blobs; ++b)
        {
            const int cx = rn.nextInt(w);
            const int cy = (rn.nextFloat() < mossTop) ? rn.nextInt(juce::jmax(1, h / 3)) : rn.nextInt(h);
            const float rad = 1.3f + rn.nextFloat() * 2.6f;

            for (int dy = -4; dy <= 4; ++dy)
            {
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const float d = std::hypot((float) dx, (float) dy * 1.1f);
                    if (d >= rad)
                        continue;
                    if (!(d < rad - 0.9f || rn.nextFloat() < 0.55f))
                        continue;

                    const int X = cx + dx;
                    const int Y = cy + dy;
                    if (X < 0 || X >= w || Y < 0 || Y >= h)
                        continue;

                    const size_t i = (size_t) (Y * w + X);
                    if (mortar[i] != 0 && rn.nextFloat() < 0.5f)
                        continue;

                    const int mi = rn.nextInt(rn.nextFloat() < 0.15f ? 4 : 3);
                    pix[i] = rgb(MO[mi][0], MO[mi][1], MO[mi][2]);
                }
            }
        }

        // Un litere de mousse sur la toute premiere ligne
        for (int x = 0; x < w; ++x)
        {
            if (rn.nextFloat() < mossTop * 0.5f)
            {
                const int mi = rn.nextInt(3);
                pix[(size_t) x] = rgb(MO[mi][0], MO[mi][1], MO[mi][2]);
            }
        }

        juce::Image img(juce::Image::ARGB, w, h, true);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.setPixelAt(x, y, pix[(size_t) (y * w + x)]);

        return img;
    }

    // Retourne une image (miroir horizontal et/ou vertical)
    juce::Image flipImage(const juce::Image& src, bool flipX, bool flipY)
    {
        const int w = src.getWidth();
        const int h = src.getHeight();
        juce::Image out(src.getFormat(), w, h, true);

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.setPixelAt(flipX ? (w - 1 - x) : x, flipY ? (h - 1 - y) : y, src.getPixelAt(x, y));

        return out;
    }

    // Echange lignes et colonnes (sert a faire le morceau vertical a partir de l'horizontal)
    juce::Image transposeImage(const juce::Image& src)
    {
        const int w = src.getWidth();
        const int h = src.getHeight();
        juce::Image out(src.getFormat(), h, w, true);

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.setPixelAt(y, x, src.getPixelAt(x, y));

        return out;
    }

    // Ajoute un polygone ferme : x1, y1, x2, y2, ...
    void addPoly(juce::Path& p, std::initializer_list<float> pts)
    {
        auto it = pts.begin();
        p.startNewSubPath(*it, *(it + 1));
        it += 2;
        for (; it != pts.end(); it += 2)
            p.lineTo(*it, *(it + 1));
        p.closeSubPath();
    }

    // Logo pixel-art "Cinzel pixel" : grille de 77 x 17 pixels, lettres B L O O M en arc.
    // Chaque entree = une ligne de pixels pleins {x, y, largeur}. 'grow' epaissit chaque pixel (contour / ombre).
    juce::Path makeLogoPath(float grow = 0.0f, float ox = 0.0f, float oy = 0.0f, float u = 1.0f, bool snap = false)
    {
        static const int runs[][3] = { {0,4,9},{1,5,3},{7,5,3},{1,6,3},{8,6,3},{1,7,3},{8,7,3},{1,8,3},{7,8,3},{1,9,7},{1,10,3},{7,10,3},{1,11,3},{8,11,3},{1,12,3},{8,12,3},{1,13,3},{8,13,3},{1,14,3},{7,14,3},{1,15,3},{6,15,3},{0,16,9},{13,1,7},{14,2,3},{14,3,3},{14,4,3},{14,5,3},{14,6,3},{14,7,3},{14,8,3},{14,9,3},{14,10,3},{14,11,3},{22,11,2},{14,12,3},{21,12,3},{13,13,11},{30,0,7},{28,1,3},{36,1,3},{27,2,3},{37,2,3},{27,3,3},{37,3,3},{26,4,3},{38,4,3},{26,5,3},{38,5,3},{26,6,3},{38,6,3},{26,7,3},{38,7,3},{26,8,3},{38,8,3},{27,9,3},{37,9,3},{27,10,3},{37,10,3},{28,11,3},{36,11,3},{30,12,7},{47,0,7},{45,1,3},{53,1,3},{44,2,3},{54,2,3},{44,3,3},{54,3,3},{43,4,3},{55,4,3},{43,5,3},{55,5,3},{43,6,3},{55,6,3},{43,7,3},{55,7,3},{43,8,3},{55,8,3},{44,9,3},{54,9,3},{44,10,3},{54,10,3},{45,11,3},{53,11,3},{47,12,7},{60,3,5},{72,3,5},{62,4,5},{72,4,4},{62,5,5},{72,5,4},{62,6,2},{65,6,3},{71,6,1},{73,6,3},{62,7,2},{65,7,3},{71,7,1},{73,7,3},{62,8,2},{65,8,3},{71,8,1},{73,8,3},{62,9,2},{66,9,3},{70,9,1},{73,9,3},{62,10,2},{66,10,3},{70,10,1},{73,10,3},{62,11,2},{66,11,3},{70,11,1},{73,11,3},{62,12,2},{67,12,4},{73,12,3},{62,13,2},{67,13,3},{73,13,3},{62,14,2},{68,14,3},{73,14,3},{60,15,5},{72,15,5} };
        juce::Path p;
        p.setUsingNonZeroWinding(true);
        for (auto& r : runs)
        {
            float x0 = ox + ((float) r[0] - grow) * u, x1 = ox + ((float) (r[0] + r[2]) + grow) * u;
            float y0 = oy + ((float) r[1] - grow) * u, y1 = oy + ((float) (r[1] + 1) + grow) * u;
            if (snap) // bords sur des pixels entiers : lettres nettes comme dans le prototype
            {
                x0 = std::round(x0); x1 = std::round(x1); y0 = std::round(y0); y1 = std::round(y1);
            }
            p.addRectangle(x0, y0, x1 - x0, y1 - y0);
        }
        return p;
    }

    int texelScale(int width)      { return juce::jmax(1, width / 300); }

    // Zone occupee par le logo (en pixels) : plus grand et un peu plus bas qu'avant
    // Taille d'un "pixel" du logo (entier, pour des bords nets)
    float logoUnit(int width) { return 0.479f * (float) width / 77.0f; }

    juce::Rectangle<float> logoBounds(int width)
    {
        const float W = (float) width;
        const float u = logoUnit(width);
        const float lw = 77.0f * u;
        return { (W - lw) * 0.5f, 0.0862f * W, lw, 17.0f * u };
    }

    // ---- Le curseur : une petite epee pixel-art (lame argentee ; doree quand on survole quelque chose) ----
    juce::Image makeSwordImage(bool glow)
    {
        const int N = 16;
        juce::uint32 col[N][N] = {};
        bool has[N][N] = {};
        auto setPx = [&](int x, int y, juce::uint32 c)
        {
            if (x >= 0 && y >= 0 && x < N && y < N) { col[y][x] = c; has[y][x] = true; }
        };

        const juce::uint32 bl[3] = { glow ? 0xfffff4dcu : 0xfff1f4fbu,
                                     glow ? 0xffffd27au : 0xffaab2c4u,
                                     glow ? 0xffc9822au : 0xff5d6578u };
        for (int i = 0; i <= 9; ++i)
        {
            setPx(i, i, bl[1]);
            if (i > 0) { setPx(i + 1, i, bl[0]); setPx(i, i + 1, bl[2]); }
        }
        setPx(0, 0, bl[0]);
        for (int k = -3; k <= 3; ++k)
        {
            setPx(11 + k, 11 - k, k == 0 ? 0xffffb347u : 0xff7a5a3au);
            setPx(12 + k, 11 - k, 0xff4a3a2au);
        }
        setPx(12, 12, 0xff5b3f86u); setPx(13, 13, 0xff3f2b60u);
        setPx(12, 13, 0xff2c1d44u); setPx(13, 12, 0xff2c1d44u);
        setPx(14, 14, 0xffffb347u); setPx(15, 14, 0xffc9822au);
        setPx(14, 15, 0xffc9822au); setPx(15, 15, 0xff8a4a1cu);

        juce::Image img(juce::Image::ARGB, N * 2, N * 2, true);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                juce::Colour c = juce::Colours::transparentBlack;
                if (has[y][x])
                    c = juce::Colour(col[y][x]);
                else
                {
                    const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
                    for (int d = 0; d < 4; ++d)
                    {
                        const int nx = x + dx[d], ny = y + dy[d];
                        if (nx >= 0 && ny >= 0 && nx < N && ny < N && has[ny][nx])
                            c = juce::Colour(0xff0b0910u);
                    }
                }
                for (int oy = 0; oy < 2; ++oy)
                    for (int ox = 0; ox < 2; ++ox)
                        img.setPixelAt(x * 2 + ox, y * 2 + oy, c);
            }
        return img;
    }

    // les deux curseurs epee sont fabriques une seule fois (avant : a chaque mouvement de souris -> lag)
    juce::MouseCursor swordCursor(bool glow)
    {
        static juce::MouseCursor* cur[2] = { nullptr, nullptr };   // jamais detruits volontairement
        auto& c = cur[glow ? 1 : 0];
        if (c == nullptr)
            c = new juce::MouseCursor(makeSwordImage(glow), 1, 1);
        return *c;
    }

    // halo doux (degrade radial) autour d'un point lumineux
    void softGlow(juce::Graphics& g, float cx, float cy, float r, juce::Colour c)
    {
        juce::ColourGradient gr(c, cx, cy, c.withAlpha(0.0f), cx + r, cy, true);
        g.setGradientFill(gr);
        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    // rectangle aux coins coupes (style pixel-art)
    juce::Path chamferPath(juce::Rectangle<float> r, float c)
    {
        juce::Path p;
        p.startNewSubPath(r.getX() + c, r.getY());
        p.lineTo(r.getRight() - c, r.getY());
        p.lineTo(r.getRight(), r.getY() + c);
        p.lineTo(r.getRight(), r.getBottom() - c);
        p.lineTo(r.getRight() - c, r.getBottom());
        p.lineTo(r.getX() + c, r.getBottom());
        p.lineTo(r.getX(), r.getBottom() - c);
        p.lineTo(r.getX(), r.getY() + c);
        p.closeSubPath();
        return p;
    }

}

//==============================================================================
// Etape 2 : les arcs autour de la scene, relies aux vrais reglages
//==============================================================================
class BloomArcsLayer : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    // donne la position a l'ecran de l'objet du monde lie a un reglage (point de vue 2)
    using AnchorSource = std::function<bool(int, float&, float&)>;

    BloomArcsLayer(juce::AudioProcessorValueTreeState& apvts, std::atomic<int>& hoverOut)
        : hoverRef(hoverOut)
    {
        struct Def
        {
            const char* id; const char* label; juce::uint32 colour;
            float p[8];          // 4 points de la courbe (fractions de la fenetre)
            float lx, ly;        // position du texte
            float defNorm;       // valeur au double-clic (0 a 1)
            const char* help;
        };

        static const Def defs[] = {
            { "space", "SPACE", 0xffa7a0bd, { .13f, .74f, .03f, .60f, .03f, .32f, .16f, .12f }, .09f, .68f, 0.0f,
              "Creuse le grave boueux et \xC3\xA9largit la st\xC3\xA9r\xC3\xA9o" },
            { "mix", "MIX", 0xffddd3e4, { .22f, .76f, .13f, .63f, .13f, .38f, .235f, .20f }, .265f, .78f, 1.0f,
              "Dosage entre ton son (0 %) et BLOOM (100 %)" },
            { "presence", "PRESENCE", 0xffffd27a, { .87f, .74f, .97f, .60f, .97f, .32f, .84f, .12f }, .91f, .68f, 0.0f,
              "D\xC3\xA9""finition et \xC3\xA9nergie vers 2,5 kHz" },
            { "enhance", "ENHANCE", 0xffe0607e, { .88f, .90f, .85f, .945f, .78f, .968f, .71f, .958f }, .88f, .865f, 0.0f,
              "Colle, sature et densifie toute la prod" },
            { "air", "AIR", 0xffb9a6ff, { .31f, .27f, .41f, .14f, .59f, .14f, .69f, .27f }, .50f, .235f, 0.0f,
              "Brillance soyeuse au-dessus de 12 kHz" },
            { "weight", "WEIGHT", 0xffff8a3a, { .35f, .88f, .43f, .96f, .57f, .96f, .65f, .88f }, .50f, .875f, 0.0f,
              "Poids et profondeur dans le grave" },
            { "warmth", "WARMTH", 0xffff5a2a, { .12f, .90f, .15f, .945f, .22f, .968f, .29f, .958f }, .12f, .865f, 0.0f,
              "Chaleur douce dans le grave" }
        };

        for (const auto& d : defs)
        {
            auto* param = apvts.getParameter(d.id);
            if (param == nullptr)
                continue;

            auto gauge = std::make_unique<Gauge>();
            gauge->label = d.label;
            gauge->help = juce::String(juce::CharPointer_UTF8(d.help));
            gauge->colour = juce::Colour(d.colour);
            gauge->param = param;
            for (int i = 0; i < 4; ++i)
                gauge->frac[i] = { d.p[i * 2], d.p[i * 2 + 1] };
            gauge->labelFrac = { d.lx, d.ly };
            gauge->defNorm = d.defNorm;
            gauge->norm = param->getValue();
            {
                const juce::String id(d.id);
                gauge->idx = id == "weight" ? 0 : id == "warmth" ? 1 : id == "space" ? 2 : id == "presence" ? 3
                           : id == "air" ? 4 : id == "enhance" ? 5 : 6;
            }

            Gauge* raw = gauge.get();
            gauge->attach = std::make_unique<juce::ParameterAttachment>(*param,
                [this, raw](float newValue)
                {
                    raw->norm = raw->param->convertTo0to1(newValue);
                    repaint();
                });
            gauge->attach->sendInitialUpdate();

            gauges.push_back(std::move(gauge));
        }
    }

    ~BloomArcsLayer() override { stopTimer(); }

    void setAnchorSource(AnchorSource src) { anchorSource = std::move(src); }

    // Point de vue 2 : plus d'arcs, on regle en touchant les objets du monde
    void setWorldMode(bool on)
    {
        if (worldMode == on)
            return;

        if (active != nullptr)
        {
            active->attach->endGesture();
            active = nullptr;
        }
        hovered = nullptr;
        linkGauge = nullptr;
        linkVis = false;
        linkArea = {};
        stopTimer();
        publishHover();
        worldMode = on;
        setMouseCursor(swordCursor(false));

        if (on)
        {
            lastTick = juce::Time::getMillisecondCounterHiRes();
            for (auto& up : gauges)
                up->has = false;
            startTimerHz(30);
        }
        else
        {
            stopTimer();
        }
        repaint();
    }

    void resized() override
    {
        rebuildGeometry();
    }

    bool hitTest(int x, int y) override
    {
        return pick({ (float) x, (float) y }) != nullptr;
    }

    juce::String getTooltip() override { return {}; } // l'aide est dessinee par la couche elle-meme (petite bulle)

    void paint(juce::Graphics& g) override
    {
        BLOOM_PERF_SCOPE("arcs : dessin");
        if (worldMode)
        {
            paintWorld(g);
            drawTip(g);
            return;
        }

        if (tracks.isValid())
            g.drawImageAt(tracks, 0, 0);

        const float fs = 10.0f;   // taille du nom
        const float fv = 15.0f;   // taille de la valeur
        const float W = (float) getWidth();
        const float H = (float) getHeight();

        for (auto& up : gauges)
        {
            Gauge& ga = *up;
            if (ga.pts.size() < 2)
                continue;

            const bool lit = (&ga == hovered) || (&ga == active);
            const float v = ga.norm;

            // partie remplie de l'arc
            if (v > 0.002f)
            {
                juce::Path fill;
                fill.startNewSubPath(ga.pts[0]);
                const float target = v * ga.length;
                for (size_t i = 1; i < ga.pts.size() && ga.cum[i] <= target; ++i)
                    fill.lineTo(ga.pts[i]);
                fill.lineTo(pointAt(ga, v));

                const juce::PathStrokeType st(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
                g.setColour(ga.colour.withAlpha(lit ? 0.22f : 0.14f));
                g.strokePath(fill, juce::PathStrokeType(13.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                g.setColour(ga.colour.withAlpha(0.30f));
                g.strokePath(fill, juce::PathStrokeType(9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                g.setColour(ga.colour);
                g.strokePath(fill, st);
            }

            // poignee en losange
            const auto hp = pointAt(ga, v);
            juce::ColourGradient glow(ga.colour.withAlpha(lit ? 0.55f : 0.35f), hp.x, hp.y,
                                      ga.colour.withAlpha(0.0f), hp.x + 15.0f, hp.y, true);
            g.setGradientFill(glow);
            g.fillEllipse(hp.x - 15.0f, hp.y - 15.0f, 30.0f, 30.0f);

            juce::Path outer, inner;
            outer.startNewSubPath(hp.x, hp.y - 8.5f);
            outer.lineTo(hp.x + 8.5f, hp.y);
            outer.lineTo(hp.x, hp.y + 8.5f);
            outer.lineTo(hp.x - 8.5f, hp.y);
            outer.closeSubPath();
            inner.startNewSubPath(hp.x, hp.y - 3.5f);
            inner.lineTo(hp.x + 3.5f, hp.y);
            inner.lineTo(hp.x, hp.y + 3.5f);
            inner.lineTo(hp.x - 3.5f, hp.y);
            inner.closeSubPath();
            g.setColour(juce::Colour(0xff1a171f));
            g.fillPath(outer);
            g.setColour(ga.colour);
            g.strokePath(outer, juce::PathStrokeType(2.0f));
            g.fillPath(inner);

            // nom et valeur
            const float tx = ga.labelFrac.x * W;
            const float ty = ga.labelFrac.y * H;

            g.setFont(juce::Font(juce::FontOptions(fs, juce::Font::bold)).withExtraKerningFactor(0.18f));
            drawOutlined(g, ga.label, juce::Rectangle<float>(tx - 70.0f, ty - fs - 2.0f, 140.0f, fs + 6.0f),
                         lit ? ga.colour : juce::Colour(0xffa59fb5));

            g.setFont(juce::Font(juce::FontOptions(fv, juce::Font::bold)));
            drawOutlined(g, valueText(ga), juce::Rectangle<float>(tx - 70.0f, ty + 3.0f, 140.0f, fv + 6.0f),
                         juce::Colour(0xfffff4e6));
        }

        paintLink(g);
        drawTip(g);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        auto* g = pick(e.position);
        mousePos = e.position;
        if (g != hovered)
        {
            const Gauge* old = hovered;
            hovered = g;
            hideTip(false);

            setMouseCursor(g != nullptr ? (worldMode ? juce::MouseCursor(juce::MouseCursor::UpDownResizeCursor) : swordCursor(true))
                                        : swordCursor(false));
            publishHover();
            updateLinkTimer();
            repaintHoverChange(old, g);
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hovered != nullptr && active == nullptr)
        {
            const Gauge* old = hovered;
            hovered = nullptr;
            hideTip(false);
            publishHover();
            updateLinkTimer();
            repaintHoverChange(old, nullptr);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        active = pick(e.position);
        if (active == nullptr)
            return;

        hideTip();

        hovered = active;
        publishHover();
        updateLinkTimer();
        active->attach->beginGesture();
        if (worldMode)
        {
            dragStartY = e.position.y;
            dragStartNorm = active->norm;
            touched = true;
        }
        else
        {
            setNorm(*active, nearestFrac(*active, e.position), false);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (active == nullptr)
            return;

        if (worldMode)
            setNorm(*active, dragStartNorm + (dragStartY - e.position.y) / 200.0f, false);
        else
            setNorm(*active, nearestFrac(*active, e.position), false);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (active != nullptr)
        {
            active->attach->endGesture();
            active = nullptr;
            publishHover();
            updateLinkTimer();
            repaint();
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (auto* g = pick(e.position))
            setNorm(*g, g->defNorm, true);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (auto* g = pick(e.position))
        {
            if (wheel.deltaY != 0.0f)
                setNorm(*g, g->norm + (wheel.deltaY > 0.0f ? 0.02f : -0.02f), true);
        }
    }

private:
    struct Gauge
    {
        juce::String label, help;
        juce::Colour colour;
        juce::RangedAudioParameter* param = nullptr;
        juce::Point<float> frac[4];
        juce::Point<float> labelFrac;
        float defNorm = 0.0f;
        float norm = 0.0f;
        float wx = 0.0f, wy = 0.0f;            // position lissee de l'objet du monde (pixels)
        bool has = false, vis = false;
        juce::Rectangle<int> lastArea;
        int idx = -1;                          // numero du reglage cote scene 3D
        std::vector<juce::Point<float>> pts;   // la courbe decoupee en petits segments (en pixels)
        std::vector<float> cum;                // longueur cumulee a chaque point
        float length = 0.0f;
        std::unique_ptr<juce::ParameterAttachment> attach;
    };

    static void drawOutlined(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, juce::Colour c,
                             juce::Justification j = juce::Justification::centred)
    {
        g.setColour(juce::Colour(0xff07060b));
        const float o[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 } };
        for (auto& d : o)
            g.drawText(text, r.translated(d[0], d[1]), j, false);
        g.setColour(c);
        g.drawText(text, r, j, false);
    }

    juce::String valueText(const Gauge& ga) const
    {
        juce::String t = ga.param->getText(ga.norm, 20);
        const juce::String unit = ga.param->getLabel().trim();
        if (unit.isNotEmpty() && !t.endsWithIgnoreCase(unit))
            t << " " << unit;
        return t;
    }

    void setNorm(Gauge& ga, float n, bool complete)
    {
        n = juce::jlimit(0.0f, 1.0f, n);
        ga.norm = n;
        const float v = ga.param->convertFrom0to1(n);
        if (complete)
            ga.attach->setValueAsCompleteGesture(v);
        else
            ga.attach->setValueAsPartOfGesture(v);
        repaint();
    }

    juce::Point<float> pointAt(const Gauge& ga, float f) const
    {
        if (ga.pts.empty())
            return {};
        const float target = juce::jlimit(0.0f, 1.0f, f) * ga.length;
        auto it = std::upper_bound(ga.cum.begin(), ga.cum.end(), target);
        size_t i = (size_t) (it - ga.cum.begin());
        if (i == 0) return ga.pts.front();
        if (i >= ga.pts.size()) return ga.pts.back();
        const float seg = ga.cum[i] - ga.cum[i - 1];
        const float t = seg > 0.0f ? (target - ga.cum[i - 1]) / seg : 0.0f;
        return ga.pts[i - 1] + (ga.pts[i] - ga.pts[i - 1]) * t;
    }

    // Position (0 a 1) du point de l'arc le plus proche de la souris
    float nearestFrac(const Gauge& ga, juce::Point<float> pos, float* distOut = nullptr) const
    {
        float best = 1.0e12f;
        size_t bi = 0;
        for (size_t i = 0; i < ga.pts.size(); ++i)
        {
            const float d = ga.pts[i].getDistanceSquaredFrom(pos);
            if (d < best) { best = d; bi = i; }
        }
        if (distOut != nullptr)
            *distOut = std::sqrt(best);
        return ga.length > 0.0f ? ga.cum[bi] / ga.length : 0.0f;
    }

    Gauge* gaugeAt(juce::Point<float> pos, float maxDist) const
    {
        Gauge* best = nullptr;
        float bestDist = maxDist;
        for (auto& up : gauges)
        {
            float d = 1.0e9f;
            nearestFrac(*up, pos, &d);
            if (d <= bestDist) { bestDist = d; best = up.get(); }
        }
        return best;
    }

    void rebuildGeometry()
    {
        const int W = getWidth();
        const int H = getHeight();
        if (W <= 0 || H <= 0)
            return;

        tracks = juce::Image(juce::Image::ARGB, W, H, true);
        juce::Graphics g(tracks);

        for (auto& up : gauges)
        {
            Gauge& ga = *up;
            const juce::Point<float> P0(ga.frac[0].x * W, ga.frac[0].y * H);
            const juce::Point<float> P1(ga.frac[1].x * W, ga.frac[1].y * H);
            const juce::Point<float> P2(ga.frac[2].x * W, ga.frac[2].y * H);
            const juce::Point<float> P3(ga.frac[3].x * W, ga.frac[3].y * H);

            const int N = 240;
            ga.pts.clear();
            ga.cum.clear();
            ga.length = 0.0f;
            for (int i = 0; i <= N; ++i)
            {
                const float t = (float) i / (float) N;
                const float u = 1.0f - t;
                const auto p = P0 * (u * u * u) + P1 * (3.0f * u * u * t) + P2 * (3.0f * u * t * t) + P3 * (t * t * t);
                if (!ga.pts.empty())
                    ga.length += p.getDistanceFrom(ga.pts.back());
                ga.pts.push_back(p);
                ga.cum.push_back(ga.length);
            }

            juce::Path path;
            path.startNewSubPath(P0);
            path.cubicTo(P1, P2, P3);

            const auto rounded = [](float w) { return juce::PathStrokeType(w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };

            g.setColour(juce::Colour(0x99000000));
            g.strokePath(path, rounded(13.0f), juce::AffineTransform::translation(0.0f, 4.0f)); // ombre
            g.setColour(juce::Colour(0xff0c0a10));
            g.strokePath(path, rounded(11.0f));
            g.setColour(juce::Colour(0xff3a3542));
            g.strokePath(path, rounded(9.0f));
            g.setColour(juce::Colour(0xff16131b));
            g.strokePath(path, rounded(7.0f));

            // petites graduations
            g.setColour(juce::Colour(0x26ffffff));
            for (float d = 6.0f; d < ga.length - 3.0f; d += 13.5f)
            {
                const float f = d / ga.length;
                const auto a = pointAt(ga, f);
                const auto b = pointAt(ga, juce::jmin(1.0f, f + 0.004f));
                auto dir = b - a;
                const float n = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (n < 0.0001f)
                    continue;
                const juce::Point<float> nrm(-dir.y / n, dir.x / n);
                g.drawLine(juce::Line<float>(a - nrm * 3.0f, a + nrm * 3.0f), 1.5f);
            }
        }
    }

    void hideTip(bool doRepaint = true)
    {
        if (tipShown)
        {
            tipShown = false;
            if (doRepaint)
                repaint(lastTipRect.getSmallestIntegerContainer().expanded(2));
        }
    }

    // ne redessine que ce qui change (avant : toute la fenetre a chaque survol -> a-coup)
    void repaintHoverChange(const Gauge* a, const Gauge* b)
    {
        if (!worldMode)
        {
            repaint();
            return;
        }
        juce::Rectangle<int> r = lastTipRect.getSmallestIntegerContainer().expanded(2);
        for (const Gauge* ga : { a, b })
            if (ga != nullptr)
            {
                const auto wa = worldArea(*ga);
                r = r.isEmpty() ? wa : r.getUnion(wa);
            }
        if (!r.isEmpty())
            repaint(r);
    }

    // petite bulle d'aide (une ligne) a cote du curseur
    void drawTip(juce::Graphics& g)
    {
        if (!tipShown || hovered == nullptr || hovered->help.isEmpty())
            return;

        const juce::Font f(juce::FontOptions(11.5f));
        const float w = textWidth(f, hovered->help) + 20.0f, h = 22.0f;
        float x = tipPos.x + 14.0f, y = tipPos.y + 22.0f;
        if (x + w > (float) getWidth() - 10.0f) x = tipPos.x - w - 10.0f;
        if (y + h > (float) getHeight() - 10.0f) y = tipPos.y - h - 10.0f;
        x = juce::jmax(10.0f, x);
        const juce::Rectangle<float> r(x, y, w, h);
        lastTipRect = r;

        g.setColour(juce::Colour(0xf20d0b12));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colour(0xb3ffb347));
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(juce::Colour(0xffe9e2f1));
        g.setFont(f);
        g.drawText(hovered->help, r, juce::Justification::centred, false);
    }

    bool tipShown = false;
    juce::Point<float> mousePos, tipPos;
    juce::Rectangle<float> lastTipRect;

    static float textWidth(const juce::Font& f, const juce::String& t)
    {
        juce::GlyphArrangement arr;
        arr.addLineOfText(f, t, 0.0f, 0.0f);
        return arr.getBoundingBox(0, -1, true).getWidth();
    }

    // ---------------- point de vue 2 : reglages poses sur les objets du monde ----------------
    Gauge* pick(juce::Point<float> pos) const
    {
        if (worldMode)
        {
            // le reglage deja survole reste "attrape" tant qu'on ne s'en eloigne pas franchement
            const Gauge* held = active != nullptr ? active : hovered;
            if (held != nullptr && held->vis && pos.getDistanceFrom({ held->wx, held->wy }) <= 44.0f)
                return const_cast<Gauge*>(held);
            return worldGaugeAt(pos, 30.0f);
        }
        return gaugeAt(pos, 20.0f);
    }

    Gauge* worldGaugeAt(juce::Point<float> pos, float maxDist) const
    {
        Gauge* best = nullptr;
        float bestDist = maxDist;
        for (auto& up : gauges)
        {
            if (!up->vis)
                continue;
            const float d = pos.getDistanceFrom({ up->wx, up->wy });
            if (d <= bestDist) { bestDist = d; best = up.get(); }
        }
        return best;
    }

    juce::Rectangle<int> worldArea(const Gauge& ga) const
    {
        // assez grand pour le halo du losange (jusqu'a ~50 px quand il est survole) et le texte a droite
        return juce::Rectangle<int>((int) ga.wx - 54, (int) ga.wy - 54, 54 + 200, 108).getIntersection(getLocalBounds());
    }

    juce::Rectangle<int> introArea() const
    {
        return juce::Rectangle<int>(0, (int) ((float) getHeight() * 0.80f), getWidth(), (int) ((float) getHeight() * 0.17f));
    }

    // ---- Trait en pointille : de l'arc survole jusqu'a l'objet du monde qu'il controle ----
    void updateLinkTimer()
    {
        if (worldMode)
            return;

        const Gauge* lit = active != nullptr ? active : hovered;
        if (lit != nullptr)
        {
            if (lit != linkGauge)
            {
                linkGauge = lit;
                linkHas = false;
                linkVis = false;
            }
            if (!isTimerRunning())
            {
                lastTick = juce::Time::getMillisecondCounterHiRes();
                startTimerHz(30);
            }
        }
        else
        {
            linkGauge = nullptr;
            linkVis = false;
            if (isTimerRunning())
                stopTimer();
            if (!linkArea.isEmpty())
            {
                repaint(linkArea);
                linkArea = {};
            }
        }
    }

    void linkTick()
    {
        if (linkGauge == nullptr || !isShowing())
            return;

        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = juce::jlimit(0.0f, 0.1f, (float) ((now - lastTick) / 1000.0));
        lastTick = now;
        linkPhase += dt;

        const Gauge& ga = *linkGauge;
        float ax = 0.0f, ay = 0.0f;
        const bool ok = anchorSource && anchorSource(ga.idx, ax, ay) && std::isfinite(ax) && std::isfinite(ay);
        if (!ok)
        {
            linkVis = false;
            return;
        }

        const float W = (float) getWidth(), H = (float) getHeight();
        const float tx = juce::jlimit(24.0f, juce::jmax(24.0f, W - 96.0f), ax);
        const float ty = juce::jlimit(46.0f, juce::jmax(46.0f, H - 34.0f), ay);
        if (!linkHas) { linkX = tx; linkY = ty; linkHas = true; }
        else
        {
            const float f = juce::jlimit(0.0f, 1.0f, dt * 9.0f);
            linkX += (tx - linkX) * f;
            linkY += (ty - linkY) * f;
        }
        linkVis = true;

        const auto hp = pointAt(ga, ga.norm);
        const auto area = juce::Rectangle<float>(hp, juce::Point<float>(linkX, linkY)).getSmallestIntegerContainer()
                              .expanded(24).getIntersection(getLocalBounds());
        repaint(area.getUnion(linkArea));
        linkArea = area;
    }

    void paintLink(juce::Graphics& g)
    {
        if (worldMode || linkGauge == nullptr || !linkVis)
            return;

        const Gauge& ga = *linkGauge;
        const auto hp = pointAt(ga, ga.norm);

        juce::Path line, dashed, halo;
        line.startNewSubPath(hp);
        line.lineTo(linkX, linkY);
        const float dash[2] = { 2.0f, 5.0f };
        juce::PathStrokeType(1.4f).createDashedStroke(dashed, line, dash, 2);
        juce::PathStrokeType(3.6f).createDashedStroke(halo, line, dash, 2);
        g.setColour(ga.colour.withAlpha(0.22f));
        g.fillPath(halo);
        g.setColour(ga.colour.withAlpha(0.95f));
        g.fillPath(dashed);

        // le repere sur l'objet du monde
        const float pulse = 0.9f + 0.2f * std::sin(linkPhase * 5.7f);
        softGlow(g, linkX, linkY, 16.0f * pulse, ga.colour.withAlpha(0.5f));
        auto diamond = [](float cx, float cy, float h)
        {
            juce::Path p;
            p.startNewSubPath(cx, cy - h);
            p.lineTo(cx + h, cy);
            p.lineTo(cx, cy + h);
            p.lineTo(cx - h, cy);
            p.closeSubPath();
            return p;
        };
        g.setColour(ga.colour.withAlpha(0.95f));
        g.strokePath(diamond(linkX, linkY, 7.0f * pulse), juce::PathStrokeType(2.0f));
        g.fillPath(diamond(linkX, linkY, 2.6f));
    }

    void timerCallback() override
    {
        BLOOM_PERF_SCOPE("arcs : minuterie");
        if (!worldMode)
        {
            linkTick();
            return;
        }
        if (!isShowing())
            return;

        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = juce::jlimit(0.0f, 0.1f, (float) ((now - lastTick) / 1000.0));
        lastTick = now;
        worldClock += dt;
        if (worldClock > 9.0f)
            touched = true;

        const float W = (float) getWidth(), H = (float) getHeight();
        const float f = juce::jlimit(0.0f, 1.0f, dt * 9.0f);

        for (auto& up : gauges)
        {
            Gauge& ga = *up;
            float ax = 0.0f, ay = 0.0f;
            const bool ok = anchorSource && anchorSource(ga.idx, ax, ay) && std::isfinite(ax) && std::isfinite(ay);
            // le reglage sous la souris ne bouge plus (sinon le corbeau, qui vole, s'echappe)
            const bool held = (&ga == hovered) || (&ga == active);
            if (ok && held && ga.has)
            {
                // on reste en place
            }
            else if (ok)
            {
                const float tx = juce::jlimit(24.0f, juce::jmax(24.0f, W - 96.0f), ax);
                const float ty = juce::jlimit(46.0f, juce::jmax(46.0f, H - 34.0f), ay);
                if (!ga.has) { ga.wx = tx; ga.wy = ty; ga.has = true; }
                else         { ga.wx += (tx - ga.wx) * f; ga.wy += (ty - ga.wy) * f; }
            }
            ga.vis = (ok || held) && ga.has;

            const auto area = ga.vis ? worldArea(ga) : juce::Rectangle<int>();
            // chaque losange redessine seulement sa petite zone (pas un grand rectangle qui couvre tout)
            const auto own = ga.lastArea.isEmpty() ? area : (area.isEmpty() ? ga.lastArea : ga.lastArea.getUnion(area));
            if (!own.isEmpty())
                repaint(own);
            ga.lastArea = area;
        }

    }

    void paintWorld(juce::Graphics& g)
    {
        for (auto& up : gauges)
        {
            Gauge& ga = *up;
            if (!ga.vis)
                continue;

            const bool lit = (&ga == hovered) || (&ga == active);
            float pulse = 1.0f;
            if (!touched)
            {
                const float ph = std::fmod(juce::jmax(0.0f, worldClock - (float) ga.idx * 0.35f), 1.6f) / 1.6f;
                pulse = 1.0f + 0.7f * (0.5f - 0.5f * std::cos(ph * juce::MathConstants<float>::twoPi));
            }

            const float half = 6.4f * (lit ? 1.55f : 1.0f) * pulse;
            const float x = ga.wx, y = ga.wy;

            juce::ColourGradient glow(ga.colour.withAlpha(lit ? 0.6f : 0.35f * pulse), x, y,
                                      ga.colour.withAlpha(0.0f), x + half * 3.0f, y, true);
            g.setGradientFill(glow);
            g.fillEllipse(x - half * 3.0f, y - half * 3.0f, half * 6.0f, half * 6.0f);

            auto diamond = [](float cx, float cy, float h)
            {
                juce::Path p;
                p.startNewSubPath(cx, cy - h);
                p.lineTo(cx + h, cy);
                p.lineTo(cx, cy + h);
                p.lineTo(cx - h, cy);
                p.closeSubPath();
                return p;
            };
            g.setColour(juce::Colour(0xff0b0910));
            g.fillPath(diamond(x, y, half + 2.2f));
            g.setColour(ga.colour.withAlpha(lit ? 1.0f : 0.78f));
            g.fillPath(diamond(x, y, half));

            const float tx = x + half + 9.0f;
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)).withExtraKerningFactor(0.22f));
            drawOutlined(g, ga.label, juce::Rectangle<float>(tx, y - 8.0f, 90.0f, 16.0f),
                         lit ? ga.colour : juce::Colour(0xffa79fb3), juce::Justification::centredLeft);

            if (lit)
            {
                const float lw = textWidth(g.getCurrentFont(), ga.label);
                g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
                drawOutlined(g, valueText(ga), juce::Rectangle<float>(tx + lw + 8.0f, y - 8.0f, 90.0f, 16.0f),
                             juce::Colour(0xfffff4e6), juce::Justification::centredLeft);
            }
        }

        if (introAlpha > 0.0f)
        {
            const float W = (float) getWidth(), H = (float) getHeight();
            const float a = introAlpha;
            // texte clair avec une simple ombre dessous (le contour complet mangeait les lettres fines)
            auto shadowText = [&g, a](const juce::String& t, juce::Rectangle<float> r, juce::Colour c)
            {
                g.setColour(juce::Colour(0xff07060b).withAlpha(0.85f * a));
                g.drawText(t, r.translated(0.0f, 1.5f), juce::Justification::centred, false);
                g.drawText(t, r.translated(1.0f, 1.0f), juce::Justification::centred, false);
                g.drawText(t, r.translated(-1.0f, 1.0f), juce::Justification::centred, false);
                g.setColour(c);
                g.drawText(t, r, juce::Justification::centred, false);
            };
            g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)).withExtraKerningFactor(0.28f));
            shadowText(juce::String(juce::CharPointer_UTF8("TOUCHE LE MONDE POUR LE FA\xC3\x87" "ONNER")),
                       juce::Rectangle<float>(0.0f, H * 0.86f - 12.0f, W, 24.0f), juce::Colour(0xfffff4e6).withAlpha(a));

            const float ly = H * 0.925f;
            const juce::String hint(juce::CharPointer_UTF8("GLISSER POUR R\xC3\x89" "GLER"));
            g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)).withExtraKerningFactor(0.28f));
            const float tw = textWidth(g.getCurrentFont(), hint);
            shadowText(hint, juce::Rectangle<float>(W * 0.5f - tw * 0.5f - 4.0f, ly - 11.0f, tw + 8.0f, 22.0f),
                       juce::Colour(0xffd9d0e6).withAlpha(a));
            for (int side = -1; side <= 1; side += 2)
            {
                const float dx = W * 0.5f + (float) side * (tw * 0.5f + 20.0f);
                juce::Path d;
                d.startNewSubPath(dx, ly - 3.5f); d.lineTo(dx + 3.5f, ly); d.lineTo(dx, ly + 3.5f); d.lineTo(dx - 3.5f, ly);
                d.closeSubPath();
                g.setColour(juce::Colour(0xffffb347).withAlpha(a));
                g.fillPath(d);
                g.setGradientFill(juce::ColourGradient(juce::Colour(0x99ffb347).withMultipliedAlpha(a), dx + (float) side * 8.0f, ly,
                                                       juce::Colour(0x00ffb347), dx + (float) side * 60.0f, ly, false));
                g.fillRect(juce::Rectangle<float>(juce::jmin(dx + (float) side * 8.0f, dx + (float) side * 60.0f), ly - 0.5f, 52.0f, 1.0f));
            }
        }
    }

    const Gauge* linkGauge = nullptr;
    bool linkHas = false, linkVis = false;
    float linkX = 0.0f, linkY = 0.0f, linkPhase = 0.0f;
    juce::Rectangle<int> linkArea;
    bool worldMode = false, touched = false;
    float worldClock = 0.0f, introAlpha = 1.0f;
    double lastTick = 0.0;
    float dragStartY = 0.0f, dragStartNorm = 0.0f;
    AnchorSource anchorSource;

    std::vector<std::unique_ptr<Gauge>> gauges;
    juce::Image tracks;
    Gauge* hovered = nullptr;
    Gauge* active = nullptr;
    std::atomic<int>& hoverRef;

    void publishHover()
    {
        const Gauge* g = active != nullptr ? active : hovered;
        hoverRef.store(g != nullptr ? g->idx : -1);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomArcsLayer)
};

//==============================================================================
// La fiche explicative (clic sur le logo) : 2 pages, avec defilement
//==============================================================================
class BloomSheet : public juce::Component, private juce::Timer
{
public:
    explicit BloomSheet(juce::AudioProcessorValueTreeState& state) : apvts(state)
    {
        setVisible(false);
        setWantsKeyboardFocus(true);
        setMouseCursor(swordCursor(false));

        // champ de saisie de la cle (page licence)
        keyBox.setMultiLine(false);
        keyBox.setReturnKeyStartsNewLine(false);
        keyBox.setJustification(juce::Justification::centred);
        keyBox.setTextToShowWhenEmpty("XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX", juce::Colour(0xff5d5668));
        keyBox.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        keyBox.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        keyBox.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        keyBox.setColour(juce::TextEditor::textColourId, juce::Colour(0xfff1ecf7));
        keyBox.setColour(juce::TextEditor::highlightColourId, juce::Colour(0x66ffb347));
        keyBox.setColour(juce::CaretComponent::caretColourId, juce::Colour(0xffffb347));
        keyBox.onReturnKey = [this] { activateNow(); };
        keyBox.onEscapeKey = [this] { closeSheet(); };
        keyBox.onTextChange = [this] { if (licMsg.isNotEmpty() && !licBusy) { licMsg = {}; contentDirty[3] = true; repaint(); } };
        addChildComponent(keyBox);

        cards = {
            { "weight", juce::Colour(0xffff8a3a), "WEIGHT", "+10 dB", "Ajoute du poids et du corps dans les basses (vers\xC2\xA0""80\xC2\xA0Hz) et les recentre au milieu. Se retire un peu quand le kick tape, pour qu\xE2\x80\x99il reste net." },
            { "warmth", juce::Colour(0xffff5a2a), "WARMTH", "SATURATION", "R\xC3\xA9""chauffe les basses avec une saturation douce. Le son para\xC3\xAEt plus \xC3\xA9pais, sans devenir sale." },
            { "space", juce::Colour(0xffa7a0bd), "SPACE", "\xE2\x88\x92""5 dB", "Creuse les bas-m\xC3\xA9""diums boueux (vers\xC2\xA0""300\xC2\xA0Hz) pour faire de la place, et \xC3\xA9largit la st\xC3\xA9r\xC3\xA9o. Reste propre en mono." },
            { "presence", juce::Colour(0xffffd27a), "PRESENCE", "+6 dB", "Fait ressortir le haut des m\xC3\xA9""diums (vers\xC2\xA0""2,5\xC2\xA0kHz) : voix et instruments passent devant. Se calme tout seul si \xC3\xA7""a devient agressif." },
            { "air", juce::Colour(0xffb9a6ff), "AIR", "+7 dB", "Ajoute de la brillance tout en haut (au-dessus de 12\xC2\xA0kHz). Se calme tout seul sur les sons qui sifflent." },
            { "enhance", juce::Colour(0xffe0607e), "ENHANCE", "COLLE", "Compresse l\xC3\xA9g\xC3\xA8rement et sature toute la prod : le son devient plus dense et plus soud\xC3\xA9, en un seul bouton." },
            { "mix", juce::Colour(0xffddd3e4), "MIX", "0 \xE2\x86\x92 100 %", "M\xC3\xA9lange ton son d\xE2\x80\x99origine et celui de BLOOM. \xC3\x80 100\xC2\xA0%, tu n\xE2\x80\x99""entends que BLOOM." },
        };
        pages[0] = {
            { Row::regler, juce::Colour(0xffffd27a), "R\xC3\x89GLER", "Glisse sur un arc pour changer sa valeur. Double-clic : retour \xC3\xA0 la valeur par d\xC3\xA9""faut. La molette marche aussi.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
            { Row::gem, juce::Colour(0xffff7d97), "BYPASS", "Compare avec et sans BLOOM, \xC3\xA0 volume \xC3\xA9gal, pour juger honn\xC3\xAAtement.", juce::Colour(0xffff7d97), juce::Colour(0xffd0224a), juce::Colour(0xff6b0b20), juce::Colour(0xffff3b63), false },
            { Row::gem, juce::Colour(0xff8dffc8), "DELTA", "N\xE2\x80\x99\xC3\xA9""coute que ce que BLOOM ajoute ou retire au son.", juce::Colour(0xff8dffc8), juce::Colour(0xff19b06c), juce::Colour(0xff06452b), juce::Colour(0xff2cff9a), false },
            { Row::gem, juce::Colour(0xffbfe9f7), "FA\xC3\x87ONNER", "Les arcs disparaissent : tu r\xC3\xA8gles en touchant directement les objets du monde (la fleur, le feu, la cascade, le gobelin\xE2\x80\xA6).", juce::Colour(0xffffffff), juce::Colour(0xffbfe9f7), juce::Colour(0xff6fa6c0), juce::Colour(0xffaee8ff), false },
            { Row::menu, juce::Colour(0xffcfc3da), "MENU", "Passe la souris sur l\xE2\x80\x99ic\xC3\xB4ne en haut \xC3\xA0 droite : BYPASS, DELTA, FA\xC3\x87ONNER et le choix du monde.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
        };
        pages[1] = {
            { Row::dot, juce::Colour(0xffff8a3a), "DES MOTS, PAS DES HERTZ", "Six r\xC3\xA9glages qui parlent comme toi : poids, espace, pr\xC3\xA9sence, air, chaleur. Chacun combine plusieurs traitements d\xC3\xA9j\xC3\xA0 calibr\xC3\xA9s pour sonner juste.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
            { Row::dot, juce::Colour(0xffb9a6ff), "IL \xC3\x89""COUTE TA MUSIQUE", "WEIGHT s\xE2\x80\x99""efface sur le kick, PRESENCE et AIR se calment quand \xC3\xA7""a devient agressif. Un EQ reste fig\xC3\xA9, BLOOM s\xE2\x80\x99""adapte \xC3\xA0 chaque instant.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
            { Row::dot, juce::Colour(0xffa7a0bd), "IMPOSSIBLE DE TOUT CASSER", "Volume compens\xC3\xA9, st\xC3\xA9r\xC3\xA9o s\xC3\xBBre en mono, limiteur int\xC3\xA9gr\xC3\xA9 : la sortie ne d\xC3\xA9passe jamais 0 dB. Tu peux pousser fort sans ab\xC3\xAEmer ton son.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
            { Row::dot, juce::Colour(0xff8dffc8), "TU ENTENDS LA DIFF\xC3\x89RENCE", "BYPASS compare \xC3\xA0 volume \xC3\xA9gal, pour juger honn\xC3\xAAtement. DELTA te fait \xC3\xA9""couter uniquement ce que BLOOM ajoute.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), false },
            { Row::dot, juce::Colour(0xffffd27a), "POURQUOI IL DEVIENT INDISPENSABLE", "En quelques secondes, ta prod gagne du corps, de la clart\xC3\xA9 et de l\xE2\x80\x99\xC3\xA9""clat. Ce qui prenait vingt minutes d\xE2\x80\x99""EQ se r\xC3\xA8gle d\xE2\x80\x99un geste, et ton \xC3\xA9nergie reste pour la musique.", juce::Colour(), juce::Colour(), juce::Colour(), juce::Colour(), true },
        };
    }

    ~BloomSheet() override { stopTimer(); }

    // appele quand la licence change (pour cacher l'etiquette DEMO)
    std::function<void()> onLicenseChanged;

    // les morceaux de pierre du cadre du plugin (barre horizontale, barre verticale, coin)
    void setTiles(const juce::Image& h, const juce::Image& v, const juce::Image& corner)
    {
        tileH = h; tileV = v; tileC = corner;
        invalidateAll();
        repaint();
    }

    void open()
    {
        setVisible(true);
        toFront(true);
        openT0 = now();
        go(0);
        grabKeyboardFocus();
    }

    // la page "licence" (ouverte par l'etiquette DEMO)
    void openLicense()
    {
        setVisible(true);
        toFront(true);
        openT0 = now();
        licMsg = {};
        licBusy = false;
        page = 3;
        pageT0 = now();
        chromeDirty[3] = contentDirty[3] = true;
        layout();
        startTimerHz(60);
        grabKeyboardFocus();
        repaint();
    }

    void closeSheet()
    {
        stopTimer();
        keyBox.setVisible(false);
        setVisible(false);
    }

    void resized() override { invalidateAll(); layout(); }

    void paint(juce::Graphics& g) override
    {
        BLOOM_PERF_SCOPE("fiche : dessin");
        const float k = sc();
        const double t = now();
        const float aSheet = ease((float) ((t - openT0) / 0.30));
        const float aPanel = easeOut((float) ((t - openT0) / 0.40));
        const float ap = ease((float) ((t - pageT0) / 0.30));
        ensureCaches(k);

        // fond : voile sombre (image calculee une fois)
        g.setOpacity(aSheet);
        g.drawImageAt(backdropImg, 0, 0);

        // panneau (apparition : glisse de 10 px, 98 % -> 100 %)
        const auto pc = panel.getCentre();
        const auto pt = juce::AffineTransform::scale(0.98f + 0.02f * aPanel, 0.98f + 0.02f * aPanel, pc.x, pc.y)
                            .translated(0.0f, 10.0f * k * (1.0f - aPanel));
        const float alpha = aPanel * aSheet;
        g.setOpacity(alpha);
        g.drawImageTransformed(chromeImg[page], juce::AffineTransform::translation((float) chromeOrigin[page].x, (float) chromeOrigin[page].y).followedBy(pt));

        // contenu de la page (fondu + glisse de 8 px)
        const auto ct = juce::AffineTransform::translation(8.0f * k * (1.0f - ap), 0.0f).followedBy(pt);
        g.setOpacity(alpha * ap);
        g.drawImageTransformed(contentImg[page], juce::AffineTransform::translation((float) contentOrigin[page].x, (float) contentOrigin[page].y).followedBy(ct));
        g.setOpacity(1.0f);

        // parties animees, dessinees a chaque image (petites)
        const bool fading = alpha < 0.999f;
        g.saveState();
        if (fading) g.beginTransparencyLayer(alpha);
        g.addTransform(pt);
        drawEmbers(g, k, t);
        drawCloseX(g, k, t);
        if (page == 0)
        {
            g.saveState();
            g.addTransform(juce::AffineTransform::translation(8.0f * k * (1.0f - ap), 0.0f));
            if (ap < 0.999f) g.beginTransparencyLayer(ap);
            drawMeters(g, k, t);
            if (ap < 0.999f) g.endTransparencyLayer();
            g.restoreState();
        }
        if (fading) g.endTransparencyLayer();
        g.restoreState();
    }

    // images en cache : le voile, le panneau (cadre, en-tete, pied), le contenu de la page
    void ensureCaches(float k)
    {
        if (!backdropImg.isValid() || backdropImg.getWidth() != getWidth() || backdropImg.getHeight() != getHeight())
        {
            backdropImg = juce::Image(juce::Image::ARGB, juce::jmax(1, getWidth()), juce::jmax(1, getHeight()), true);
            juce::Graphics bg(backdropImg);
            const float W = (float) getWidth(), H = (float) getHeight();
            const float cx = W * 0.5f, cy = H * 0.4f;
            const float far = std::hypot(juce::jmax(cx, W - cx), juce::jmax(cy, H - cy));
            juce::ColourGradient gr(juce::Colour(0xcc1a1424), cx, cy, juce::Colour(0xf007060b), cx + far * 0.70f, cy, true);
            bg.setGradientFill(gr);
            bg.fillAll();
        }
        if (chromeDirty[page] || !chromeImg[page].isValid())
        {
            const double pt0 = juce::Time::getMillisecondCounterHiRes();
            struct PerfMark { double t; int p; ~PerfMark() { bloomperf::mark("fiche : decor page " + juce::String(p + 1) + " prepare en " + juce::String(juce::Time::getMillisecondCounterHiRes() - t, 1) + " ms"); } } pm { pt0, page };
            chromeDirty[page] = false;
            const auto area = panel.expanded(64.0f * k).getSmallestIntegerContainer();
            chromeOrigin[page] = area.getPosition();
            chromeImg[page] = juce::Image(juce::Image::ARGB, juce::jmax(1, area.getWidth()), juce::jmax(1, area.getHeight()), true);
            juce::Graphics cg(chromeImg[page]);
            cg.addTransform(juce::AffineTransform::translation((float) -chromeOrigin[page].x, (float) -chromeOrigin[page].y));
            drawShadow(cg, k);
            drawPanelBackground(cg, k);
            drawHeader(cg, k);
            drawFooter(cg, k);
            drawFrame(cg, k);
            drawCloseBg(cg, k);
        }
        if (contentDirty[page] || !contentImg[page].isValid())
        {
            const double pt0 = juce::Time::getMillisecondCounterHiRes();
            struct PerfMark { double t; int p; ~PerfMark() { bloomperf::mark("fiche : contenu page " + juce::String(p + 1) + " prepare en " + juce::String(juce::Time::getMillisecondCounterHiRes() - t, 1) + " ms"); } } pm { pt0, page };
            contentDirty[page] = false;
            const auto area = content.expanded(24.0f * k).getSmallestIntegerContainer();
            contentOrigin[page] = area.getPosition();
            contentImg[page] = juce::Image(juce::Image::ARGB, juce::jmax(1, area.getWidth()), juce::jmax(1, area.getHeight()), true);
            juce::Graphics cg(contentImg[page]);
            cg.addTransform(juce::AffineTransform::translation((float) -contentOrigin[page].x, (float) -contentOrigin[page].y));
            if (page == 0)      drawCards(cg, k);
            else if (page == 3) drawLicense(cg, k);
            else                drawRows(cg, k, pages[page - 1]);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { updateHover(e.position); }
    void mouseExit(const juce::MouseEvent&) override { updateHover({ -1000.0f, -1000.0f }); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        if (closeRect.contains(p)) { closeSheet(); return; }
        if (page == 3)
        {
            const bool lic = bloomlic::License::get().isLicensed();
            if (primaryRect.translated(content.getX(), content.getY()).contains(p)) { if (lic) closeSheet(); else activateNow(); return; }
            if (!lic && secondaryRect.translated(content.getX(), content.getY()).contains(p)) { closeSheet(); return; }
            if (!lic && buyRect.contains(p)) { recheckNow(); return; }
            if (!panel.contains(p)) closeSheet();
            return;
        }
        for (int i = 0; i < 3; ++i)
            if (tabRect[i].contains(p)) { go(i); return; }
        if (prevRect.contains(p) && page > 0) { go(page - 1); return; }
        if (nextRect.contains(p) && page < 2) { go(page + 1); return; }
        if (!panel.contains(p)) closeSheet();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey) { closeSheet(); return true; }
        if (page == 3)
        {
            if (key == juce::KeyPress::returnKey) { if (bloomlic::License::get().isLicensed()) closeSheet(); else activateNow(); }
            return true;
        }
        if (key == juce::KeyPress::rightKey && page < 2) { go(page + 1); return true; }
        if (key == juce::KeyPress::leftKey && page > 0) { go(page - 1); return true; }
        return true;
    }

private:
    struct Card { const char* id; juce::Colour col; const char* name; const char* badge; const char* text; };
    struct Row
    {
        enum Icon { regler, gem, menu, dot };
        Icon icon; juce::Colour col; const char* name; const char* text;
        juce::Colour gl, gm, gd, glow; bool key;
    };

    static juce::String T(const char* utf8) { return juce::String::fromUTF8(utf8); }
    static double now() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }
    static float ease(float x)    { x = juce::jlimit(0.0f, 1.0f, x); return x * x * (3.0f - 2.0f * x); }
    static float easeOut(float x) { x = juce::jlimit(0.0f, 1.0f, x); const float u = 1.0f - x; return 1.0f - u * u * u * u; }
    float sc() const { return (float) getWidth() / 600.0f; }

    void timerCallback() override
    {
        BLOOM_PERF_SCOPE("fiche : minuterie");
        if (!isShowing()) return;
        const double t = now();
        {
            const bool want = false; // plus de cle a saisir : BLOOM s'active depuis ONIRIQUE CENTRAL
            juce::ignoreUnused(t);
            if (want != keyBox.isVisible())
            {
                keyBox.setVisible(want);
                if (want) keyBox.grabKeyboardFocus();
            }
        }
        const bool anim = (t - openT0) < 0.45 || (t - pageT0) < 0.65 || (hoverBtn == 3 && (t - closeHoverT0) < 0.4) || (page == 3 && licBusy);
        if (anim)
        {
            repaint();
            return;
        }
        // au repos : seuls les losanges des coins bougent -> 4 petites zones, 30 images/s
        if (getTimerInterval() < 30)
            startTimerHz(30);
        const float s = 30.0f * sc();
        for (auto c : { panel.getTopLeft(), panel.getTopRight(), panel.getBottomLeft(), panel.getBottomRight() })
            repaint(juce::Rectangle<float>(s * 2.0f, s * 2.0f).withCentre(c).getSmallestIntegerContainer());
    }

    void go(int p)
    {
        keyBox.setVisible(false);
        page = juce::jlimit(0, 2, p);
        pageT0 = now();
        layout();
        startTimerHz(60);
        repaint();
    }

    // ---------------- mise en page (memes mesures que le prototype, pour 600 px de large) ----------------
    void layout()
    {
        const float k = sc();
        if (k <= 0.0f) return;
        const float W = (float) getWidth(), H = (float) getHeight();

        Tk = 13.2f * k;                       // epaisseur du cadre
        Ck = 38.4f * k;                       // taille des coins
        const float pw = 562.11f * k;
        const float padTop = Tk + 14.4f * k, padSide = Tk + 18.0f * k;
        const float cw = pw - 2.0f * padSide;  // largeur du contenu

        // hauteur de la page
        float pageH = 0.0f;
        const juce::Font body = bloomfonts::bodyFont(11.063f * k);
        const float lh = 15.71f * k;
        if (page == 3)
        {
            const bool lic = bloomlic::License::get().isLicensed();
            const float lh2 = 16.5f * k;
            licLines = wrap(licFont(k), licText(lic).toUpperCase(), cw);
            float y = (float) licLines.size() * lh2 + 14.0f * k;
            labelRect = { 0.0f, y, cw, 10.0f * k };
            y += 10.0f * k + 7.0f * k;
            keyRect = { 0.0f, y, cw, 36.0f * k };
            y += 36.0f * k + 14.0f * k;
            if (lic)
            {
                primaryRect = { cw * 0.25f, y, cw * 0.5f, 34.0f * k };
                secondaryRect = {};
            }
            else
            {
                const float gap = 10.0f * k;
                primaryRect = { 0.0f, y, (cw - gap) * 0.58f, 34.0f * k };
                secondaryRect = { primaryRect.getRight() + gap, y, cw - primaryRect.getWidth() - gap, 34.0f * k };
            }
            y += 34.0f * k + 10.0f * k;
            msgRect = { 0.0f, y, cw, 2.0f * lh2 };
            y += 2.0f * lh2;
            pageH = y;
        }
        else if (page == 0)
        {
            const float colW = (cw - 7.8f * k) * 0.5f;
            cardRects.clear(); cardLines.clear();
            float y = 0.0f;
            for (size_t i = 0; i < cards.size(); )
            {
                const bool wide = (i + 1 == cards.size());
                const int n = wide ? 1 : 2;
                float rowH = 0.0f;
                std::vector<juce::StringArray> lines;
                for (int j = 0; j < n; ++j)
                {
                    const float w = (wide ? cw : colW) - 21.6f * k;
                    lines.push_back(wrap(body, T(cards[i + (size_t) j].text), w));
                    rowH = juce::jmax(rowH, 9.6f * k + 14.0f * k + 7.2f * k + 3.0f * k + 6.0f * k + (float) lines.back().size() * lh + 9.6f * k);
                }
                for (int j = 0; j < n; ++j)
                {
                    cardRects.push_back({ (float) j * (colW + 7.8f * k), y, wide ? cw : colW, rowH });
                    cardLines.push_back(lines[(size_t) j]);
                }
                y += rowH + 7.8f * k;
                i += (size_t) n;
            }
            y -= 7.8f * k;
            safeY = y + 12.0f * k;
            pageH = safeY + 10.0f * k;
        }
        else
        {
            const auto& rows = pages[page - 1];
            rowRects.clear(); rowLines.clear();
            float y = 0.0f;
            const float tw = cw - 52.8f * k - 12.0f * k;
            for (auto& r : rows)
            {
                auto l = wrap(body, T(r.text), tw);
                const float h = 10.8f * k + 14.0f * k + 3.0f * k + (float) l.size() * lh + 10.8f * k;
                rowRects.push_back({ 0.0f, y, cw, h });
                rowLines.push_back(l);
                y += h + 7.8f * k;
            }
            pageH = y - 7.8f * k;
        }

        const float headerH = 38.27f * k + 7.2f * k + 10.0f * k + 7.2f * k + 6.0f * k + 31.0f * k;
        const float footH = 45.42f * k;
        const float ph = padTop + headerH + 13.2f * k + pageH + 12.0f * k + footH + Tk;
        panel = { (W - pw) * 0.5f, juce::jmax(18.0f * k, (H - ph) * 0.5f), pw, ph };

        content = { panel.getX() + padSide, panel.getY() + padTop + headerH + 13.2f * k, cw, pageH };
        logoBox = { panel.getCentreX() - 147.59f * k * 0.5f, panel.getY() + padTop, 147.59f * k, 38.27f * k };
        tagBox = { panel.getX() + padSide, logoBox.getBottom() + 7.2f * k, cw, 10.0f * k };

        // onglets : largeur = texte + 2 x 12 px
        const juce::Font tf = bloomfonts::pixelFont(8.97f * k, false, 1.2558f * k);
        float tabsW = 6.0f * k + 8.0f * k;
        float tws[3];
        for (int i = 0; i < 3; ++i) { tws[i] = juce::GlyphArrangement::getStringWidth(tf, tabLabel(i)) + 24.0f * k; tabsW += tws[i]; }
        tabsBox = { panel.getCentreX() - tabsW * 0.5f, tagBox.getBottom() + 7.2f * k + 6.0f * k, tabsW, 31.0f * k };
        float tx = tabsBox.getX() + 3.0f * k;
        for (int i = 0; i < 3; ++i) { tabRect[i] = { tx, tabsBox.getY() + 3.0f * k, tws[i], 25.0f * k }; tx += tws[i] + 4.0f * k; }

        footRect = { panel.getX() + Tk, content.getBottom() + 12.0f * k, panel.getWidth() - 2.0f * Tk, footH };
        prevRect = { footRect.getX() + 18.0f * k, footRect.getY() + 9.6f * k, 38.4f * k, 26.4f * k };
        nextRect = { footRect.getRight() - 18.0f * k - 38.4f * k, footRect.getY() + 9.6f * k, 38.4f * k, 26.4f * k };
        closeRect = { panel.getRight() - Tk - 8.0f * k - 30.0f * k, panel.getY() + Tk + 8.0f * k, 30.0f * k, 30.0f * k };

        // page licence : champ de saisie et lien d'achat
        if (page == 3)
        {
            keyBox.setFont(bloomfonts::bodyFont(14.0f * k));
            keyBox.setBounds(keyRect.translated(content.getX(), content.getY()).reduced(6.0f * k, 2.0f * k).getSmallestIntegerContainer());
            const juce::Font lf = bloomfonts::pixelFont(8.6f * k, false, 1.2f * k);
            const float w = juce::GlyphArrangement::getStringWidth(lf, buyLabel()) + 16.0f * k;
            buyRect = { footRect.getCentreX() - w * 0.5f, footRect.getCentreY() - 11.0f * k, w, 22.0f * k };
        }
    }

    static juce::String licText(bool lic)
    {
        auto& licence = bloomlic::License::get();
        if (licence.isTrial())
            return T("BLOOM est en essai gratuit : encore ") + juce::String(juce::jmax(1, licence.trialDaysLeft()))
                 + T(licence.trialDaysLeft() > 1 ? " jours" : " jour") + T(". Tout y est, mais un export reste remplac\xC3\xA9 par un bruit blanc tant que BLOOM n'est pas achet\xC3\xA9. "
                     "Ach\xC3\xA8te-le depuis ONIRIQUE CENTRAL pour le garder.");
        if (! lic && licence.status().renewal)
            return T("BLOOM est bien \xC3\xA0 toi, mais sa licence n'a pas \xC3\xA9t\xC3\xA9 renouvel\xC3\xA9""e depuis plus de 90 jours. "
                     "Ouvre ONIRIQUE CENTRAL avec une connexion Internet : il la renouvelle en quelques secondes et BLOOM repart aussit\xC3\xB4t.");
        return lic ? T("BLOOM est activ\xC3\xA9 sur cet ordinateur. Le mode d\xC3\xA9mo est termin\xC3\xA9 : Make your mix BLOOM.")
                   : T("Tu utilises BLOOM en mode d\xC3\xA9mo : l'\xC3\xA9""coute dans ton DAW est intacte, mais un bruit blanc remplace tout export. "
                       "Ouvre ONIRIQUE CENTRAL, connecte-toi \xC3\xA0 ton compte, puis active BLOOM ou lance son essai gratuit de 7 jours : "
                       "c'est pris en compte ici en quelques secondes.");
    }

    // la police du texte de la page licence : celle du plugin, pas celle du systeme
    static juce::Font licFont(float k) { return bloomfonts::pixelFont(8.2f * k, false, 1.0f * k); }

    static juce::String buyLabel() { return T("D\xC3\x89J\xC3\x80 ACTIV\xC3\x89 DANS CENTRAL ?  REV\xC3\x89RIFIER"); }

    // le bouton principal de la page : ouvre ONIRIQUE CENTRAL, d'ou BLOOM s'active
    void activateNow()
    {
        if (bloomlic::License::get().isLicensed())
            return;
        bloomlic::License::get().openCentral();
        licBusy = false;
        licMsgOk = true;
        licMsg = T("ONIRIQUE CENTRAL s'ouvre : active BLOOM, puis reviens ici.");
        contentDirty[3] = true;
        repaint();
    }

    // relit la licence tout de suite, sans attendre la prochaine verification
    void recheckNow()
    {
        auto& licence = bloomlic::License::get();
        licence.refresh();
        licBusy = false;
        licMsgOk = licence.isLicensed();
        licMsg = licMsgOk ? T("BLOOM est activ\xC3\xA9 sur cet ordinateur.") : T("Pas encore de licence sur cet ordinateur.");
        licenseChanged();
        if (onLicenseChanged) onLicenseChanged();
    }

public:
    // l'etat de la licence a change (depuis ONIRIQUE CENTRAL, pendant que la fenetre est ouverte)
    void licenseChanged()
    {
        chromeDirty[3] = contentDirty[3] = true;
        if (isVisible() && page == 3)
        {
            layout();
            repaint();
        }
    }
private:

    juce::String tabLabel(int i) const
    {
        static const char* labels[3] = { "R\xC3\xA9glages", "Contr\xC3\xB4les", "Pourquoi" };
        return T(labels[i]).toUpperCase();
    }

    static juce::StringArray wrap(const juce::Font& f, const juce::String& text, float width)
    {
        juce::StringArray out;
        juce::StringArray words;
        words.addTokens(text, " ", "");
        juce::String line;
        for (auto& w : words)
        {
            const juce::String test = line.isEmpty() ? w : line + " " + w;
            if (line.isNotEmpty() && juce::GlyphArrangement::getStringWidth(f, test) > width)
            {
                out.add(line);
                line = w;
            }
            else
                line = test;
        }
        if (line.isNotEmpty()) out.add(line);
        return out;
    }

    // texte sur une ligne, centre verticalement dans sa boite (comme le CSS)
    static void text(juce::Graphics& g, const juce::Font& font, const juce::String& s, juce::Rectangle<float> box,
                     juce::Justification j, juce::Colour c)
    {
        // Un texte ne sort jamais de sa case : trop large pour elle (un long e-mail, un libelle dans un petit bouton),
        // il est resserre jusqu'a y tenir, avec une marge de chaque cote.
        juce::Font f(font);
        if (box.getWidth() > 24.0f)
        {
            const float room = box.getWidth() - 2.0f * juce::jmin(10.0f, box.getWidth() * 0.05f);
            const float natural = juce::GlyphArrangement::getStringWidth(f, s);
            if (natural > room)
                f.setHorizontalScale(juce::jmax(0.5f, f.getHorizontalScale() * room / natural));
        }
        g.setFont(f);
        g.setColour(c);
        const float w = juce::GlyphArrangement::getStringWidth(f, s);
        float x = box.getX();
        if (j.testFlags(juce::Justification::horizontallyCentred)) x = box.getCentreX() - w * 0.5f;
        else if (j.testFlags(juce::Justification::right))          x = box.getRight() - w;
        const float asc = f.getAscent(), desc = f.getDescent();
        const float base = box.getY() + (box.getHeight() - (asc + desc)) * 0.5f + asc;
        g.drawSingleLineText(s, juce::roundToInt(x), juce::roundToInt(base));
    }

    static void para(juce::Graphics& g, const juce::Font& f, const juce::StringArray& lines, float x, float y, float lh, juce::Colour c)
    {
        g.setFont(f);
        g.setColour(c);
        const float asc = f.getAscent(), desc = f.getDescent();
        for (int i = 0; i < lines.size(); ++i)
        {
            const float base = y + (float) i * lh + (lh - (asc + desc)) * 0.5f + asc;
            g.drawSingleLineText(lines[i], juce::roundToInt(x), juce::roundToInt(base));
        }
    }

    // halo doux autour d'un rectangle (imite box-shadow / drop-shadow du prototype)
    static void rectGlow(juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour c)
    {
        const int steps = 6;
        for (int i = steps; i >= 1; --i)
        {
            const float f = (float) i / (float) steps;
            g.setColour(c.withMultipliedAlpha((1.0f - f) * (1.0f - f) * 0.55f));
            g.fillRoundedRectangle(r.expanded(radius * f), radius * f);
        }
    }

    static void diamond(juce::Graphics& g, float cx, float cy, float half, juce::Colour c)
    {
        juce::Path d;
        d.addQuadrilateral(cx, cy - half, cx + half, cy, cx, cy + half, cx - half, cy);
        g.setColour(c);
        g.fillPath(d);
    }

    // pierre (texture du coin, agrandie x2, pixels nets) dans une forme
    void stoneFill(juce::Graphics& g, const juce::Path& clip, juce::Rectangle<float> area, float k) const
    {
        g.saveState();
        g.reduceClipRegion(clip);
        g.setColour(juce::Colour(0xff2b2732));
        g.fillRect(area);
        if (tileC.isValid())
        {
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            const float ts = 48.0f * k;
            for (float y = area.getY(); y < area.getBottom(); y += ts)
                for (float x = area.getX(); x < area.getRight(); x += ts)
                    g.drawImageTransformed(tileC, juce::AffineTransform::scale(ts / (float) tileC.getWidth()).translated(x, y));
        }
        g.restoreState();
    }

    // ---------------- dessin ----------------
    void drawShadow(juce::Graphics& g, float k)
    {
        const float d = 8.0f;
        const int pad = 8;
        juce::Image small(juce::Image::ARGB, (int) (panel.getWidth() / d) + pad * 2, (int) (panel.getHeight() / d) + pad * 2, true);
        {
            juce::Graphics sg(small);
            sg.setColour(juce::Colours::black);
            sg.fillRect(juce::Rectangle<float>((float) pad, (float) pad + 18.0f * k / d, panel.getWidth() / d, panel.getHeight() / d));
        }
        juce::ImageConvolutionKernel kk(9);
        kk.createGaussianBlur(2.2f);
        kk.applyToImage(small, small, small.getBounds());
        g.saveState();
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.setOpacity(0.9f);
        g.drawImageTransformed(small, juce::AffineTransform::scale(d).translated(panel.getX() - (float) pad * d, panel.getY() - (float) pad * d));
        g.restoreState();
        rectGlow(g, panel, 30.0f * k, juce::Colour(0x0cff8a3a));
    }

    void drawPanelBackground(juce::Graphics& g, float k)
    {
        juce::Path r;
        r.addRectangle(panel);
        stoneFill(g, r, panel, k);
        juce::ColourGradient lin(juce::Colour(0xf01b1722), 0.0f, panel.getY(), juce::Colour(0xf70e0c13), 0.0f, panel.getBottom(), false);
        g.setGradientFill(lin);
        g.fillRect(panel);
        // lueur chaude en haut
        {
            g.saveState();
            g.reduceClipRegion(panel.toNearestInt());
            const float cx = panel.getCentreX(), cy = panel.getY();
            const float rx = std::hypot(panel.getWidth() * 0.5f, panel.getHeight()) * 0.55f;
            juce::ColourGradient rad(juce::Colour(0x14ff8a3a), cx, cy, juce::Colour(0x00ff8a3a), cx + rx, cy, true);
            g.setGradientFill(rad);
            g.addTransform(juce::AffineTransform::scale(1.0f, panel.getHeight() / panel.getWidth() * 1.4f, cx, cy));
            g.fillEllipse(cx - rx, cy - rx, rx * 2.0f, rx * 2.0f);
            g.restoreState();
        }
        // fine ligne doree sous le cadre
        juce::ColourGradient line(juce::Colour(0x00ffb347), panel.getX() + Tk, 0.0f, juce::Colour(0x00ffb347), panel.getRight() - Tk, 0.0f, false);
        line.addColour(0.5, juce::Colour(0x66ffb347));
        g.setGradientFill(line);
        g.fillRect(panel.getX() + Tk, panel.getY() + Tk, panel.getWidth() - 2.0f * Tk, 2.0f * k);
    }

    void drawLogoSmall(juce::Graphics& g, float k)
    {
        const float u = logoBox.getHeight() / 21.0f;
        const float ox = logoBox.getX() + 2.0f * u, oy = logoBox.getY() + 2.0f * u;
        const juce::Path fillP    = makeLogoPath(0.0f,  ox, oy, u, true);
        const juce::Path outlineP = makeLogoPath(0.4f,  ox, oy, u, true);
        const juce::Path shadowP  = makeLogoPath(0.45f, ox + 0.55f * u, oy + 0.75f * u, u, true);

        // halo (drop-shadow 8 px #ff8a3a40), calcule une fois
        if (!logoGlow.isValid() || logoGlowFor != logoBox)
        {
            logoGlowFor = logoBox;
            const int pad = 16;
            logoGlow = juce::Image(juce::Image::ARGB, (int) logoBox.getWidth() + pad * 2, (int) logoBox.getHeight() + pad * 2, true);
            {
                juce::Graphics gg(logoGlow);
                gg.setColour(juce::Colour(0xffff8a3a));
                gg.fillPath(outlineP, juce::AffineTransform::translation(pad - std::floor(logoBox.getX()), pad - std::floor(logoBox.getY())));
            }
            juce::ImageConvolutionKernel kk(17);
            kk.createGaussianBlur(5.0f);
            kk.applyToImage(logoGlow, logoGlow, logoGlow.getBounds());
        }
        g.setOpacity(0.25f);
        g.drawImageAt(logoGlow, (int) std::floor(logoBox.getX()) - 16, (int) std::floor(logoBox.getY()) - 16);
        g.setOpacity(1.0f);

        g.setColour(juce::Colour(0xb305040a));
        g.fillPath(shadowP);
        g.setColour(juce::Colour(0xff0d0a14));
        g.fillPath(outlineP);
        juce::ColourGradient grad(juce::Colour(0xffe9dff0), 0.0f, logoBox.getY(), juce::Colour(0xffb3a6c3), 0.0f, logoBox.getBottom(), false);
        grad.addColour(0.6, juce::Colour(0xffcfc3da));
        g.setGradientFill(grad);
        g.fillPath(fillP);
        for (float cxv : { 33.5f, 50.5f })
        {
            const float cx = ox + cxv * u, cy = oy + 6.5f * u;
            juce::Path d;
            d.addQuadrilateral(cx, cy - 2.0f * u, cx + 1.4f * u, cy, cx, cy + 2.0f * u, cx - 1.4f * u, cy);
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(d);
            g.setColour(juce::Colour(0xfffff4dc));
            g.strokePath(d, juce::PathStrokeType(0.2f * u));
        }
        juce::ignoreUnused(k);
    }

    void drawHeader(juce::Graphics& g, float k)
    {
        drawLogoSmall(g, k);
        text(g, bloomfonts::pixelFont(8.073f * k, false, 1.2917f * k),
             page == 3 ? T("Activation de ta licence").toUpperCase() : T("Un traitement tonal rapide et musical, sans EQ \xC3\xA0 r\xC3\xA9gler").toUpperCase(), tagBox,
             juce::Justification::horizontallyCentred, juce::Colour(0xff857d90));

        if (page == 3)
        {
            const bool lic = bloomlic::License::get().isLicensed();
            const juce::Font pf = bloomfonts::pixelFont(8.97f * k, false, 1.2558f * k);
            const juce::String label = bloomlic::License::get().isTrial() ? T("ESSAI GRATUIT") : lic ? T("LICENCE ACTIVE")
                                     : bloomlic::License::get().status().renewal ? T("\xC3\x80 RENOUVELER") : T("MODE D\xC3\x89MO");
            const float w = juce::GlyphArrangement::getStringWidth(pf, label) + 40.0f * k;
            const juce::Rectangle<float> pill(panel.getCentreX() - w * 0.5f, tabsBox.getY() + 3.0f * k, w, 25.0f * k);
            const juce::Colour amber(0xffffb347), pale(0xfffff4dc);
            rectGlow(g, pill, 12.0f * k, juce::Colour(lic ? 0x66ff8a3a : 0x33ff8a3a));
            g.setColour(lic ? amber : juce::Colour(0xff6b5a3a));
            g.fillPath(chamferPath(pill, 6.0f * k));
            const auto in = pill.reduced(1.0f);
            const auto inner = chamferPath(in, 5.0f * k);
            g.setColour(juce::Colour(0xff17131d));
            g.fillPath(inner);
            g.saveState();
            g.reduceClipRegion(inner);
            juce::ColourGradient shade(juce::Colour(0x26ffffff), 0.0f, in.getY(), juce::Colour(0x8a000000), 0.0f, in.getBottom(), false);
            shade.addColour(0.40, juce::Colour(0x00000000));
            g.setGradientFill(shade);
            g.fillRect(in);
            g.restoreState();
            diamond(g, pill.getX() + 12.0f * k, pill.getCentreY(), 3.2f * k, amber);
            diamond(g, pill.getRight() - 12.0f * k, pill.getCentreY(), 3.2f * k, amber);
            text(g, pf, label, pill, juce::Justification::horizontallyCentred, lic ? pale : amber);
            return;
        }

        // onglets
        g.setColour(juce::Colour(0xff0b0910));
        g.fillRect(tabsBox);
        g.setColour(juce::Colour(0xff2e2937));
        g.drawRect(tabsBox, 1.0f);
        const juce::Font tf = bloomfonts::pixelFont(8.97f * k, false, 1.2558f * k);
        for (int i = 0; i < 3; ++i)
        {
            const auto r = tabRect[i];
            if (i == page)
            {
                rectGlow(g, r, 12.0f * k, juce::Colour(0x66ff8a3a));
                juce::ColourGradient gr(juce::Colour(0xffffd27a), 0.0f, r.getY(), juce::Colour(0xffffb347), 0.0f, r.getBottom(), false);
                g.setGradientFill(gr);
                g.fillRect(r);
            }
            text(g, tf, tabLabel(i), r, juce::Justification::horizontallyCentred,
                 i == page ? juce::Colour(0xff1a120a) : (hoverTab == i ? juce::Colour(0xffddd3e4) : juce::Colour(0xff857d90)));
        }
    }

    float paramValue(const char* id) const
    {
        if (auto* p = apvts.getParameter(id))
            return p->getValue();
        return 0.0f;
    }

    void drawCards(juce::Graphics& g, float k)
    {
        const juce::Font nameF  = bloomfonts::pixelFont(10.465f * k, true, 1.6744f * k);
        const juce::Font badgeF = bloomfonts::pixelFont(7.774f * k, false, 0.7774f * k);
        const juce::Font body   = bloomfonts::bodyFont(11.063f * k);
        for (size_t i = 0; i < cards.size() && i < cardRects.size(); ++i)
        {
            const auto& c = cards[i];
            const auto r = cardRects[i].translated(content.getX(), content.getY());
            const bool hov = hoverCard == (int) i;

            if (hov) rectGlow(g, r, 14.0f * k, c.col.withAlpha(0.22f));
            g.setColour(hov ? juce::Colour(0xd915111b) : juce::Colour(0xd90d0b12));
            g.fillRect(r);
            g.setColour(hov ? c.col.interpolatedWith(juce::Colour(0xff2a2532), 0.55f) : juce::Colour(0xff2a2532));
            g.drawRect(r, 1.0f);
            g.setColour(c.col);
            g.fillRect(r.getX(), r.getY(), 2.0f * k, r.getHeight());

            // en-tete : gemme, nom, badge
            const float hy = r.getY() + 9.6f * k;
            softGlow(g, r.getX() + 10.8f * k + 3.5f * k, hy + 7.0f * k, 8.0f * k, c.col.withAlpha(0.45f));
            diamond(g, r.getX() + 10.8f * k + 3.5f * k, hy + 7.0f * k, 4.95f * k, c.col);
            text(g, nameF, T(c.name), { r.getX() + 25.8f * k, hy, 200.0f * k, 14.0f * k }, juce::Justification::left, c.col);

            const juce::String badge = T(c.badge).toUpperCase();
            const float bw = juce::GlyphArrangement::getStringWidth(badgeF, badge) + 12.0f * k;
            const juce::Rectangle<float> br(r.getRight() - 10.8f * k - bw, hy, bw, 14.0f * k);
            g.setColour(c.col.interpolatedWith(juce::Colour(0xff0b0910), 0.78f));
            g.fillRect(br);
            g.setColour(c.col.withAlpha(0.4f));
            g.drawRect(br, 1.0f);
            text(g, badgeF, badge, br, juce::Justification::horizontallyCentred, juce::Colour(0xfffff4e6));

            // jauge : valeur actuelle du reglage
            const juce::Rectangle<float> mr(r.getX() + 10.8f * k, hy + 14.0f * k + 7.2f * k, r.getWidth() - 21.6f * k, 3.0f * k);
            g.setColour(juce::Colour(0xff1f1b26));
            g.fillRect(mr);

            para(g, body, cardLines[i], r.getX() + 10.8f * k, mr.getBottom() + 6.0f * k, 15.71f * k, juce::Colour(0xffc9c0d3));
        }

        // "La sortie ne depasse jamais 0 dB"
        const juce::Font sf = bloomfonts::pixelFont(8.073f * k, false, 1.2917f * k);
        const juce::String s = T("La sortie ne d\xC3\xA9passe jamais 0 dB").toUpperCase();
        const float sw = juce::GlyphArrangement::getStringWidth(sf, s);
        const juce::Rectangle<float> sb(content.getX(), content.getY() + safeY, content.getWidth(), 10.0f * k);
        text(g, sf, s, sb, juce::Justification::horizontallyCentred, juce::Colour(0xffffb347));
        for (float side : { -1.0f, 1.0f })
        {
            const float cx = sb.getCentreX() + side * (sw * 0.5f + 10.0f * k + 3.5f * k);
            softGlow(g, cx, sb.getCentreY(), 7.0f * k, juce::Colour(0x88ff8a3a));
            diamond(g, cx, sb.getCentreY(), 3.54f * k, juce::Colour(0xffffb347));
        }
    }

    // jauges : valeur actuelle de chaque reglage, qui se remplit a l'ouverture
    void drawMeters(juce::Graphics& g, float k, double t)
    {
        const float meterA = easeOut((float) ((t - pageT0) / 0.6));
        for (size_t i = 0; i < cards.size() && i < cardRects.size(); ++i)
        {
            const auto& c = cards[i];
            const auto r = cardRects[i].translated(content.getX(), content.getY());
            const juce::Rectangle<float> mr(r.getX() + 10.8f * k, r.getY() + 9.6f * k + 14.0f * k + 7.2f * k, r.getWidth() - 21.6f * k, 3.0f * k);
            const float v = juce::jlimit(0.0f, 1.0f, paramValue(c.id)) * meterA;
            if (v > 0.001f)
            {
                const auto fr = mr.withWidth(mr.getWidth() * v);
                g.setColour(c.col.withAlpha(0.25f));
                g.fillRect(fr.expanded(0.0f, 1.5f * k));
                g.setColour(c.col);
                g.fillRect(fr);
            }
        }
    }

    void drawIcon(juce::Graphics& g, const Row& r, float cx, float cy, float k)
    {
        if (r.icon == Row::regler)
        {
            const float s = 21.6f * k / 20.0f, x0 = cx - 10.0f * s, y0 = cy - 10.0f * s;
            juce::Path arc;
            arc.addCentredArc(x0 + 10.0f * s, y0 + 15.0f * s + 0.0f, 7.0f * s * 1.142857f, 7.0f * s * 1.142857f, 0.0f,
                              -juce::MathConstants<float>::halfPi * 1.0f, juce::MathConstants<float>::halfPi * 1.0f, true);
            juce::Path a2;
            a2.startNewSubPath(x0 + 3.0f * s, y0 + 15.0f * s);
            a2.cubicTo(x0 + 3.0f * s, y0 + 10.6f * s, x0 + 6.6f * s, y0 + 7.0f * s, x0 + 10.0f * s, y0 + 7.0f * s);
            a2.cubicTo(x0 + 13.4f * s, y0 + 7.0f * s, x0 + 17.0f * s, y0 + 10.6f * s, x0 + 17.0f * s, y0 + 15.0f * s);
            g.setColour(juce::Colour(0xffffd27a));
            g.strokePath(a2, juce::PathStrokeType(2.4f * s));
            juce::Path d;
            const float hx = x0 + 15.2f * s, hy = y0 + 8.6f * s, h = 2.83f * s;
            d.addQuadrilateral(hx, hy - h, hx + h, hy, hx, hy + h, hx - h, hy);
            g.setColour(juce::Colour(0xff1a171f));
            g.fillPath(d);
            g.setColour(juce::Colour(0xffffd27a));
            g.strokePath(d, juce::PathStrokeType(1.2f * s));
            juce::ignoreUnused(arc);
        }
        else if (r.icon == Row::gem)
        {
            const float half = 14.4f * k * 0.70710678f;
            juce::Path d;
            d.addQuadrilateral(cx, cy - half, cx + half, cy, cx, cy + half, cx - half, cy);
            softGlow(g, cx, cy, half + 10.0f * k, r.glow.withAlpha(0.55f));
            g.setColour(juce::Colour(0xff0b0910));
            g.strokePath(d, juce::PathStrokeType(2.0f * k));
            // degrade 135 deg (haut-gauche clair -> bas-droite sombre)
            juce::ColourGradient gr(r.gl, cx - half, cy - half * 0.0f - 0.0f, r.gd, cx + half, cy, false);
            gr = juce::ColourGradient(r.gl, cx, cy - half, r.gd, cx, cy + half, false);
            gr.addColour(0.45, r.gm);
            g.setGradientFill(gr);
            g.fillPath(d);
        }
        else if (r.icon == Row::menu)
        {
            const float s = 21.6f * k / 20.0f, x0 = cx - 10.0f * s, y0 = cy - 10.0f * s;
            g.setColour(juce::Colour(0xff857d90));
            for (float yy : { 4.0f, 8.8f, 13.6f })
                g.fillRect(x0 + 3.0f * s, y0 + yy * s, 14.0f * s, 2.4f * s);
        }
        else
        {
            softGlow(g, cx, cy, 11.0f * k, r.col.withAlpha(0.5f));
            diamond(g, cx, cy, 5.66f * k, r.col);
        }
    }

    void drawRows(juce::Graphics& g, float k, const std::vector<Row>& rows)
    {
        const juce::Font nameF = bloomfonts::pixelFont(10.465f * k, true, 1.6744f * k);
        const juce::Font body  = bloomfonts::bodyFont(11.063f * k);
        for (size_t i = 0; i < rows.size() && i < rowRects.size(); ++i)
        {
            const auto& r = rows[i];
            const auto b = rowRects[i].translated(content.getX(), content.getY());
            if (r.key)
            {
                rectGlow(g, b, 18.0f * k, juce::Colour(0x26ff8a3a));
                g.setColour(juce::Colour(0xd90d0b12));
                g.fillRect(b);
                juce::ColourGradient gr(juce::Colour(0x1fffb347), b.getX(), 0.0f, juce::Colour(0x00ffb347), b.getX() + b.getWidth() * 0.7f, 0.0f, false);
                g.setGradientFill(gr);
                g.fillRect(b);
                g.setColour(juce::Colour(0x70ffb347));
                g.drawRect(b, 1.0f);
            }
            else
            {
                g.setColour(juce::Colour(0xd90d0b12));
                g.fillRect(b);
                g.setColour(juce::Colour(0xff2a2532));
                g.drawRect(b, 1.0f);
            }
            drawIcon(g, r, b.getX() + 12.0f * k + 15.0f * k, b.getCentreY(), k);
            const float tx = b.getX() + 52.8f * k;
            text(g, nameF, T(r.name), { tx, b.getY() + 10.8f * k, b.getWidth() - 60.0f * k, 14.0f * k }, juce::Justification::left, r.col);
            para(g, body, rowLines[i], tx, b.getY() + 10.8f * k + 17.0f * k, 15.71f * k, juce::Colour(0xffc9c0d3));
        }
    }

    void drawLicense(juce::Graphics& g, float k)
    {
        const bool lic = bloomlic::License::get().isLicensed();
        const auto o = content.getPosition();
        const juce::Font body = bloomfonts::bodyFont(11.063f * k);
        const float lh2 = 16.5f * k;
        {
            const juce::Font lf = licFont(k);
            g.setFont(lf);
            g.setColour(juce::Colour(0xffc9bfd6));
            for (int i = 0; i < licLines.size(); ++i)
            {
                const float w = juce::GlyphArrangement::getStringWidth(lf, licLines[i]);
                const float base = o.y + (float) i * lh2 + (lh2 - (lf.getAscent() + lf.getDescent())) * 0.5f + lf.getAscent();
                g.drawSingleLineText(licLines[i], juce::roundToInt(o.x + (content.getWidth() - w) * 0.5f), juce::roundToInt(base));
            }
        }

        // champ de la cle
        text(g, bloomfonts::pixelFont(7.774f * k, false, 1.2f * k), lic ? T("TON COMPTE") : T("ACTIVATION"),
             labelRect.translated(o.x, o.y), juce::Justification::left, juce::Colour(0xff857d90));
        const auto kr = keyRect.translated(o.x, o.y);
        g.setColour(juce::Colour(0xff07060b));
        g.fillPath(chamferPath(kr, 5.0f * k));
        g.setColour(lic ? juce::Colour(0xffffb347) : juce::Colour(0xff6b5a3a));
        g.strokePath(chamferPath(kr.reduced(0.5f), 5.0f * k), juce::PathStrokeType(1.0f));
        juce::ColourGradient inner(juce::Colour(0x66000000), 0.0f, kr.getY(), juce::Colour(0x00000000), 0.0f, kr.getY() + 8.0f * k, false);
        g.setGradientFill(inner);
        g.fillRect(kr.reduced(2.0f));
        text(g, bloomfonts::bodyFont(lic ? 14.0f * k : 12.0f * k), lic ? bloomlic::License::get().getMaskedKey() : T("Depuis l'application ONIRIQUE CENTRAL"),
             kr, juce::Justification::horizontallyCentred, lic ? juce::Colour(0xffddd3e4) : juce::Colour(0xff857d90));

        // boutons
        auto button = [&](juce::Rectangle<float> r, const juce::String& label, bool primary, bool hov, bool disabled)
        {
            const auto rr = r.translated(o.x, o.y);
            if (hov && !disabled) rectGlow(g, rr, 8.0f * k, primary ? juce::Colour(0x99ff8a3a) : juce::Colour(0x55b9a6ff));
            g.saveState();
            if (disabled) g.beginTransparencyLayer(0.55f);
            const juce::Colour edge = primary ? (hov ? juce::Colour(0xfffff4dc) : juce::Colour(0xffffb347))
                                              : (hov ? juce::Colour(0xffffd27a) : juce::Colour(0xff5a5466));
            drawStoneButton(g, rr, 6.0f * k, edge, k, juce::Colour(0x22ffffff), juce::Colour(0x7a000000), 0.40f);
            if (primary)
            {
                juce::ColourGradient gg(juce::Colour(0x55ffd27a), 0.0f, rr.getY(), juce::Colour(0x22ff8a3a), 0.0f, rr.getBottom(), false);
                g.setGradientFill(gg);
                g.fillPath(chamferPath(rr.reduced(1.0f), 5.0f * k));
            }
            const juce::Font bf = bloomfonts::pixelFont(9.4f * k, true, 1.5f * k);
            for (auto off : { juce::Point<float>(0.0f, 1.0f), juce::Point<float>(1.0f, 1.0f), juce::Point<float>(-1.0f, 1.0f), juce::Point<float>(0.0f, -1.0f) })
                text(g, bf, label, rr.translated(off.x * k, off.y * k), juce::Justification::horizontallyCentred, juce::Colour(0xe607060b));
            text(g, bf, label, rr, juce::Justification::horizontallyCentred,
                 primary ? juce::Colour(0xfffff4dc) : (hov ? juce::Colour(0xfff1ecf7) : juce::Colour(0xffb3a6c3)));
            if (disabled) g.endTransparencyLayer();
            g.restoreState();
        };
        if (lic)
            button(primaryRect, T("FERMER"), true, hoverBtn == 4, false);
        else
        {
            button(primaryRect, licBusy ? T("V\xC3\x89RIFICATION\xE2\x80\xA6") : T("OUVRIR ONIRIQUE CENTRAL"), true, hoverBtn == 4, licBusy);
            button(secondaryRect, T("CONTINUER EN D\xC3\x89MO"), false, hoverBtn == 5, false);
        }

        // message (resultat de l'activation)
        if (licMsg.isNotEmpty())
        {
            const auto mr = msgRect.translated(o.x, o.y);
            const auto lines = wrap(body, licMsg, mr.getWidth());
            const float startY = mr.getY() + (mr.getHeight() - (float) juce::jmin(2, lines.size()) * lh2) * 0.5f;
            g.setFont(body);
            g.setColour(licBusy ? juce::Colour(0xffb3a6c3) : (licMsgOk ? juce::Colour(0xffffd27a) : juce::Colour(0xffff7d97)));
            for (int i = 0; i < juce::jmin(2, lines.size()); ++i)
            {
                const float w = juce::GlyphArrangement::getStringWidth(body, lines[i]);
                const float base = startY + (float) i * lh2 + (lh2 - (body.getAscent() + body.getDescent())) * 0.5f + body.getAscent();
                g.drawSingleLineText(lines[i], juce::roundToInt(mr.getCentreX() - w * 0.5f), juce::roundToInt(base));
            }
        }
    }

    void drawStoneButton(juce::Graphics& g, juce::Rectangle<float> r, float chamfer, juce::Colour edge, float k,
                         juce::Colour hiTop, juce::Colour loBottom, float midStop)
    {
        g.setColour(edge);
        g.fillPath(chamferPath(r, chamfer));
        const auto in = r.reduced(1.0f);
        const auto ip = chamferPath(in, chamfer - 1.0f);
        stoneFill(g, ip, in, k);
        g.saveState();
        g.reduceClipRegion(ip);
        juce::ColourGradient gr(hiTop, 0.0f, in.getY(), loBottom, 0.0f, in.getBottom(), false);
        gr.addColour(midStop, juce::Colour(0x00000000));
        g.setGradientFill(gr);
        g.fillRect(in);
        g.restoreState();
    }

    void drawFooter(juce::Graphics& g, float k)
    {
        juce::ColourGradient gr(juce::Colour(0xf20d0b12), 0.0f, footRect.getY(), juce::Colour(0xff0b0910), 0.0f, footRect.getBottom(), false);
        g.setGradientFill(gr);
        g.fillRect(footRect);
        g.setColour(juce::Colour(0xff2e2937));
        g.fillRect(footRect.getX(), footRect.getY(), footRect.getWidth(), 1.0f);

        if (page == 3)
        {
            const bool lic = bloomlic::License::get().isLicensed();
            const juce::Font lf = bloomfonts::pixelFont(8.6f * k, false, 1.2f * k);
            if (lic)
            {
                text(g, lf, T("MERCI D'AVOIR CHOISI BLOOM"), footRect, juce::Justification::horizontallyCentred, juce::Colour(0xff857d90));
            }
            else
            {
                const bool hov = hoverBtn == 6;
                const juce::Colour c = hov ? juce::Colour(0xfffff4dc) : juce::Colour(0xffffb347);
                text(g, lf, buyLabel(), buyRect.withTrimmedRight(16.0f * k), juce::Justification::horizontallyCentred, c);
                // petite fleche en pixels
                const float px = 1.6f * k, ax = buyRect.getRight() - 12.0f * k, ay = buyRect.getCentreY();
                g.setColour(c);
                g.fillRect(ax, ay - px * 0.5f, 7.0f * px, px);
                for (int i = 1; i <= 3; ++i)
                {
                    g.fillRect(ax + (7.0f - (float) i) * px, ay - px * 0.5f - (float) i * px, px, px);
                    g.fillRect(ax + (7.0f - (float) i) * px, ay - px * 0.5f + (float) i * px, px, px);
                }
                if (hov)
                {
                    g.setColour(c.withAlpha(0.6f));
                    g.fillRect(buyRect.getX() + 8.0f * k, buyRect.getBottom() - 3.0f * k, buyRect.getWidth() - 16.0f * k, 1.0f * k);
                }
            }
            return;
        }

        for (int i = 0; i < 2; ++i)
        {
            const auto r = i == 0 ? prevRect : nextRect;
            const bool disabled = i == 0 ? page == 0 : page == 2;
            const bool hov = !disabled && hoverBtn == i + 1;
            g.saveState();
            if (disabled) g.beginTransparencyLayer(0.3f);
            drawStoneButton(g, r, 5.0f * k, disabled ? juce::Colour(0xff2e2937) : (hov ? juce::Colour(0xffffd27a) : juce::Colour(0xff4a4553)),
                            k, juce::Colour(0x1cffffff), juce::Colour(0x6a000000), 0.45f);
            juce::Path a;
            const float s = k, cx = r.getCentreX() - 8.0f * s, cy = r.getCentreY() - 5.0f * s;
            if (i == 0) { a.startNewSubPath(cx + 16 * s, cy + 5 * s); a.lineTo(cx + 2 * s, cy + 5 * s); a.startNewSubPath(cx + 6 * s, cy + 1 * s); a.lineTo(cx + 2 * s, cy + 5 * s); a.lineTo(cx + 6 * s, cy + 9 * s); }
            else        { a.startNewSubPath(cx, cy + 5 * s); a.lineTo(cx + 14 * s, cy + 5 * s); a.startNewSubPath(cx + 10 * s, cy + 1 * s); a.lineTo(cx + 14 * s, cy + 5 * s); a.lineTo(cx + 10 * s, cy + 9 * s); }
            g.setColour(hov ? juce::Colour(0xfffff4dc) : juce::Colour(0xffffd27a));
            g.strokePath(a, juce::PathStrokeType(1.6f * s));
            if (disabled) g.endTransparencyLayer();
            g.restoreState();
        }

        // trois losanges : page actuelle
        const float cy = footRect.getCentreY() + 0.3f * k;
        for (int i = 0; i < 3; ++i)
        {
            const float cx = footRect.getCentreX() + (float) (i - 1) * 17.0f * k;
            if (i == page)
            {
                softGlow(g, cx, cy, 11.0f * k, juce::Colour(0x99ff8a3a));
            }
            diamond(g, cx, cy, 4.95f * k, i == page ? juce::Colour(0xffffb347) : juce::Colour(0xff2e2937));
        }
    }

    void drawFrame(juce::Graphics& g, float k)
    {
        const auto r = panel;
        const float T = Tk, C = Ck;
        g.saveState();
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);

        auto tileRow = [&](const juce::Image& img, juce::Rectangle<float> area, bool horizontal, float offset, bool flip)
        {
            if (!img.isValid()) return;
            g.saveState();
            g.reduceClipRegion(area.toNearestInt());
            if (horizontal)
            {
                const float tw = T * 8.0f, s = T / (float) img.getHeight();
                for (float x = area.getX() + std::fmod(offset * k, tw) - tw; x < area.getRight(); x += tw)
                {
                    auto tr = juce::AffineTransform::scale(s).translated(x, area.getY());
                    if (flip) tr = juce::AffineTransform::scale(s, -s).translated(x, area.getBottom());
                    g.drawImageTransformed(img, tr);
                }
            }
            else
            {
                const float th = T * 8.0f, s = T / (float) img.getWidth();
                for (float y = area.getY() + std::fmod(offset * k, th) - th; y < area.getBottom(); y += th)
                {
                    auto tr = juce::AffineTransform::scale(s).translated(area.getX(), y);
                    if (flip) tr = juce::AffineTransform::scale(-s, s).translated(area.getRight(), y);
                    g.drawImageTransformed(img, tr);
                }
            }
            g.restoreState();
        };

        const juce::Rectangle<float> top(r.getX(), r.getY(), r.getWidth(), T), bot(r.getX(), r.getBottom() - T, r.getWidth(), T);
        const juce::Rectangle<float> lef(r.getX(), r.getY(), T, r.getHeight()), rig(r.getRight() - T, r.getY(), T, r.getHeight());
        tileRow(tileH, top, true, 0.0f, false);
        tileRow(tileH, bot, true, 37.0f, true);
        tileRow(tileV, lef, false, 0.0f, false);
        tileRow(tileV, rig, false, 53.0f, true);

        // relief : clair cote exterieur, sombre cote interieur + fin trait sombre
        auto bevel = [&](juce::Rectangle<float> a, float ox, float oy, float ix, float iy)
        {
            juce::ColourGradient gr(juce::Colour(0x1fffffff), ox, oy, juce::Colour(0x5c000000), ix, iy, false);
            gr.addColour(0.35, juce::Colour(0x00000000));
            g.setGradientFill(gr);
            g.fillRect(a);
            g.setColour(juce::Colour(0xff0b0910));
            g.drawRect(a, 1.0f);
        };
        bevel(top, 0.0f, top.getY(), 0.0f, top.getBottom());
        bevel(bot, 0.0f, bot.getBottom(), 0.0f, bot.getY());
        bevel(lef, lef.getX(), 0.0f, lef.getRight(), 0.0f);
        bevel(rig, rig.getRight(), 0.0f, rig.getX(), 0.0f);

        // coins en equerre + losange ambre qui pulse
        juce::Path shape;
        shape.startNewSubPath(0.0f, 0.0f); shape.lineTo(1.0f, 0.0f); shape.lineTo(1.0f, 0.2f); shape.lineTo(0.7f, 0.2f);
        shape.lineTo(0.53f, 0.37f); shape.lineTo(0.37f, 0.37f); shape.lineTo(0.37f, 0.53f); shape.lineTo(0.2f, 0.7f);
        shape.lineTo(0.2f, 1.0f); shape.lineTo(0.0f, 1.0f); shape.closeSubPath();
        const juce::AffineTransform tf[4] = {
            juce::AffineTransform(C, 0, r.getX(), 0, C, r.getY()),
            juce::AffineTransform(-C, 0, r.getRight(), 0, C, r.getY()),
            juce::AffineTransform(C, 0, r.getX(), 0, -C, r.getBottom()),
            juce::AffineTransform(-C, 0, r.getRight(), 0, -C, r.getBottom())
        };
        for (int i = 0; i < 4; ++i)
        {
            juce::Path p(shape);
            p.applyTransform(tf[i]);
            g.setColour(juce::Colour(0x80000000));
            g.fillPath(p, juce::AffineTransform::translation(0.0f, 2.0f));
            g.saveState();
            g.reduceClipRegion(p);
            if (tileC.isValid())
                g.drawImageTransformed(tileC, juce::AffineTransform::scale(1.0f / (float) tileC.getWidth()).followedBy(tf[i]));
            g.restoreState();

        }
        g.restoreState();
    }

    // losanges ambre des coins, qui pulsent
    void drawEmbers(juce::Graphics& g, float k, double t)
    {
        const auto r = panel;
        const float C = Ck;
        const juce::Point<float> pos[4] = { { r.getX() + C / 6.0f, r.getY() + C / 6.0f }, { r.getRight() - C / 6.0f, r.getY() + C / 6.0f },
                                            { r.getX() + C / 6.0f, r.getBottom() - C / 6.0f }, { r.getRight() - C / 6.0f, r.getBottom() - C / 6.0f } };
        const float delay[4] = { 0.0f, 0.7f, 2.1f, 1.4f }; // haut-g, haut-d, bas-g, bas-d
        for (int i = 0; i < 4; ++i)
        {
            const float ph = (float) std::fmod(t + delay[i], 2.8) / 2.8f;
            const float v = 0.5f - 0.5f * std::cos(ph * juce::MathConstants<float>::twoPi);
            const float cx = pos[i].x, cy = pos[i].y;
            const float half = 6.5f / 60.0f * C * (0.88f + 0.24f * v);
            softGlow(g, cx, cy, half + 4.0f * k, juce::Colour(0xffff8a3a).withAlpha(0.55f * (0.75f + 0.25f * v)));
            juce::Path d;
            d.addQuadrilateral(cx, cy - half, cx + half, cy, cx, cy + half, cx - half, cy);
            g.setColour(juce::Colour(0xffffb347).withAlpha(0.75f + 0.25f * v));
            g.fillPath(d);
            g.setColour(juce::Colour(0xfffff4dc).withAlpha(0.75f + 0.25f * v));
            g.strokePath(d, juce::PathStrokeType(0.9f / 60.0f * C));
        }
    }

    void drawCloseBg(juce::Graphics& g, float k)
    {
        const bool hov = hoverBtn == 3;
        if (hov) rectGlow(g, closeRect, 8.0f * k, juce::Colour(0x99ff8a3a));
        drawStoneButton(g, closeRect, 7.0f * k, hov ? juce::Colour(0xffffd27a) : juce::Colour(0xff5a5466), k,
                        juce::Colour(0x22ffffff), juce::Colour(0x7a000000), 0.40f);
    }

    void drawCloseX(juce::Graphics& g, float k, double t)
    {
        const bool hov = hoverBtn == 3;
        // croix en pixels (7 x 7), cernee de noir ; petit quart de tour a l'arrivee de la souris
        const float a = hov ? easeOut((float) ((t - closeHoverT0) / 0.35)) : 1.0f;
        const float px = 2.0f * k;
        const auto c = closeRect.getCentre();
        g.saveState();
        g.addTransform(juce::AffineTransform::scale(0.6f + 0.4f * a).rotated(-juce::MathConstants<float>::halfPi * (1.0f - a)).translated(c.x, c.y));
        auto cross = [&](juce::Colour col, float dx, float dy)
        {
            g.setColour(col);
            for (int i = 0; i < 7; ++i)
            {
                g.fillRect((float) i * px - 3.5f * px + dx, (float) i * px - 3.5f * px + dy, px, px);
                g.fillRect((float) (6 - i) * px - 3.5f * px + dx, (float) i * px - 3.5f * px + dy, px, px);
            }
        };
        const juce::Colour dark(0xff07060b);
        cross(dark, -1.0f, 0.0f); cross(dark, 1.0f, 0.0f); cross(dark, 0.0f, -1.0f); cross(dark, 0.0f, 1.0f);
        cross(hov ? juce::Colour(0xfffff4dc) : juce::Colour(0xffd9cfe3), 0.0f, 0.0f);
        g.restoreState();
    }

    void updateHover(juce::Point<float> p)
    {
        int tab = -1, card = -1, btn = 0;
        if (page == 3)
        {
            const bool lic = bloomlic::License::get().isLicensed();
            if (primaryRect.translated(content.getX(), content.getY()).contains(p)) btn = 4;
            if (!lic && secondaryRect.translated(content.getX(), content.getY()).contains(p)) btn = 5;
            if (!lic && buyRect.contains(p)) btn = 6;
            if (closeRect.contains(p)) btn = 3;
            if (btn == 3 && hoverBtn != 3) closeHoverT0 = now();
            if (btn != hoverBtn)
            {
                if (btn == 3 || hoverBtn == 3 || btn == 6 || hoverBtn == 6) chromeDirty[3] = true;
                if (btn == 4 || hoverBtn == 4 || btn == 5 || hoverBtn == 5) contentDirty[3] = true;
                hoverBtn = btn;
                setMouseCursor(swordCursor(btn != 0));
                if (btn == 3) startTimerHz(60);
                repaint();
            }
            return;
        }
        for (int i = 0; i < 3; ++i) if (tabRect[i].contains(p)) tab = i;
        if (page == 0)
            for (size_t i = 0; i < cardRects.size(); ++i)
                if (cardRects[i].translated(content.getX(), content.getY()).contains(p)) card = (int) i;
        if (prevRect.contains(p) && page > 0) btn = 1;
        if (nextRect.contains(p) && page < 2) btn = 2;
        if (closeRect.contains(p)) btn = 3;
        if (btn == 3 && hoverBtn != 3) closeHoverT0 = now();
        if (tab != hoverTab || card != hoverCard || btn != hoverBtn)
        {
            if (tab != hoverTab || btn != hoverBtn) chromeDirty[page] = true;
            if (card != hoverCard) contentDirty[page] = true;
            hoverTab = tab; hoverCard = card; hoverBtn = btn;
            setMouseCursor(swordCursor(tab >= 0 || btn != 0));
            if (btn == 3) startTimerHz(60);
            repaint();
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<Card> cards;
    std::vector<Row> pages[2];
    juce::Image tileH, tileV, tileC, logoGlow;
    juce::Rectangle<float> logoGlowFor;
    juce::Image backdropImg, chromeImg[4], contentImg[4];   // images en cache, une par page (la 4e = licence)
    juce::Point<int> chromeOrigin[4], contentOrigin[4];
    bool chromeDirty[4] = { true, true, true, true }, contentDirty[4] = { true, true, true, true };

    void invalidateAll()
    {
        for (int i = 0; i < 4; ++i) { chromeDirty[i] = contentDirty[i] = true; }
    }

    // page licence
    juce::TextEditor keyBox;
    juce::StringArray licLines;
    juce::String licMsg;
    bool licBusy = false, licMsgOk = false;
    juce::Rectangle<float> labelRect, keyRect, primaryRect, secondaryRect, msgRect, buyRect;

    int page = 0, hoverTab = -1, hoverCard = -1, hoverBtn = 0;
    double openT0 = 0.0, pageT0 = 0.0, closeHoverT0 = 0.0;
    float Tk = 13.2f, Ck = 38.4f, safeY = 0.0f;
    juce::Rectangle<float> panel, content, logoBox, tagBox, tabsBox, tabRect[3], footRect, prevRect, nextRect, closeRect;
    std::vector<juce::Rectangle<float>> cardRects, rowRects;
    std::vector<juce::StringArray> cardLines, rowLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomSheet)
};

//==============================================================================
// Le menu en haut a droite : une icone qui, au survol, deroule un panneau de pierre
// avec les 3 gemmes (BYPASS rubis, DELTA emeraude, FACONNER diamant) et la section MONDE.
//==============================================================================
class BloomMenu : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 200, kHeight = 250;

    BloomMenu(juce::TextButton& bypass, juce::TextButton& delta, juce::TextButton& pov, const juce::Image& stoneTile)
        : tile(stoneTile)
    {
        items[0] = { &bypass, T("BYPASS"), juce::Colour(0xffff7d97), juce::Colour(0xffd0224a), juce::Colour(0xff6b0b20), juce::Colour(0xffff3b63), juce::Colours::white, 0 };
        items[1] = { &delta,  T("DELTA"),  juce::Colour(0xff8dffc8), juce::Colour(0xff19b06c), juce::Colour(0xff06452b), juce::Colour(0xff2cff9a), juce::Colours::white, 1 };
        items[2] = { &pov,    T("FA\xC3\x87" "ONNER"), juce::Colour(0xffffffff), juce::Colour(0xffbfe9f7), juce::Colour(0xff6fa6c0), juce::Colour(0xffaee8ff), juce::Colour(0xff0e3347), 2 };
        setMouseCursor(swordCursor(true));
    }

    void setIconTile(const juce::Image& img) { iconTile = img; repaint(); }

    void resized() override
    {
        iconRect = juce::Rectangle<int>(kWidth - 30, 0, 30, 30);
        panelRect = juce::Rectangle<int>(0, 36, kWidth, kHeight - 36);
        bridgeRect = juce::Rectangle<int>(iconRect.getX() - 4, 28, iconRect.getWidth() + 8, 10);

        const int pad = 18;
        int y = panelRect.getY() + pad;
        for (int i = 0; i < 3; ++i)
        {
            gemRect[i] = juce::Rectangle<int>(panelRect.getX() + pad, y, panelRect.getWidth() - pad * 2, 28);
            y += 28 + 8;
        }
        y += 2;
        headerRect = juce::Rectangle<int>(panelRect.getX() + pad, y, panelRect.getWidth() - pad * 2, 12);
        y += 12 + 6;
        for (int i = 0; i < 2; ++i)
        {
            worldRect[i] = juce::Rectangle<int>(panelRect.getX() + pad, y, panelRect.getWidth() - pad * 2, 22);
            y += 22 + 6;
        }
    }

    bool hitTest(int x, int y) override
    {
        const juce::Point<int> p(x, y);
        if (iconRect.contains(p))
            return true;
        if (open > 0.02f || target)
            return panelRect.contains(p) || bridgeRect.contains(p);
        return false;
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        target = true;
        leaving = false;
        startAnim();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        leaving = true;
        closeAt = juce::Time::getMillisecondCounterHiRes() + 140.0;
        hoverGem = -1;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int h = -1;
        for (int i = 0; i < 3; ++i)
            if (open > 0.8f && gemRect[i].contains(e.getPosition()))
                h = i;
        if (h != hoverGem)
        {
            hoverGem = h;
            hoverStart = juce::Time::getMillisecondCounterHiRes();
            panelDirty = true;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (open < 0.8f)
            return;
        for (int i = 0; i < 3; ++i)
            if (gemRect[i].contains(e.getPosition()))
            {
                items[i].button->triggerClick();
                panelDirty = true;
                repaint();
            }
    }

    void paint(juce::Graphics& g) override
    {
        BLOOM_PERF_SCOPE("menu : dessin");
        const float t = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);
        drawIcon(g);
        if (open > 0.01f)
            drawPanelFast(g, t);
    }

private:
    struct Item
    {
        juce::TextButton* button;
        juce::String label;
        juce::Colour light, mid, dark, glow, text;
        int icon;
    };

    static juce::String T(const char* utf8) { return juce::String::fromUTF8(utf8); }

    void timerCallback() override
    {
        BLOOM_PERF_SCOPE("menu : minuterie");
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit(0.0, 0.05, (now - lastTick) * 0.001);
        lastTick = now;
        if (leaving && now > closeAt)
        {
            target = false;
            leaving = false;
        }
        // progression lineaire dans le temps (ouverture 0.24 s, fermeture 0.18 s), puis courbe douce
        if (target) prog = juce::jmin(1.0f, prog + dt / 0.24f);
        else        prog = juce::jmax(0.0f, prog - dt / 0.18f);
        const float inv = 1.0f - prog;
        open = 1.0f - inv * inv * inv;
        repaint();
        if (prog <= 0.0f && !target)
            stopTimer();
    }

    void startAnim()
    {
        if (!isTimerRunning())
        {
            lastTick = juce::Time::getMillisecondCounterHiRes();
            startTimerHz(60);
        }
    }

    bool anyOn() const
    {
        for (const auto& it : items)
            if (it.button->getToggleState())
                return true;
        return false;
    }

    void drawIcon(juce::Graphics& g)
    {
        const auto r = iconRect.toFloat();
        const bool hot = target || open > 0.05f;
        // bord (1 px) aux coins coupes
        g.setColour(hot ? juce::Colour(0xffffd27a) : juce::Colour(0xff4a4553));
        g.fillPath(chamferPath(r, 7.0f));
        // interieur : pierre moussue (texture x2) + leger relief
        const auto in = r.reduced(1.0f);
        const auto inner = chamferPath(in, 6.0f);
        g.saveState();
        g.reduceClipRegion(inner);
        g.setColour(juce::Colour(0xff2b2732));
        g.fillRect(in);
        if (iconTile.isValid())
        {
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(iconTile, (int) in.getX(), (int) in.getY(), iconTile.getWidth() * 2, iconTile.getHeight() * 2,
                        0, 0, iconTile.getWidth(), iconTile.getHeight());
        }
        juce::ColourGradient shade(juce::Colour(0x22ffffff), 0, in.getY(), juce::Colour(0x7a000000), 0, in.getBottom(), false);
        shade.addColour(0.40, juce::Colour(0x00000000));
        g.setGradientFill(shade);
        g.fillRect(in);
        g.restoreState();
        // trois barres (42 % de la largeur, 2 px, espacees de 3 px)
        g.setColour(hot ? juce::Colour(0xfffff4dc) : juce::Colour(0xff857d90));
        const float bw = std::round(r.getWidth() * 0.42f);
        const float bx = std::round(r.getCentreX() - bw * 0.5f);
        const float by = std::round(r.getCentreY() - 6.0f);
        for (int i = 0; i < 3; ++i)
            g.fillRect(bx, by + (float) i * 5.0f, bw, 2.0f);
        // petit point ambre si une option est active
        if (anyOn())
        {
            juce::Path d;
            const float cx = r.getRight() - 5.5f, cy = r.getY() + 5.5f;
            d.addQuadrilateral(cx, cy - 3.5f, cx + 3.5f, cy, cx, cy + 3.5f, cx - 3.5f, cy);
            softGlow(g, cx, cy, 8.0f, juce::Colour(0xaaff8a3a));
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(d);
        }
    }

    int gemStateMask() const
    {
        int m = (hoverGem + 1) << 3;
        for (int i = 0; i < 3; ++i)
            if (items[i].button->getToggleState())
                m |= 1 << i;
        return m;
    }

    // Le panneau est dessine une fois dans une image (pierre, velours, gemmes, mondes),
    // puis simplement recopie a chaque image de l'animation : deroulement tres fluide.
    void drawPanelFast(juce::Graphics& g, float t)
    {
        const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int mask = gemStateMask();
        if (panelDirty || mask != panelMask || !panelImg.isValid() || std::abs(scale - panelScale) > 0.01f)
        {
            panelScale = scale;
            panelMask = mask;
            panelDirty = false;
            const int iw = juce::roundToInt((float) (panelRect.getWidth() + 8) * scale);
            const int ih = juce::roundToInt((float) (panelRect.getHeight() + 8) * scale);
            panelImg = juce::Image(juce::Image::ARGB, iw, ih, true);
            juce::Graphics pg(panelImg);
            pg.addTransform(juce::AffineTransform::scale(scale));
            pg.addTransform(juce::AffineTransform::translation((float) (4 - panelRect.getX()), (float) (4 - panelRect.getY())));
            drawPanel(pg, t);
        }

        const float reveal = juce::jlimit(0.0f, 1.0f, open);
        const float alpha = juce::jmin(1.0f, open * 1.7f);
        g.saveState();
        g.reduceClipRegion(juce::Rectangle<int>(panelRect.getX() - 4, panelRect.getY() - 4, panelRect.getWidth() + 8,
                                                (int) (panelRect.getHeight() * reveal) + 8));
        g.setOpacity(alpha);
        g.drawImageTransformed(panelImg, juce::AffineTransform::scale(1.0f / panelScale)
                                             .translated((float) (panelRect.getX() - 4), (float) (panelRect.getY() - 4)));
        g.setOpacity(1.0f);

        // parties animees, seulement une fois le panneau ouvert
        if (open > 0.98f)
        {
            const auto pr = panelRect.toFloat();
            const juce::Point<float> rv[4] = { { pr.getX() + 8.0f, pr.getY() + 8.0f }, { pr.getRight() - 8.0f, pr.getY() + 8.0f },
                                               { pr.getX() + 8.0f, pr.getBottom() - 8.0f }, { pr.getRight() - 8.0f, pr.getBottom() - 8.0f } };
            for (int i = 0; i < 4; ++i)
            {
                const float pulse = 0.12f * (0.5f + 0.5f * std::sin(t * 2.2f + (float) i * 1.3f));
                softGlow(g, rv[i].x, rv[i].y, 9.0f, juce::Colour(0xffff8a3a).withAlpha(pulse));
            }
            for (int i = 0; i < 3; ++i)
                drawGemSweep(g, i, t);
        }
        g.restoreState();
    }

    void drawPanel(juce::Graphics& g, float t)
    {
        const auto pr = panelRect.toFloat();

        // la pierre
        const auto outline = chamferPath(pr, 5.0f);
        g.saveState();
        g.reduceClipRegion(outline);
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        const int tw = tile.getWidth() * 2, th = tile.getHeight() * 2;
        if (tw > 0 && th > 0)
            for (int ty = panelRect.getY(); ty < panelRect.getBottom(); ty += th)
                for (int tx = panelRect.getX(); tx < panelRect.getRight(); tx += tw)
                    g.drawImage(tile, tx, ty, tw, th, 0, 0, tile.getWidth(), tile.getHeight());
        juce::ColourGradient shade(juce::Colour(0x26ffffff), 0, pr.getY(), juce::Colour(0x66000000), 0, pr.getBottom(), false);
        shade.addColour(0.30, juce::Colour(0x00000000));
        g.setGradientFill(shade);
        g.fillRect(pr);
        g.restoreState();
        g.setColour(juce::Colour(0xff0b0910));
        g.strokePath(outline, juce::PathStrokeType(1.0f));

        // le lit de velours etoile
        const auto vr = pr.reduced(9.0f);
        const auto velvet = chamferPath(vr, 6.0f);
        g.saveState();
        g.reduceClipRegion(velvet);
        juce::ColourGradient vg(juce::Colour(0xff3d2a70), 0, vr.getY(), juce::Colour(0xff0c0818), 0, vr.getBottom(), false);
        vg.addColour(0.58, juce::Colour(0xff1c1334));
        g.setGradientFill(vg);
        g.fillRect(vr);
        for (float sy = vr.getY() + 3.0f; sy < vr.getBottom(); sy += 15.0f)
            for (float sx = vr.getX() + 3.0f; sx < vr.getRight(); sx += 15.0f)
            {
                g.setColour(juce::Colour(0x2bffffff));
                g.fillRect(sx, sy, 1.0f, 1.0f);
                g.setColour(juce::Colour(0x22b9a6ff));
                g.fillRect(sx + 7.0f, sy + 7.0f, 1.0f, 1.0f);
            }
        juce::ColourGradient inner(juce::Colour(0xcc000000), 0, vr.getY(), juce::Colour(0x00000000), 0, vr.getY() + 10.0f, false);
        g.setGradientFill(inner);
        g.fillRect(vr.getX(), vr.getY(), vr.getWidth(), 10.0f);
        g.restoreState();
        g.setColour(juce::Colour(0xff07060b));
        g.strokePath(velvet, juce::PathStrokeType(1.0f));
        g.setColour(juce::Colour(0x2e6b4fb0));
        g.strokePath(chamferPath(vr.reduced(1.0f), 5.0f), juce::PathStrokeType(1.0f));

        // les 4 rivets ambre
        const juce::Point<float> rv[4] = { { pr.getX() + 8.0f, pr.getY() + 8.0f }, { pr.getRight() - 8.0f, pr.getY() + 8.0f },
                                           { pr.getX() + 8.0f, pr.getBottom() - 8.0f }, { pr.getRight() - 8.0f, pr.getBottom() - 8.0f } };
        for (int i = 0; i < 4; ++i)
        {
            const float s = 3.0f;
            juce::ignoreUnused(t);
            juce::Path d;
            d.addQuadrilateral(rv[i].x, rv[i].y - s, rv[i].x + s, rv[i].y, rv[i].x, rv[i].y + s, rv[i].x - s, rv[i].y);
            softGlow(g, rv[i].x, rv[i].y, 9.0f, juce::Colour(0x99ff8a3a));
            g.setColour(juce::Colour(0xff0b0910));
            g.strokePath(d, juce::PathStrokeType(2.0f));
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(d);
        }

        for (int i = 0; i < 3; ++i)
            drawGem(g, i, t);
        drawWorlds(g);
    }

    juce::Path gemPoly(int i) const
    {
        const auto r = gemRect[i].toFloat();
        const float c = 12.0f, cy = r.getCentreY();
        juce::Path poly;
        poly.startNewSubPath(r.getX() + c, r.getY());
        poly.lineTo(r.getRight() - c, r.getY());
        poly.lineTo(r.getRight(), cy);
        poly.lineTo(r.getRight() - c, r.getBottom());
        poly.lineTo(r.getX() + c, r.getBottom());
        poly.lineTo(r.getX(), cy);
        poly.closeSubPath();
        return poly;
    }

    // eclat qui balaie la gemme (en continu si allumee, une fois au survol)
    void drawGemSweep(juce::Graphics& g, int i, float t)
    {
        const auto& it = items[(size_t) i];
        const auto r = gemRect[i].toFloat();
        const bool on = it.button->getToggleState();
        float sweepU = -1.0f;
        if (on)
            sweepU = std::fmod(t, 3.2f) / 3.2f;
        else if (hoverGem == i)
        {
            const float u = (float) ((juce::Time::getMillisecondCounterHiRes() - hoverStart) / 1100.0);
            if (u < 1.0f) sweepU = u * 0.46f;
        }
        if (sweepU >= 0.0f && sweepU < 0.46f)
        {
            const auto poly = gemPoly(i);
            const float pos = -0.45f + (sweepU / 0.45f) * 1.7f;
            const float op = juce::jmin(1.0f, sweepU / 0.12f);
            const float x0 = r.getX() + pos * r.getWidth();
            const float w = 0.28f * r.getWidth();
            g.saveState();
            g.reduceClipRegion(poly);
            juce::ColourGradient sw(juce::Colour(0x00ffffff), x0, 0, juce::Colour(0x00ffffff), x0 + w, 0, false);
            sw.addColour(0.5, juce::Colours::white.withAlpha(0.8f * op));
            g.setGradientFill(sw);
            juce::Path p;
            p.addQuadrilateral(x0, r.getBottom(), x0 + w, r.getBottom(), x0 + w + 7.0f, r.getY(), x0 + 7.0f, r.getY());
            g.fillPath(p);
            g.restoreState();
            // l'icone et le texte restent par-dessus l'eclat
            const float ix = r.getX() + 24.0f;
            drawIconGlyph(g, it.icon, ix, r.getCentreY(), it.text);
            g.setColour(it.text);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)).withExtraKerningFactor(0.18f));
            g.drawText(it.label, juce::Rectangle<float>(ix + 12.0f, r.getY(), r.getRight() - ix - 22.0f, r.getHeight()),
                       juce::Justification::centredLeft, false);
        }
    }

    void drawGem(juce::Graphics& g, int i, float t)
    {
        const auto& it = items[(size_t) i];
        const auto r = gemRect[i].toFloat();
        const bool on = it.button->getToggleState();
        const bool hover = (hoverGem == i);
        const float c = 12.0f, cy = r.getCentreY();

        juce::Path poly;
        poly.startNewSubPath(r.getX() + c, r.getY());
        poly.lineTo(r.getRight() - c, r.getY());
        poly.lineTo(r.getRight(), cy);
        poly.lineTo(r.getRight() - c, r.getBottom());
        poly.lineTo(r.getX() + c, r.getBottom());
        poly.lineTo(r.getX(), cy);
        poly.closeSubPath();

        // halo quand la gemme est allumee
        if (on)
            for (int k = 3; k >= 1; --k)
            {
                g.setColour(it.glow.withAlpha(0.11f));
                g.strokePath(poly, juce::PathStrokeType((float) k * 4.0f, juce::PathStrokeType::curved));
            }

        auto tone = [&](juce::Colour col)
        {
            if (on)    return col.brighter(0.18f);
            if (hover) return col.brighter(0.06f);
            return col.darker(0.10f);
        };
        juce::ColourGradient grad(tone(it.light), 0, r.getY(), tone(it.dark), 0, r.getBottom(), false);
        grad.addColour(0.46, tone(it.mid));
        g.setGradientFill(grad);
        g.fillPath(poly);

        // facettes
        const auto ir = juce::Rectangle<float>(r.getX() + 11.0f, r.getY() + 3.0f, r.getWidth() - 22.0f, r.getHeight() - 6.0f);
        const auto inner = [&]
        {
            juce::Path p;
            const float cc = 7.0f;
            p.startNewSubPath(ir.getX() + cc, ir.getY());
            p.lineTo(ir.getRight() - cc, ir.getY());
            p.lineTo(ir.getRight(), ir.getCentreY());
            p.lineTo(ir.getRight() - cc, ir.getBottom());
            p.lineTo(ir.getX() + cc, ir.getBottom());
            p.lineTo(ir.getX(), ir.getCentreY());
            p.closeSubPath();
            return p;
        }();

        g.saveState();
        g.reduceClipRegion(inner);
        juce::ColourGradient ig(it.dark.withAlpha(0.45f), 0, ir.getY(), it.light.withAlpha(0.60f), 0, ir.getBottom(), false);
        g.setGradientFill(ig);
        g.fillRect(ir);
        const float skew = ir.getHeight() * 0.47f;
        auto band = [&](float a, float b, juce::Colour col)
        {
            juce::Path p;
            p.addQuadrilateral(ir.getX() + a * ir.getWidth(), ir.getBottom(), ir.getX() + b * ir.getWidth(), ir.getBottom(),
                               ir.getX() + b * ir.getWidth() + skew, ir.getY(), ir.getX() + a * ir.getWidth() + skew, ir.getY());
            g.setColour(col);
            g.fillPath(p);
        };
        band(0.0f, 0.18f, juce::Colour(on ? 0x66ffffff : 0x50ffffff));
        band(0.40f, 0.62f, juce::Colour(0x26000000));
        g.restoreState();
        g.setColour(juce::Colour(0x55000000));
        g.strokePath(poly, juce::PathStrokeType(1.0f));

        // icone + texte
        const float ix = r.getX() + 24.0f;
        drawIconGlyph(g, it.icon, ix, cy, it.text);
        g.setColour(it.text);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)).withExtraKerningFactor(0.18f));
        g.drawText(it.label, juce::Rectangle<float>(ix + 12.0f, r.getY(), r.getRight() - ix - 22.0f, r.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    void drawIconGlyph(juce::Graphics& g, int kind, float cx, float cy, juce::Colour col)
    {
        g.setColour(col);
        const juce::PathStrokeType st(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        juce::Path p;
        if (kind == 0) // bouton marche/arret
        {
            p.addCentredArc(cx, cy + 0.5f, 4.6f, 4.6f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
            p.startNewSubPath(cx, cy - 5.5f);
            p.lineTo(cx, cy - 0.5f);
        }
        else if (kind == 1) // delta
        {
            p.startNewSubPath(cx, cy - 5.0f);
            p.lineTo(cx + 5.0f, cy + 4.0f);
            p.lineTo(cx - 5.0f, cy + 4.0f);
            p.closeSubPath();
        }
        else // etincelle (facon)
        {
            p.startNewSubPath(cx, cy - 5.5f);
            p.quadraticTo(cx, cy, cx + 5.5f, cy);
            p.quadraticTo(cx, cy, cx, cy + 5.5f);
            p.quadraticTo(cx, cy, cx - 5.5f, cy);
            p.quadraticTo(cx, cy, cx, cy - 5.5f);
        }
        g.strokePath(p, st);
    }

    void drawWorlds(juce::Graphics& g)
    {
        // en-tete MONDE avec filets ambre
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)).withExtraKerningFactor(0.30f));
        const auto hr = headerRect.toFloat();
        const float tw = 44.0f;
        const float tx = hr.getCentreX() - tw * 0.5f;
        g.setColour(juce::Colour(0xff000000));
        g.drawText(T("MONDE"), juce::Rectangle<float>(tx, hr.getY() + 1.0f, tw, hr.getHeight()), juce::Justification::centred, false);
        g.setColour(juce::Colour(0xffb8aec6));
        g.drawText(T("MONDE"), juce::Rectangle<float>(tx, hr.getY(), tw, hr.getHeight()), juce::Justification::centred, false);
        const float ly = hr.getCentreY();
        juce::ColourGradient l1(juce::Colour(0x00ffb347), hr.getX(), ly, juce::Colour(0xaaffb347), tx - 6.0f, ly, false);
        g.setGradientFill(l1);
        g.fillRect(hr.getX(), ly, tx - 6.0f - hr.getX(), 1.0f);
        juce::ColourGradient l2(juce::Colour(0xaaffb347), tx + tw + 6.0f, ly, juce::Colour(0x00ffb347), hr.getRight(), ly, false);
        g.setGradientFill(l2);
        g.fillRect(tx + tw + 6.0f, ly, hr.getRight() - (tx + tw + 6.0f), 1.0f);

        const char* names[2] = { "\xC3\x8ELE FLOTTANTE", "MUSHROOM ISLAND" };
        for (int i = 0; i < 2; ++i)
        {
            const bool on = (i == 0);
            const auto r = worldRect[i].toFloat();
            const auto shape = chamferPath(r, 6.0f);
            juce::ColourGradient bg(on ? juce::Colour(0xff4a3a78) : juce::Colour(0xff251d3d), 0, r.getY(),
                                    on ? juce::Colour(0xff2a1f55) : juce::Colour(0xff171230), 0, r.getBottom(), false);
            g.setGradientFill(bg);
            g.fillPath(shape);
            g.setColour(on ? juce::Colour(0xff6b4fb0) : juce::Colour(0xff3d3166));
            g.strokePath(shape, juce::PathStrokeType(1.0f));

            const float mx = r.getX() + 13.0f, my = r.getCentreY();
            juce::Path d;
            d.addQuadrilateral(mx, my - 3.5f, mx + 3.5f, my, mx, my + 3.5f, mx - 3.5f, my);
            if (on)
            {
                softGlow(g, mx, my, 9.0f, juce::Colour(0xaaff8a3a));
                g.setColour(juce::Colour(0xffffb347));
                g.fillPath(d);
            }
            else
            {
                g.setColour(juce::Colour(0xff6d6678));
                g.strokePath(d, juce::PathStrokeType(1.0f));
            }
            g.setColour(on ? juce::Colour(0xfffff4dc) : juce::Colour(0xff8c82a0));
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)).withExtraKerningFactor(0.08f));
            g.drawText(T(names[i]), juce::Rectangle<float>(r.getX() + 24.0f, r.getY(), r.getWidth() - 30.0f, r.getHeight()),
                       juce::Justification::centredLeft, false);
            if (!on)
            {
                g.setColour(juce::Colour(0xffffb347));
                g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)).withExtraKerningFactor(0.10f));
                g.drawText(T("BIENT\xC3\x94T"), juce::Rectangle<float>(r.getX(), r.getY(), r.getWidth() - 8.0f, r.getHeight()),
                           juce::Justification::centredRight, false);
            }
        }
    }

    Item items[3];
    juce::Image tile;
    juce::Rectangle<int> iconRect, panelRect, bridgeRect, headerRect, gemRect[3], worldRect[2];
    juce::Image iconTile;
    float open = 0.0f, prog = 0.0f;
    bool target = false, leaving = false;
    double closeAt = 0.0, hoverStart = 0.0, lastTick = 0.0;
    juce::Image panelImg;
    float panelScale = 0.0f;
    int panelMask = -1;
    bool panelDirty = true;
    int hoverGem = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomMenu)
};

//==============================================================================
// La chaine argentee qui pend du cadre : un pendule, qui bouge doucement
// et reagit (un peu) quand on passe la souris dessus.
//==============================================================================
//==============================================================================
// Etiquette "DEMO" en haut a gauche (seulement sans licence) : clic = page d'activation
//==============================================================================
class BloomDemoTag : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 84, kHeight = 30;
    std::function<void()> onClick;

    BloomDemoTag() { setMouseCursor(swordCursor(true)); setVisible(true); startTimerHz(20); }
    ~BloomDemoTag() override { stopTimer(); }

    void setTile(const juce::Image& t) { tile = t; repaint(); }

    // affiche ou cache l'etiquette selon l'etat de la licence
    void refresh()
    {
        // en demo, en essai ou active : l'etiquette reste, et dit lequel
        setVisible(true);
        if (!isTimerRunning())
            startTimerHz(20);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour(hover ? juce::Colour(0xffffd27a) : juce::Colour(0xff6b5a3a));
        g.fillPath(chamferPath(r, 7.0f));
        const auto in = r.reduced(1.0f);
        const auto inner = chamferPath(in, 6.0f);
        g.saveState();
        g.reduceClipRegion(inner);
        g.setColour(juce::Colour(0xff2b2732));
        g.fillRect(in);
        if (tile.isValid())
        {
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            for (int x = 0; x < getWidth(); x += tile.getWidth() * 2)
                g.drawImage(tile, (int) in.getX() + x, (int) in.getY(), tile.getWidth() * 2, tile.getHeight() * 2,
                            0, 0, tile.getWidth(), tile.getHeight());
        }
        juce::ColourGradient shade(juce::Colour(0x22ffffff), 0, in.getY(), juce::Colour(0x8a000000), 0, in.getBottom(), false);
        shade.addColour(0.40, juce::Colour(0x00000000));
        g.setGradientFill(shade);
        g.fillRect(in);
        g.restoreState();

        // losange ambre qui pulse + texte
        const float t = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);
        const float v = 0.5f - 0.5f * std::cos(t * 2.4f);
        const float cx = 14.0f, cy = r.getCentreY();
        // l'ambre du plugin dans tous les cas : c'est le mot qui dit l'etat
        auto& licence = bloomlic::License::get();
        const bool trial = licence.isTrial(), active = licence.isLicensed() && !trial;
        const juce::Colour main(0xffffb347), bright(0xfffff4dc);
        softGlow(g, cx, cy, 9.0f + 2.0f * v, juce::Colour(0xffff8a3a).withAlpha(0.45f + 0.35f * v));
        juce::Path d;
        const float h = 3.6f + 0.6f * v;
        d.addQuadrilateral(cx, cy - h, cx + h, cy, cx, cy + h, cx - h, cy);
        g.setColour(main);
        g.fillPath(d);
        g.setColour(bright);
        g.strokePath(d, juce::PathStrokeType(0.8f));

        const juce::String label = juce::String::fromUTF8(active ? (hover ? "LICENCE" : "ACTIV\xC3\x89")
                                                               : trial ? (hover ? "LICENCE" : "ESSAI")
                                                                       : (hover ? "ACTIVER" : "D\xC3\x89MO"));
        const float tx = 25.0f, tw = r.getWidth() - tx - 7.0f;
        float size = 9.0f, spacing = 1.2f;
        juce::Font f = bloomfonts::pixelFont(size, true, spacing);
        while (juce::GlyphArrangement::getStringWidth(f, label) > tw && size > 6.5f)
        {
            size -= 0.5f;
            spacing = juce::jmax(0.4f, spacing - 0.2f);
            f = bloomfonts::pixelFont(size, true, spacing);
        }
        g.setFont(f);
        const float w = juce::GlyphArrangement::getStringWidth(f, label);
        const float base = cy + (f.getAscent() - f.getDescent()) * 0.5f;
        g.setColour(juce::Colour(0xff07060b));
        g.drawSingleLineText(label, juce::roundToInt(tx + (tw - w) * 0.5f), juce::roundToInt(base + 1.0f));
        g.setColour(hover ? bright : main);
        g.drawSingleLineText(label, juce::roundToInt(tx + (tw - w) * 0.5f), juce::roundToInt(base));
    }

    void mouseEnter(const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }

private:
    void timerCallback() override { if (isShowing()) repaint(); }

    juce::Image tile;
    bool hover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomDemoTag)
};

class BloomChain : public juce::Component, private juce::Timer
{
public:
    BloomChain()
    {
        juce::Random rn(7);
        for (int i = 0; i < kMax; ++i)
        {
            texX[i] = rn.nextInt(24);
            texY[i] = rn.nextInt(24);
            rust[i] = 0.15f + rn.nextFloat() * 0.5f;
            dirt[i] = 0.08f + rn.nextFloat() * 0.2f;
        }
        setMouseCursor(swordCursor(true));
        lastTime = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(30);
    }

    void setTile(const juce::Image& stone) { tile = stone; repaint(); }

    // pivot : point d'accroche (coordonnees de l'editeur), length : longueur de la chaine, pitch : pas entre maillons
    void setGeometry(float pivotX, float pivotY, float length, float pitchPx, float gemSizePx)
    {
        pitch = pitchPx;
        gemSize = gemSizePx;
        pivot = { 75.0f, pivotY };
        chainLen = length;
        links = juce::jlimit(2, kMax, (int) (length / pitchPx));
        setBounds((int) (pivotX - 75.0f), 0, 150, (int) (pivotY + length + 40.0f));
    }

    bool hitTest(int x, int y) override
    {
        return std::abs((float) x - pivot.x) < 16.0f && (float) y > pivot.y && (float) y < pivot.y + chainLen + 8.0f;
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        lastX = (float) e.x;
        vel += (juce::Random::getSystemRandom().nextBool() ? -0.4f : 0.4f);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        vel += juce::jlimit(-1.0f, 1.0f, ((float) e.x - lastX) * 0.04f);
        lastX = (float) e.x;
    }

    void paint(juce::Graphics& g) override
    {
        BLOOM_PERF_SCOPE("chaine : dessin");
        const float t = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);
        const float th = ang + 0.03f * std::sin(t * 0.9f) + 0.008f * std::sin(t * 2.3f + 1.0f);
        const float dx = std::sin(th), dy = std::cos(th);
        const float rot = juce::MathConstants<float>::halfPi - th;

        for (int pass = 0; pass < 2; ++pass) // 0 : ombre portee, 1 : chaine
        {
            const float ox = pass == 0 ? 1.0f : 0.0f, oy = pass == 0 ? 3.0f : 0.0f;
            for (int i = 0; i < links; ++i)
            {
                const float d = (float) i * pitch + pitch * 0.5f;
                drawLink(g, pivot.x + dx * d + ox, pivot.y + dy * d + oy, rot, (i % 2) == 0, i, pass == 0);
            }
            // le cristal ambre au bout (carre tourne de 45 degres)
            const float d = (float) links * pitch + pitch * 0.3f;
            const float cx = pivot.x + dx * d + ox, cy = pivot.y + dy * d + oy;
            const float s = gemSize * 0.70710678f; // demi-diagonale
            juce::Path gem;
            gem.addQuadrilateral(cx, cy - s, cx + s, cy, cx, cy + s, cx - s, cy);
            if (pass == 0)
            {
                g.setColour(juce::Colour(0x8c000000));
                g.fillPath(gem);
            }
            else
            {
                softGlow(g, cx, cy, gemSize * 0.5f + 6.0f, juce::Colour(0x99ff8a3a));
                g.setColour(juce::Colour(0xff0b0910));
                g.strokePath(gem, juce::PathStrokeType(2.0f));
                juce::ColourGradient gg(juce::Colour(0xfffff4dc), cx + 0.035f * gemSize, cy - 0.247f * gemSize,
                                        juce::Colour(0xffa8501a), cx + 0.035f * gemSize + 0.955f * gemSize, cy - 0.247f * gemSize, true);
                gg.addColour(0.45, juce::Colour(0xffffb347));
                g.setGradientFill(gg);
                g.fillPath(gem);
            }
        }
    }

private:
    static constexpr int kMax = 24;

    // meme reglage que le prototype : luminosite 0.95, contraste 1.15
    static juce::Colour bc(int r, int gr, int b, float a)
    {
        auto f = [](int v) { return (juce::uint8) juce::jlimit(0, 255, juce::roundToInt(((v / 255.0f * 0.95f) - 0.5f) * 1.15f * 255.0f + 127.5f)); };
        return juce::Colour(f(r), f(gr), f(b), a);
    }

    void timerCallback() override
    {
        BLOOM_PERF_SCOPE("chaine : minuterie");
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = juce::jlimit(0.0f, 0.05f, (float) ((now - lastTime) * 0.001));
        lastTime = now;
        vel += (-22.0f * ang - 1.6f * vel) * dt;
        ang += vel * dt;
        ang = juce::jlimit(-0.45f, 0.45f, ang);
        if (isShowing())
            repaint();
    }

    void drawLink(juce::Graphics& g, float x, float y, float rot, bool front, int idx, bool shadow)
    {
        // maillon de face : plaque avec un trou ovale ; maillon de profil : fine barre
        const float len = front ? pitch * 1.6f : pitch * 1.15f;
        const float thick = front ? pitch * 1.0f : pitch * 0.34f;
        const juce::Rectangle<float> r(x - len * 0.5f, y - thick * 0.5f, len, thick);

        juce::Path shape;
        if (front)
        {
            shape.addRectangle(r);
            shape.addEllipse(juce::Rectangle<float>(len * 0.515f, thick * 0.515f).withCentre({ x, y }));
            shape.setUsingNonZeroWinding(false);
        }
        else
            shape.addRoundedRectangle(r, 1.0f);

        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(rot, x, y));
        if (shadow)
        {
            g.setColour(juce::Colour(0x8c000000));
            g.fillPath(shape);
            g.setColour(juce::Colour(0x40000000));
            g.strokePath(shape, juce::PathStrokeType(2.0f));
        }
        else
        {
            // contour sombre fin (comme le drop-shadow du prototype)
            g.setColour(juce::Colour(0xd9000000));
            g.strokePath(shape, juce::PathStrokeType(1.6f));

            g.saveState();
            g.reduceClipRegion(shape);
            // 1) texture de pierre (base)
            g.setColour(bc(138, 144, 160, 1.0f));
            g.fillRect(r);
            if (tile.isValid())
            {
                g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
                for (float ty = r.getY() - (float) texY[idx % kMax]; ty < r.getBottom(); ty += 24.0f)
                    for (float tx = r.getX() - (float) texX[idx % kMax]; tx < r.getRight(); tx += 24.0f)
                        g.drawImageAt(tile, (int) std::floor(tx), (int) std::floor(ty));
            }
            // 2) sombrissement (salete)
            g.setColour(juce::Colour::fromRGBA(20, 16, 12, (juce::uint8) (dirt[idx % kMax] * 255.0f)));
            g.fillRect(r);
            // 3) rouille
            const float rr = 0.6f * std::hypot(len * 0.7f, thick * 0.6f);
            const float rx = r.getX() + len * 0.3f, ry = r.getY() + thick * 0.4f;
            juce::ColourGradient rg(juce::Colour::fromRGBA(110, 70, 40, (juce::uint8) (rust[idx % kMax] * 0.9f * 255.0f)), rx, ry,
                                    juce::Colour::fromRGBA(110, 70, 40, 0), rx + rr, ry, true);
            g.setGradientFill(rg);
            g.fillRect(r);
            // 4) le metal argente (reflets fixes sur l'epaisseur du maillon)
            juce::ColourGradient grad(bc(214, 220, 232, 0.90f), x, r.getY(), bc(150, 156, 170, 0.90f), x, r.getBottom(), false);
            grad.addColour(0.24, bc(150, 158, 174, 0.88f));
            grad.addColour(0.40, bc(196, 202, 214, 0.92f));
            grad.addColour(0.64, bc(86, 92, 108, 0.90f));
            grad.addColour(0.82, bc(54, 58, 72, 0.92f));
            g.setGradientFill(grad);
            g.fillRect(r);
            g.restoreState();
        }
        g.restoreState();
    }

    juce::Image tile;
    float pitch = 10.0f, chainLen = 100.0f, gemSize = 11.0f, ang = 0.0f, vel = 0.0f, lastX = 0.0f;
    int links = 10;
    juce::Point<float> pivot { 75.0f, 12.0f };
    float rust[kMax] = {}, dirt[kMax] = {};
    int texX[kMax] = {}, texY[kMax] = {};
    double lastTime = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BloomChain)
};

//==============================================================================
BloomView::BloomView(BLOOMAudioProcessor& p)
    : audioProcessor(p)
{
    createdAt = juce::Time::getMillisecondCounterHiRes();
    poster = bloomGetPoster();
    bloomperf::resetClock();
    bloomperf::mark("editeur : debut de creation");
    // Les 7 arcs (ils gerent eux-memes leurs reglages)
    arcs = std::make_unique<BloomArcsLayer>(audioProcessor.parameters, hoverIndex);
    arcs->setAnchorSource([this](int index, float& x, float& y) { return scene != nullptr && scene->getAnchor(index, x, y); });
    addAndMakeVisible(*arcs);

    setupSwitch(bypassButton, "Compare avec et sans BLOOM, a volume egal.", juce::Colour(0xffe0457b));
    setupSwitch(deltaButton, "N'ecoute que ce que BLOOM ajoute ou retire.", juce::Colour(0xff3fc1c9));
    setupSwitch(povButton, "Point de vue 2 : tu regles en touchant directement les objets du monde.", juce::Colour(0xffffb347));
    povButton.onClick = [this]
    {
        arcs->setWorldMode(povButton.getToggleState());
    };

    // la fiche explicative (cachee tant qu'on ne clique pas sur le logo)
    sheet = std::make_unique<BloomSheet>(audioProcessor.parameters);
    addChildComponent(*sheet);

    // On relie chaque bouton a son reglage
    auto& params = audioProcessor.parameters;
    bypassAttachment = std::make_unique<ButtonAttachment>(params, "bypass", bypassButton);
    deltaAttachment = std::make_unique<ButtonAttachment>(params, "delta", deltaButton);

    bloomperf::mark("editeur : arcs, fiche et boutons crees");
    // --- Etape 1 : on fabrique les pierres et le logo une seule fois ---
    const juce::Image horiz = makeStoneTile(64, 8, 4, 0.85f, 22, 20261005);
    tileHTop = horiz;
    tileHBottom = flipImage(horiz, false, true);

    const juce::Image vert = transposeImage(makeStoneTile(64, 8, 4, 0.60f, 22, 20261006)); // 8 x 64
    tileVLeft = vert;
    tileVRight = flipImage(vert, true, false);

    const juce::Image corner = makeStoneTile(24, 24, 6, 0.65f, 18, 20261007);
    tileCorner[0] = corner;
    tileCorner[1] = flipImage(corner, true, false);
    tileCorner[2] = flipImage(corner, false, true);
    tileCorner[3] = flipImage(corner, true, true);

    logoPath = makeLogoPath();

    bloomperf::mark("editeur : pierres et logo prets");
    // le menu (icone en haut a droite) et la chaine qui pend du cadre
    menu = std::make_unique<BloomMenu>(bypassButton, deltaButton, povButton, tileHTop);
    addAndMakeVisible(*menu);
    chain = std::make_unique<BloomChain>();
    chain->setTile(tileCorner[0]);
    sheet->setTiles(tileHTop, tileVLeft, tileCorner[0]);
    menu->setIconTile(tileCorner[0]);
    addAndMakeVisible(*chain);

    // licence : l'etiquette en haut a gauche (demo, essai ou active) et la page de la licence dans la fiche
    demoTag = std::make_unique<BloomDemoTag>();
    demoTag->setTile(tileCorner[0]);
    demoTag->onClick = [this] { sheet->openLicense(); };
    addAndMakeVisible(*demoTag);
    demoTag->refresh();
    lastLicenseState = bloomlic::License::get().stateCode();
    sheet->onLicenseChanged = [this] { demoTag->refresh(); };
    // en mode demo, la page d'activation s'ouvre toute seule une fois par session de FL
    if (!bloomlic::License::get().isLicensed() && firstOpenThisSession())
        licenseNagPending = true;
    sheet->toFront(false);
    setMouseCursor(swordCursor(false));

    bloomperf::mark("editeur : menu et chaine crees");
    setSize(600, 750); // format 4:5
    bloomperf::mark("editeur : mise en page faite");

    // --- Etape 3 : le monde en 3D (OpenGL), dessine derriere les arcs, le cadre et les boutons ---
    scene = std::make_unique<BloomScene>(audioProcessor.parameters, glContext, *this, hoverIndex);
    glContext.setRenderer(scene.get());
    glContext.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
    glContext.setComponentPaintingEnabled(true);   // JUCE dessine le reste de l'interface par-dessus la 3D
    glContext.setContinuousRepainting(true);       // dessin synchronise sur l'ecran (fluide)
    // on branche la 3D seulement quand le decor est pret : sa toute premiere image est alors complete
    if (bloomSceneDataReady())
        attachGl();
    startTimerHz(30);
    bloomperf::mark("editeur : creation terminee");                               // surveille si la fenetre est visible
}

BloomView::~BloomView()
{
    stopTimer();
    arcs->setWorldMode(false);
    glContext.detach();
    bloomSavePosterIfNeeded();
    bloomperf::mark("editeur : fermeture");
    bloomperf::flush();
}

// Vrai seulement la toute premiere fois dans cette session de FL (meme si FL decharge puis recharge BLOOM) :
// on note le numero du processus de FL dans un petit fichier temporaire.
bool BloomView::firstOpenThisSession()
{
   #if JUCE_WINDOWS
    const juce::String pid((juce::int64) _getpid());
   #else
    const juce::String pid((juce::int64) getpid());
   #endif
    const auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("bloom_session.txt");
    if (f.existsAsFile() && f.loadFileAsString().trim() == pid)
        return false;
    f.replaceWithText(pid);
    return true;
}

// L'outil d'apercu d'ONIRIQUE CENTRAL ouvre le plugin, tourne chaque reglage et photographie ce qui change. Un monde en
// 3D qui bouge sans arret lui cache les reglages : pour lui, BLOOM garde l'interface dessinee sans OpenGL, celle qui
// s'affiche d'habitude le temps que la 3D demarre.
static bool bloomIsPreviewHost()
{
    static const bool preview = juce::File::getSpecialLocation(juce::File::hostApplicationPath)
                                    .getFileNameWithoutExtension().containsIgnoreCase("probe");
    return preview;
}

void BloomView::attachGl()
{
    if (glAttached || bloomIsPreviewHost())
        return;
    glAttached = true;
    glContext.attachTo(*this);
    bloomperf::mark("editeur : 3D branchee");
}

bool BloomView::isGlLive() const
{
    return glAttached && scene != nullptr && scene->hasPresentedWorld();
}

// Dessin "logiciel" de toute l'interface (utilise seulement pendant les premieres fractions de seconde)
void BloomView::paintFallback(juce::Graphics& g)
{
    BLOOM_PERF_SCOPE("editeur : dessin de demarrage", 15.0);
    if (poster.isValid())
        g.drawImage(poster, getLocalBounds().toFloat());
    else
        g.fillAll(juce::Colour::fromFloatRGBA(0.05f, 0.04f, 0.08f, 1.0f));
    if (glAttached)
        paintEntireComponent(g, false);   // une fois la 3D branchee, l'interface ne se dessine plus toute seule
}

void BloomView::timerCallback()
{
    // diagnostic : fil de l'interface bloque ? traitement audio trop lent ?
    {
        const double t = juce::Time::getMillisecondCounterHiRes();
        const double expected = (double) getTimerInterval();
        if (perfLastTick > 0.0 && t - perfLastTick > expected + 150.0)
            bloomperf::mark("interface : bloquee " + juce::String(t - perfLastTick - expected, 0) + " ms");
        perfLastTick = t;
        // resume audio toutes les 5 secondes
        if (t - perfLastAudioSummary > 5000.0)
        {
            perfLastAudioSummary = t;
            const float worst = audioProcessor.maxBlockLoad.exchange(0.0f);
            const auto busy  = audioProcessor.perfBusyTicks.exchange(0);
            const auto avail = audioProcessor.perfAvailTicks.exchange(0);
            const int blocks = audioProcessor.perfBlocks.exchange(0);
            const int over   = audioProcessor.perfOverBlocks.exchange(0);
            if (blocks > 0)
                bloomperf::mark("audio : moyenne " + juce::String(100.0 * (double) busy / juce::jmax(1.0, (double) avail), 1)
                                + " %, pire " + juce::String(juce::roundToInt(worst * 100.0f)) + " %, blocs trop lents "
                                + juce::String(over) + "/" + juce::String(blocks)
                                + ", taille " + juce::String(audioProcessor.perfLastBlockSize.load())
                                + " @ " + juce::String(juce::roundToInt(audioProcessor.getSampleRate())) + " Hz");
        }
    }
    BLOOM_PERF_SCOPE("editeur : minuterie");

    // la licence s'active depuis ONIRIQUE CENTRAL, fenetre ouverte : l'etiquette et la page suivent
    if (const int licenseState = bloomlic::License::get().stateCode(); licenseState != lastLicenseState)
    {
        lastLicenseState = licenseState;
        demoTag->refresh();
        sheet->licenseChanged();
    }

    // page d'activation automatique (mode demo, une fois par session), quand la fenetre est bien affichee
    if (licenseNagPending && (isGlLive() || juce::Time::getMillisecondCounterHiRes() - createdAt > 4000.0)
        && juce::Time::getMillisecondCounterHiRes() - createdAt > 600.0)
    {
        licenseNagPending = false;
        if (!bloomlic::License::get().isLicensed() && !sheet->isVisible())
            sheet->openLicense();
    }

    // demarrage de la 3D : on la branche des que le decor est pret (ou au bout de 3 s quoi qu'il arrive)
    if (!glAttached)
    {
        if (bloomSceneDataReady() || juce::Time::getMillisecondCounterHiRes() - createdAt > 3000.0)
            attachGl();
    }
    else if (!isGlLive())
    {
        // la 3D n'a pas encore pris le relais : on garde l'interface vivante en dessin logiciel
        if (auto* parent = getParentComponent())
            parent->repaint();
    }

    // effet lumineux du titre : monte en 0.28 s, redescend pareil
    {
        const double t = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit(0.0, 0.1, (t - logoHoverLast) * 0.001);
        logoHoverLast = t;
        const float target = logoHover ? 1.0f : 0.0f;
        if (logoHoverAmt != target || logoHover)
        {
            logoHoverAmt = target > logoHoverAmt ? juce::jmin(1.0f, logoHoverAmt + dt / 0.28f)
                                                 : juce::jmax(0.0f, logoHoverAmt - dt / 0.28f);
            repaint(logoFxArea());
        }
        else if (getTimerInterval() < 200 && isGlLive())
            startTimerHz(4); // effet termine : on revient au rythme lent
    }

    // pas de dessin (donc pas de CPU/GPU) quand la fenetre du plugin est cachee
    if (menu != nullptr)
        menu->repaint();
    if (demoTag != nullptr)
        demoTag->refresh();
    const bool show = isShowing();
    if (show != sceneRunning)
    {
        sceneRunning = show;
        glContext.setContinuousRepainting(show);
    }
}

// Reglages communs aux interrupteurs
void BloomView::setupSwitch(juce::TextButton& button, const juce::String& help, juce::Colour onColour)
{
    button.setClickingTogglesState(true);
    button.setColour(juce::TextButton::buttonOnColourId, onColour);
    juce::ignoreUnused(help); // plus de bulles d'aide au survol
    addChildComponent(button);   // invisible : c'est le menu qui les pilote
}

//==============================================================================
// Dessine le cadre complet (barres + coins + losanges ambre) dans une image transparente
void BloomView::rebuildFrameCache()
{
    const int W = getWidth();
    const int H = getHeight();
    if (W <= 0 || H <= 0)
        return;

    frameCache = juce::Image(juce::Image::ARGB, W, H, true);
    juce::Graphics g(frameCache);
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality); // pixels nets, pas de lissage

    const int s = texelScale(W);
    const int T = 8 * s;   // epaisseur des barres
    const int C = 24 * s;  // taille des coins

    // 1) Les quatre barres, en repetant le morceau de pierre
    for (int x = 0; x < W; x += 64 * s)
    {
        g.drawImage(tileHTop,    x, 0,     64 * s, T, 0, 0, 64, 8);
        g.drawImage(tileHBottom, x, H - T, 64 * s, T, 0, 0, 64, 8);
    }
    for (int y = 0; y < H; y += 64 * s)
    {
        g.drawImage(tileVLeft,  0,     y, T, 64 * s, 0, 0, 8, 64);
        g.drawImage(tileVRight, W - T, y, T, 64 * s, 0, 0, 8, 64);
    }

    // 2) Relief : un peu de lumiere au bord exterieur, de l'ombre cote interieur
    auto bevel = [&g](juce::Rectangle<int> r, float ox, float oy, float ix, float iy)
    {
        juce::ColourGradient gr(juce::Colour(0x1fffffff), ox, oy, juce::Colour(0x5c000000), ix, iy, false);
        gr.addColour(0.35, juce::Colour(0x00000000));
        g.setGradientFill(gr);
        g.fillRect(r);
    };
    bevel(juce::Rectangle<int>(0, 0, W, T),     0.0f, 0.0f,      0.0f, (float) T);
    bevel(juce::Rectangle<int>(0, H - T, W, T), 0.0f, (float) H, 0.0f, (float) (H - T));
    bevel(juce::Rectangle<int>(0, 0, T, H),     0.0f, 0.0f,      (float) T, 0.0f);
    bevel(juce::Rectangle<int>(W - T, 0, T, H), (float) W, 0.0f, (float) (W - T), 0.0f);

    // fin trait sombre sur le bord interieur
    g.setColour(juce::Colour(0xff0b0910));
    g.fillRect(0, T - 1, W, 1);
    g.fillRect(0, H - T, W, 1);
    g.fillRect(T - 1, 0, 1, H);
    g.fillRect(W - T, 0, 1, H);

    // 3) Les quatre coins en equerre, avec leur losange ambre
    juce::Path cornerShape;
    cornerShape.startNewSubPath(0.0f, 0.0f);
    cornerShape.lineTo(1.0f, 0.0f);
    cornerShape.lineTo(1.0f, 0.2f);
    cornerShape.lineTo(0.7f, 0.2f);
    cornerShape.lineTo(0.53f, 0.37f);
    cornerShape.lineTo(0.37f, 0.37f);
    cornerShape.lineTo(0.37f, 0.53f);
    cornerShape.lineTo(0.2f, 0.7f);
    cornerShape.lineTo(0.2f, 1.0f);
    cornerShape.lineTo(0.0f, 1.0f);
    cornerShape.closeSubPath();

    const float fC = (float) C;
    const float fW = (float) W;
    const float fH = (float) H;

    const juce::AffineTransform transforms[4] = {
        juce::AffineTransform(fC, 0.0f, 0.0f, 0.0f, fC, 0.0f),     // haut-gauche
        juce::AffineTransform(-fC, 0.0f, fW, 0.0f, fC, 0.0f),      // haut-droite
        juce::AffineTransform(fC, 0.0f, 0.0f, 0.0f, -fC, fH),      // bas-gauche
        juce::AffineTransform(-fC, 0.0f, fW, 0.0f, -fC, fH)        // bas-droite
    };
    const int destX[4] = { 0, W - C, 0, W - C };
    const int destY[4] = { 0, 0, H - C, H - C };

    for (int i = 0; i < 4; ++i)
    {
        juce::Path shape(cornerShape);
        shape.applyTransform(transforms[i]);

        // la pierre du coin, decoupee a la forme de l'equerre
        g.saveState();
        g.reduceClipRegion(shape);
        g.drawImage(tileCorner[i], destX[i], destY[i], C, C, 0, 0, 24, 24);
        g.restoreState();

        g.setColour(juce::Colour(0xff0b0910));
        g.strokePath(shape, juce::PathStrokeType(1.0f));

        // losange ambre
        float cx = 0.167f;
        float cy = 0.167f;
        transforms[i].transformPoint(cx, cy);
        const float half = 0.108f * fC;

        juce::ColourGradient glow(juce::Colour(0x66ffb347), cx, cy,
                                  juce::Colour(0x00ffb347), cx + half * 3.0f, cy, true);
        g.setGradientFill(glow);
        g.fillEllipse(cx - half * 3.0f, cy - half * 3.0f, half * 6.0f, half * 6.0f);

        juce::Path diamond;
        diamond.startNewSubPath(cx, cy - half);
        diamond.lineTo(cx + half, cy);
        diamond.lineTo(cx, cy + half);
        diamond.lineTo(cx - half, cy);
        diamond.closeSubPath();

        g.setColour(juce::Colour(0xffffb347));
        g.fillPath(diamond);
        g.setColour(juce::Colour(0xfffff4dc));
        g.strokePath(diamond, juce::PathStrokeType(1.0f));
    }
}

//==============================================================================
void BloomView::drawLogo(juce::Graphics& g)
{
    const auto r = logoBounds(getWidth());
    const float u = logoUnit(getWidth());
    const float ox = r.getX(), oy = r.getY();
    const float hv = logoHoverAmt * logoHoverAmt * (3.0f - 2.0f * logoHoverAmt); // adouci
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    const juce::Path fillP    = makeLogoPath(0.0f,  ox, oy, u, true);
    const juce::Path outlineP = makeLogoPath(0.4f,  ox, oy, u, true);
    const juce::Path shadowP  = makeLogoPath(0.45f, ox + 0.55f * u, oy + 0.75f * u, u, true);

    // halos ambre (calcules une seule fois par taille ; membres de l'editeur, jamais "static")
    // le flou est fait sur une image 4x plus petite puis agrandi : quasi instantane
    const float ds = 4.0f;
    auto makeGlow = [&](juce::Image& img, int& forW, int pad, float radius)
    {
        if (forW == getWidth() && img.isValid())
            return;
        forW = getWidth();
        img = juce::Image(juce::Image::ARGB, (int) ((r.getWidth() + pad * 2) / ds) + 1, (int) ((r.getHeight() + pad * 2) / ds) + 1, true);
        {
            juce::Graphics gg(img);
            gg.setColour(juce::Colour(0xffff8a3a));
            gg.fillPath(outlineP, juce::AffineTransform::translation(pad - std::floor(ox), pad - std::floor(oy)).scaled(1.0f / ds));
        }
        const float rr = juce::jmax(0.6f, radius / ds);
        juce::ImageConvolutionKernel kk(2 * (int) std::ceil(rr * 2.5f) + 1);
        kk.createGaussianBlur(rr);
        kk.applyToImage(img, img, img.getBounds());
    };
    auto drawGlow = [&](const juce::Image& img, int pad, float opacity)
    {
        g.saveState();
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.setOpacity(opacity);
        g.drawImageTransformed(img, juce::AffineTransform::scale(ds).translated(std::floor(ox) - (float) pad, std::floor(oy) - (float) pad));
        g.restoreState();
    };
    makeGlow(logoGlow, logoGlowW, 16, 4.5f);
    drawGlow(logoGlow, 16, 0.32f);
    if (hv > 0.0f)
    {
        makeGlow(logoGlowHi, logoGlowHiW, 40, 13.0f);
        drawGlow(logoGlowHi, 40, 0.8f * hv);
    }

    // ombre portee decalee en pixels (lettres "gravees")
    g.setColour(juce::Colour(0xb305040a));
    g.fillPath(shadowP);

    // contour sombre
    g.setColour(juce::Colour(0xff0d0a14));
    g.fillPath(outlineP);

    // lettres : gris-lilas quasi uniforme ; au survol elles s'eclaircissent (x 1.22)
    auto lit = [hv](juce::uint32 c)
    {
        const juce::Colour col(c);
        const float m = 1.0f + 0.22f * hv;
        return juce::Colour((juce::uint8) juce::jmin(255.0f, col.getRed() * m), (juce::uint8) juce::jmin(255.0f, col.getGreen() * m),
                            (juce::uint8) juce::jmin(255.0f, col.getBlue() * m));
    };
    juce::ColourGradient grad(lit(0xffe9dff0), 0.0f, oy, lit(0xffb3a6c3), 0.0f, r.getBottom(), false);
    grad.addColour(0.6, lit(0xffcfc3da));
    g.setGradientFill(grad);
    g.fillPath(fillP);

    // reflet lumineux qui balaie le mot (1.6 s par passage)
    if (hv > 0.0f)
    {
        const float ph = (float) std::fmod(now, 1.6) / 1.6f;
        const float x1 = -30.0f + 125.0f * ph;
        g.saveState();
        g.reduceClipRegion(fillP);
        juce::ColourGradient sheen(juce::Colours::white.withAlpha(0.0f), ox + x1 * u, oy,
                                   juce::Colours::white.withAlpha(0.0f), ox + (x1 + 22.0f) * u, oy + 6.0f * u, false);
        sheen.addColour(0.5, juce::Colours::white.withAlpha(0.75f * hv));
        g.setGradientFill(sheen);
        g.fillRect(r.expanded(4.0f));
        g.restoreState();
    }

    // deux braises ambre au centre des O
    const float cxs[2] = { 33.5f, 50.5f };
    for (float cxv : cxs)
    {
        const float cx = ox + cxv * u;
        const float cy = oy + 6.5f * u;
        const float hw = 1.4f * u;
        const float hh = 2.0f * u;

        softGlow(g, cx, cy, hh + 4.0f, juce::Colour(0x66ff8a3a));
        if (hv > 0.0f)
        {
            softGlow(g, cx, cy, hh + 10.0f, juce::Colour(0xffffb347).withAlpha(0.7f * hv));
            softGlow(g, cx, cy, hh + 3.0f, juce::Colour(0xfffff4dc).withAlpha(0.6f * hv));
        }

        juce::Path d;
        d.startNewSubPath(cx, cy - hh);
        d.lineTo(cx + hw, cy);
        d.lineTo(cx, cy + hh);
        d.lineTo(cx - hw, cy);
        d.closeSubPath();

        g.setColour(juce::Colour(0xffffb347));
        g.fillPath(d);
        g.setColour(juce::Colour(0xfffff4dc));
        g.strokePath(d, juce::PathStrokeType(0.2f * u));
    }

    // etiquette "COMMENT CA MARCHE" au-dessus du titre
    if (hv > 0.0f)
    {
        const float k = (float) getWidth() / 600.0f;
        const juce::Font f = bloomfonts::pixelFont(8.372f * k, false, 1.6744f * k);
        const juce::String label = juce::String::fromUTF8("COMMENT \xC3\x87""A MARCHE");
        const float tw = juce::GlyphArrangement::getStringWidth(f, label);
        const float w = 10.0f * k * 2.0f + 4.0f * k * 2.0f + 8.0f * k * 2.0f + tw;
        const float h = 19.0f * k;
        const float bottom = oy - 2.0f * u - 2.0f * k + 6.0f * k * (1.0f - hv);
        const juce::Rectangle<float> box(r.getCentreX() - w * 0.5f, bottom - h, w, h);

        g.saveState();
        g.beginTransparencyLayer(hv);
        g.setColour(juce::Colour(0xd90b0910));
        g.fillRect(box);
        g.setColour(juce::Colour(0x55ffb347));
        g.drawRect(box, 1.0f);
        g.setFont(f);
        const float asc = f.getAscent(), desc = f.getDescent();
        const float base = box.getY() + (h - (asc + desc)) * 0.5f + asc;
        const float tx = box.getCentreX() - tw * 0.5f;
        g.setColour(juce::Colour(0x40ff8a3a)); // lueur du texte
        for (auto o : { juce::Point<float>(-1, 0), { 1, 0 }, { 0, -1 }, { 0, 1 } })
            g.drawSingleLineText(label, juce::roundToInt(tx + o.x), juce::roundToInt(base + o.y));
        g.setColour(juce::Colour(0xffffd27a));
        g.drawSingleLineText(label, juce::roundToInt(tx), juce::roundToInt(base));
        for (float side : { -1.0f, 1.0f })
        {
            const float cx = box.getCentreX() + side * (tw * 0.5f + 8.0f * k + 2.0f * k);
            softGlow(g, cx, box.getCentreY(), 5.0f * k, juce::Colour(0x99ff8a3a));
            juce::Path d;
            const float hd = 2.83f * k;
            d.addQuadrilateral(cx, box.getCentreY() - hd, cx + hd, box.getCentreY(), cx, box.getCentreY() + hd, cx - hd, box.getCentreY());
            g.setColour(juce::Colour(0xffffb347));
            g.fillPath(d);
        }
        g.endTransparencyLayer();
        g.restoreState();
    }
}

void BloomView::paint(juce::Graphics& g)
{
    BLOOM_PERF_SCOPE("editeur : dessin");
    if (!perfFirstPaint)
    {
        perfFirstPaint = true;
        bloomperf::mark("editeur : premier dessin de l'interface");
    }
    // Le fond (ciel, ile, fleur...) est dessine par la scene 3D : ici on ne pose que le logo.
    drawLogo(g);
}

void BloomView::mouseMove(const juce::MouseEvent& e)
{
    const bool onLogo = logoBounds(getWidth()).expanded(8.0f).contains(e.position);
    setMouseCursor(swordCursor(onLogo));
    if (onLogo != logoHover)
    {
        logoHover = onLogo;
        logoHoverLast = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(30); // animation du titre
    }
}

void BloomView::mouseExit(const juce::MouseEvent&)
{
    if (logoHover)
    {
        logoHover = false;
        logoHoverLast = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(30);
    }
}

// zone a redessiner pour l'effet du titre (logo + halo + etiquette au-dessus)
juce::Rectangle<int> BloomView::logoFxArea() const
{
    return logoBounds(getWidth()).expanded(48.0f, 40.0f).toNearestInt().getIntersection(getLocalBounds());
}

void BloomView::mouseDown(const juce::MouseEvent& e)
{
    if (logoBounds(getWidth()).expanded(8.0f).contains(e.position))
        sheet->open();
}

void BloomView::paintOverChildren(juce::Graphics& g)
{
    BLOOM_PERF_SCOPE("editeur : dessin par-dessus");
    if (frameCache.isValid())
        g.drawImageAt(frameCache, 0, 0);
}

void BloomView::resized()
{
    const int W = getWidth();
    if (W <= 0 || getHeight() <= 0)
        return;

    rebuildFrameCache();

    // les arcs couvrent toute la fenetre (ils ne captent la souris que sur les arcs)
    arcs->setBounds(getLocalBounds());
    sheet->setBounds(getLocalBounds());

    // le menu en haut a droite, la chaine qui pend a gauche du logo
    // memes proportions que le prototype (en % de la largeur de la fenetre)
    const float Wf = (float) W;
    const int m = juce::roundToInt(juce::jlimit(14.0f, 24.0f, 0.03f * Wf)) + 10;
    menu->setBounds(W - BloomMenu::kWidth - m, m, BloomMenu::kWidth, BloomMenu::kHeight);
    demoTag->setBounds(m, m, BloomDemoTag::kWidth, BloomDemoTag::kHeight);
    const float pivotY = (juce::jlimit(14.0f, 24.0f, 0.03f * Wf) + 10.0f) * 0.5f;
    const float chainLen = 0.155f * (float) getHeight() - pivotY;
    chain->setGeometry(0.21f * Wf, pivotY, chainLen, 0.0165f * Wf, juce::jlimit(10.0f, 15.0f, 0.018f * Wf));
}


//==============================================================================
//  La fenetre du plugin
//==============================================================================
BLOOMAudioProcessorEditor::BLOOMAudioProcessorEditor(BLOOMAudioProcessor& p)
    : AudioProcessorEditor(&p)
{
    setOpaque(true);
    view = std::make_unique<BloomView>(p);
    addAndMakeVisible(*view);
    setResizable(false, false);
    setSize(view->getWidth(), view->getHeight());
}

BLOOMAudioProcessorEditor::~BLOOMAudioProcessorEditor()
{
    view = nullptr;
}

void BLOOMAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Tant que la 3D n'est pas a l'ecran, on montre l'interface complete dessinee sans OpenGL.
    // Ensuite la fenetre OpenGL recouvre tout : ce dessin n'est plus visible.
    if (view != nullptr && !view->isGlLive())
        view->paintFallback(g);
    else
        g.fillAll(juce::Colour::fromFloatRGBA(0.05f, 0.04f, 0.08f, 1.0f));
}

void BLOOMAudioProcessorEditor::resized()
{
    if (view != nullptr)
        view->setBounds(getLocalBounds());
}
