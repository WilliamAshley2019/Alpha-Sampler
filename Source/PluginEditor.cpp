// PluginEditor.cpp — Alpha Sampler editor.

#include "PluginEditor.h"

// ============================================================================
// pt::ui — visual language bodies (was Theme.cpp).
// ============================================================================
namespace pt::ui
{
// ----------------------------------------------------------------------------
// Typeface selection.
// ----------------------------------------------------------------------------
static juce::String pickTypeface()
{
    static const juce::String choice = []
    {
        const auto names = juce::Font::findAllTypefaceNames();
        for (auto* n : { "Segoe UI", "Inter", "Helvetica Neue", "Roboto", "DejaVu Sans" })
            if (names.contains(n)) return juce::String(n);
        return juce::Font::getDefaultSansSerifFontName();
    }();
    return choice;
}

juce::Font sans(float h, bool bold)
{
    return juce::Font(juce::FontOptions(pickTypeface(), h, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font caps(float h)
{
    return juce::Font(juce::FontOptions(pickTypeface(), h, juce::Font::bold).withKerningFactor(0.12f));
}

// ----------------------------------------------------------------------------
// Textures.
// ----------------------------------------------------------------------------
Textures::Textures()
{
    brushed = juce::Image(juce::Image::ARGB, 256, 64, true);
    juce::Graphics g(brushed);
    juce::Random r(0x70a11);
    for (int y = 0; y < 64; ++y)
    {
        for (int pass = 0; pass < 3; ++pass)
        {
            const float a = r.nextFloat() * 0.05f;
            const int x0 = r.nextInt(256), len = 40 + r.nextInt(216);
            g.setColour((r.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha(a));
            g.fillRect(x0, y, len, 1);
            if (x0 + len > 256) g.fillRect(0, y, x0 + len - 256, 1);
        }
    }
}

// ----------------------------------------------------------------------------
// Painting helpers.
// ----------------------------------------------------------------------------
void brushedMetal(juce::Graphics& g, juce::Rectangle<float> r, float corner, bool raised, const Textures& t)
{
    juce::Path p; p.addRoundedRectangle(r, corner);

    juce::ColourGradient grad(raised ? col::metalHi : col::metalLo, r.getX(), r.getY(),
                              raised ? col::metalLo : col::metalHi, r.getX(), r.getBottom(), false);
    grad.addColour(0.5, col::metalMid);
    g.setGradientFill(grad);
    g.fillPath(p);

    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(p);
        g.setTiledImageFill(t.brushed, 0, (int)r.getY() & 63, 0.9f);
        g.fillPath(p);
    }

    g.setColour(col::edgeLight.withAlpha(raised ? 0.55f : 0.2f));
    g.drawLine(r.getX() + corner * 0.6f, r.getY() + 0.5f,
               r.getRight() - corner * 0.6f, r.getY() + 0.5f, 1.f);
    g.setColour(col::edgeDark.withAlpha(0.9f));
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.f);
}

void glassWell(juce::Graphics& g, juce::Rectangle<float> r, float corner)
{
    juce::ColourGradient grad(col::glassHi, r.getX(), r.getY(), col::glass, r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, corner);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.5f);
    g.setColour(col::edgeLight.withAlpha(0.18f));
    g.drawLine(r.getX() + corner, r.getBottom() + 0.5f,
               r.getRight() - corner, r.getBottom() + 0.5f, 1.f);
}

void glow(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float spread, float alpha)
{
    const int steps = 5;
    for (int i = steps; i >= 1; --i)
    {
        const float k = (float)i / steps;
        g.setColour(c.withAlpha(alpha * (1.f - k) * 0.5f + 0.01f));
        g.fillRoundedRectangle(r.expanded(spread * k), r.getHeight() * 0.4f + spread * k);
    }
}

void engravedText(juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r,
                  juce::Justification j, const juce::Font& f, bool lit)
{
    g.setFont(f);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawText(s, r.translated(0.f, -1.f), j, false);
    g.setColour(juce::Colours::white.withAlpha(0.07f));
    g.drawText(s, r.translated(0.f, 1.f), j, false);
    g.setColour(lit ? col::amber : col::textDim);
    g.drawText(s, r, j, false);
}

void keyPlate(juce::Graphics& g, juce::Rectangle<float> r, bool down, bool on, const Textures& t)
{
    brushedMetal(g, r, 4.f, !down, t);
    if (down) { g.setColour(juce::Colours::black.withAlpha(0.25f)); g.fillRoundedRectangle(r, 4.f); }
    const float d = juce::jmin(5.f, r.getHeight() * 0.22f);
    const juce::Rectangle<float> led(r.getRight() - d - 5.f, r.getY() + 5.f, d, d);
    if (on) { glow(g, led, col::amber, 5.f, 0.5f); g.setColour(col::amber); }
    else    g.setColour(col::amberGhost.brighter(0.2f));
    g.fillEllipse(led);
}

// ----------------------------------------------------------------------------
// Seven-segment display.
// ----------------------------------------------------------------------------
static void addHBar(juce::Path& p, float cx, float cy, float len, float t)
{
    const float h = t * 0.5f, l = len * 0.5f;
    p.startNewSubPath(cx - l, cy);    p.lineTo(cx - l + h, cy - h);
    p.lineTo(cx + l - h, cy - h);     p.lineTo(cx + l, cy);
    p.lineTo(cx + l - h, cy + h);     p.lineTo(cx - l + h, cy + h);
    p.closeSubPath();
}
static void addVBar(juce::Path& p, float cx, float cy, float len, float t)
{
    const float h = t * 0.5f, l = len * 0.5f;
    p.startNewSubPath(cx, cy - l);    p.lineTo(cx + h, cy - l + h);
    p.lineTo(cx + h, cy + l - h);     p.lineTo(cx, cy + l);
    p.lineTo(cx - h, cy + l - h);     p.lineTo(cx - h, cy - l + h);
    p.closeSubPath();
}

float sevenSegmentWidthPerHeight(const juce::String& text)
{
    float w = 0.f;
    for (auto c : text) w += (c == '.' || c == ':' || c == ' ' ? 0.22f : 0.56f) + 0.16f;
    return juce::jmax(0.1f, w - 0.16f + 0.1f);
}

void sevenSegment(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                  juce::Colour on, juce::Colour off, bool ghost)
{
    static const int mask[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

    const float h = area.getHeight();
    const float dw = h * 0.56f, gap = h * 0.16f, t = h * 0.12f, sg = t * 0.18f;
    const float narrow = h * 0.22f;

    float total = 0.f;
    for (auto c : text) total += (c == '.' || c == ':' || c == ' ' ? narrow : dw) + gap;
    total -= gap;

    float x = area.getRight() - total;
    const float y = area.getY();
    const auto slant = juce::AffineTransform::shear(-0.1f, 0.f).translated(h * 0.05f, 0.f);

    auto drawDigit = [&](float dx, int m, bool ghostPass)
    {
        const float L = dw - t - sg * 2.f, V = (h - t * 3.f) * 0.5f - sg * 2.f;
        juce::Path p[7];
        addHBar(p[0], dx + dw * 0.5f, y + t * 0.5f, L, t);
        addVBar(p[1], dx + dw - t * 0.5f, y + t * 0.5f + (h - t) * 0.25f, V + t * 0.5f, t);
        addVBar(p[2], dx + dw - t * 0.5f, y + h - t * 0.5f - (h - t) * 0.25f, V + t * 0.5f, t);
        addHBar(p[3], dx + dw * 0.5f, y + h - t * 0.5f, L, t);
        addVBar(p[4], dx + t * 0.5f, y + h - t * 0.5f - (h - t) * 0.25f, V + t * 0.5f, t);
        addVBar(p[5], dx + t * 0.5f, y + t * 0.5f + (h - t) * 0.25f, V + t * 0.5f, t);
        addHBar(p[6], dx + dw * 0.5f, y + h * 0.5f, L, t);
        for (int s = 0; s < 7; ++s)
        {
            const bool lit = (m >> s) & 1;
            if (ghostPass == lit) continue;
            p[s].applyTransform(slant);
            if (lit) { g.setColour(on.withAlpha(0.16f)); g.strokePath(p[s], juce::PathStrokeType(t * 1.1f)); g.setColour(on); }
            else     { g.setColour(off); }
            g.fillPath(p[s]);
        }
    };

    for (auto c : text)
    {
        if (c >= '0' && c <= '9')
        {
            if (ghost) drawDigit(x, 0x7F, true);
            drawDigit(x, mask[c - '0'], false);
            x += dw + gap;
        }
        else if (c == '-')
        {
            if (ghost) drawDigit(x, 0x7F, true);
            drawDigit(x, 0x40, false);
            x += dw + gap;
        }
        else if (c == '.' || c == ':')
        {
            g.setColour(on);
            const float cx = x + narrow * 0.4f + h * 0.04f;
            if (c == '.') g.fillEllipse(cx - t * 0.6f, y + h - t * 1.2f, t * 1.2f, t * 1.2f);
            else { g.fillEllipse(cx - t * 0.55f, y + h * 0.3f, t * 1.1f, t * 1.1f);
                   g.fillEllipse(cx - t * 0.55f, y + h * 0.66f, t * 1.1f, t * 1.1f); }
            x += narrow + gap;
        }
        else if (c == ' ') x += narrow + gap;
        else { if (ghost) drawDigit(x, 0x7F, true); x += dw + gap; }
    }
}

// ----------------------------------------------------------------------------
// AlumLookAndFeel.
// ----------------------------------------------------------------------------
AlumLookAndFeel::AlumLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, col::bg);
    setColour(juce::Label::textColourId, col::text);
    setColour(juce::TextButton::textColourOffId, col::textDim);
    setColour(juce::TextButton::textColourOnId, col::amber);
    setColour(juce::ComboBox::textColourId, col::text);
    setColour(juce::PopupMenu::backgroundColourId, col::glass);
    setColour(juce::TooltipWindow::backgroundColourId, col::glass);
    setColour(juce::TooltipWindow::textColourId, col::text);
    setColour(juce::TooltipWindow::outlineColourId, col::edgeLight);
}

void AlumLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                                       float a0, float a1, juce::Slider& s)
{
    auto b = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(2.f);
    const float d = juce::jmin(b.getWidth(), b.getHeight());
    b = b.withSizeKeepingCentre(d, d);
    const auto c = b.getCentre();
    const float R = d * 0.5f;
    const float ang = a0 + pos * (a1 - a0);
    const bool enabled = s.isEnabled();

    {
        const float rr = R - 2.f, th = juce::jmax(2.f, d * 0.045f);
        juce::Path track; track.addCentredArc(c.x, c.y, rr, rr, 0.f, a0, a1, true);
        g.setColour(col::amberGhost.brighter(0.15f));
        g.strokePath(track, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val; val.addCentredArc(c.x, c.y, rr, rr, 0.f, a0, ang, true);
        if (enabled && pos > 0.f) {
            g.setColour(col::amber.withAlpha(0.18f));
            g.strokePath(val, juce::PathStrokeType(th * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(col::amber);
            g.strokePath(val, juce::PathStrokeType(th, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    const float rc = R * 0.74f;
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillEllipse(c.x - rc + 1.5f, c.y - rc + 3.f, rc * 2.f, rc * 2.f);

    juce::ColourGradient body(juce::Colour(0xff6a6d75), c.x - rc * 0.6f, c.y - rc * 0.8f,
                              juce::Colour(0xff1d1e22), c.x + rc * 0.7f, c.y + rc * 0.9f, true);
    body.addColour(0.45, juce::Colour(0xff3b3d44));
    g.setGradientFill(body);
    g.fillEllipse(c.x - rc, c.y - rc, rc * 2.f, rc * 2.f);

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    const int ticks = 48;
    for (int i = 0; i < ticks; ++i) {
        const float t = juce::MathConstants<float>::twoPi * (float)i / ticks;
        g.drawLine(c.x + std::sin(t) * rc * 0.9f, c.y - std::cos(t) * rc * 0.9f,
                   c.x + std::sin(t) * rc,       c.y - std::cos(t) * rc,       1.f);
    }

    const float rf = rc * 0.78f;
    juce::ColourGradient face(juce::Colour(0xff53565e), c.x - rf, c.y - rf,
                              juce::Colour(0xff2a2b30), c.x + rf, c.y + rf, false);
    g.setGradientFill(face);
    g.fillEllipse(c.x - rf, c.y - rf, rf * 2.f, rf * 2.f);
    for (int i = 1; i < 6; ++i) {
        const float r2 = rf * (float)i / 6.f;
        g.setColour(juce::Colours::white.withAlpha(i % 2 ? 0.045f : 0.02f));
        g.drawEllipse(c.x - r2, c.y - r2, r2 * 2.f, r2 * 2.f, 1.f);
    }
    g.setColour(juce::Colours::white.withAlpha(0.22f));
    juce::Path sheen; sheen.addCentredArc(c.x, c.y, rf * 0.92f, rf * 0.92f, 0.f, -2.5f, -0.9f, true);
    g.strokePath(sheen, juce::PathStrokeType(1.4f));

    g.setColour(enabled ? col::amber : col::textFaint);
    const float s1 = std::sin(ang), c1 = -std::cos(ang);
    g.drawLine(c.x + s1 * rf * 0.28f, c.y + c1 * rf * 0.28f,
               c.x + s1 * rf * 0.86f, c.y + c1 * rf * 0.86f,
               juce::jmax(1.5f, d * 0.035f));
}

void AlumLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced(1.f);
    keyPlate(g, r, down, b.getToggleState(), *tex);
    if (hi && !down) { g.setColour(juce::Colours::white.withAlpha(0.05f)); g.fillRoundedRectangle(r, 4.f); }
}

void AlumLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool down)
{
    g.setFont(caps(juce::jmin(11.5f, b.getHeight() * 0.42f)));
    g.setColour(b.getToggleState() ? col::text : col::textDim);
    g.drawText(b.getButtonText(), b.getLocalBounds().translated(0, down ? 1 : 0).reduced(4, 0),
               juce::Justification::centred, true);
}

void AlumLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced(1.f);
    keyPlate(g, r, down, b.getToggleState(), *tex);
    if (hi && !down) { g.setColour(juce::Colours::white.withAlpha(0.05f)); g.fillRoundedRectangle(r, 4.f); }
    g.setFont(caps(juce::jmin(11.f, b.getHeight() * 0.42f)));
    g.setColour(b.getToggleState() ? col::text : col::textDim);
    g.drawText(b.getButtonText(), b.getLocalBounds().reduced(6, 0).translated(0, down ? 1 : 0),
               juce::Justification::centredLeft, true);
}

void AlumLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox&)
{
    auto r = juce::Rectangle<float>(0.f, 0.f, (float)w, (float)h).reduced(1.f);
    brushedMetal(g, r, 4.f, !down, *tex);
    juce::Path tri;
    const float cx = (float)w - 12.f, cy = (float)h * 0.5f;
    tri.addTriangle(cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
    g.setColour(col::textDim);
    g.fillPath(tri);
}

void AlumLookAndFeel::positionComboBoxText(juce::ComboBox& c, juce::Label& l)
{
    l.setBounds(8, 1, c.getWidth() - 26, c.getHeight() - 2);
    l.setFont(getComboBoxFont(c));
}

void AlumLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    juce::ColourGradient grad(col::glassHi.withAlpha(0.97f), 0.f, 0.f,
                              col::glass.withAlpha(0.97f), 0.f, (float)h, false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(0.f, 0.f, (float)w, (float)h, 6.f);
    g.setColour(col::edgeLight.withAlpha(0.5f));
    g.drawRoundedRectangle(0.5f, 0.5f, (float)w - 1.f, (float)h - 1.f, 6.f, 1.f);
}

void AlumLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool sep, bool active,
                                         bool hi, bool ticked, bool sub, const juce::String& text,
                                         const juce::String&, const juce::Drawable*, const juce::Colour*)
{
    if (sep) {
        g.setColour(col::edgeLight.withAlpha(0.3f));
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }
    auto r = area.reduced(3, 1);
    if (hi && active) { g.setColour(col::amber.withAlpha(0.2f)); g.fillRoundedRectangle(r.toFloat(), 3.f); }
    g.setColour(!active ? col::textFaint : hi ? col::amber : col::text);
    g.setFont(getPopupMenuFont());
    g.drawText(text, r.withTrimmedLeft(22).withTrimmedRight(16), juce::Justification::centredLeft, true);
    if (ticked) {
        g.setColour(col::amber);
        g.fillEllipse((float)r.getX() + 8.f, (float)r.getCentreY() - 2.5f, 5.f, 5.f);
    }
    if (sub) {
        juce::Path p;
        p.addTriangle((float)r.getRight() - 12.f, (float)r.getCentreY() - 4.f,
                      (float)r.getRight() - 12.f, (float)r.getCentreY() + 4.f,
                      (float)r.getRight() - 6.f,  (float)r.getCentreY());
        g.fillPath(p);
    }
}

void AlumLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h,
                                     bool vertical, int thumbStart, int thumbSize, bool over, bool down)
{
    juce::ignoreUnused(x, y, w, h);
    juce::Rectangle<float> thumb = vertical
        ? juce::Rectangle<float>((float)x + 2.f, (float)thumbStart, (float)w - 4.f, (float)thumbSize)
        : juce::Rectangle<float>((float)thumbStart, (float)y + 2.f, (float)thumbSize, (float)h - 4.f);
    g.setColour(col::amber.withAlpha(down ? 0.8f : over ? 0.55f : 0.32f));
    g.fillRoundedRectangle(thumb, thumb.getWidth() * 0.5f);
}

void AlumLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                        juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, minPos, maxPos, style, s);
        return;
    }
    const float cy = (float)y + (float)h * 0.5f;
    juce::Rectangle<float> track((float)x, cy - 2.f, (float)w, 4.f);
    glassWell(g, track.expanded(0.f, 1.f), 2.f);
    g.setColour(col::amber.withAlpha(0.85f));
    g.fillRoundedRectangle(track.withRight(pos), 2.f);
    const juce::Rectangle<float> cap(pos - 7.f, cy - 9.f, 14.f, 18.f);
    brushedMetal(g, cap, 3.f, true, *tex);
    g.setColour(col::amber);
    g.fillRect(cap.withSizeKeepingCentre(2.f, 9.f));
}
} // namespace pt::ui

// ============================================================================
// as — editor components.
// ============================================================================
namespace as {

// ---- BlockMeter ------------------------------------------------------------
BlockMeter::BlockMeter() { setInterceptsMouseClicks (false, false); }

void BlockMeter::setValue (float v)
{
    v = juce::jlimit (0.0f, 1.0f, v);
    if (v == value) return;
    value = v;
    repaint();
}

void BlockMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    pt::ui::glassWell (g, b, 3.f);
    constexpr int n = 10;
    const float pad = 2.f;
    const float w = (b.getWidth() - pad * 2.f) / (float) n;
    const int lit = juce::jlimit (0, n, (int) std::round (value * (float) n));
    for (int i = 0; i < n; ++i)
    {
        const juce::Rectangle<float> seg (b.getX() + pad + (float) i * w + 0.5f, b.getY() + pad,
                                          w - 1.f, b.getHeight() - pad * 2.f);
        g.setColour (i < lit ? pt::ui::col::amber : pt::ui::col::amberGhost);
        g.fillRoundedRectangle (seg, 1.f);
    }
}

// ---- LevelMeter ------------------------------------------------------------
LevelMeter::LevelMeter (juce::Colour a) : accent (a) {}

void LevelMeter::setValue (float v)
{
    v = juce::jlimit (0.0f, 1.0f, v);
    if (std::abs (v - value) < 0.002f) return;
    value = v;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    pt::ui::glassWell (g, b, 3.f);
    const auto in = b.reduced (4.f, 6.f).withTrimmedBottom (12.f);
    g.setColour (pt::ui::col::edgeLight.withAlpha (0.35f));
    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine ((int) (in.getY() + in.getHeight() * (float) i / 4.f), in.getX(), in.getRight());

    // Show the level on a -60..0 dB scale.
    const float db = value > 1.0e-4f ? 20.0f * std::log10 (value) : -100.0f;
    const float shownFrac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    const float h = in.getHeight() * shownFrac;
    if (h > 0.f)
    {
        juce::ColourGradient grad (accent.brighter (0.15f), in.getX(), in.getBottom(),
                                   accent.darker (0.3f), in.getX(), in.getBottom() - h, false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (in.withTop (in.getBottom() - h), 2.f);
        g.setColour (pt::ui::col::text);
        g.fillRect (in.getX(), in.getBottom() - h - 1.f, in.getWidth(), 1.5f);
    }
    g.setFont (pt::ui::caps (9.f));
    g.setColour (pt::ui::col::textDim);
    g.drawText (value < 1.0e-4f ? juce::String ("-inf") : juce::String (db, 1),
                b.reduced (2.f, 2.f), juce::Justification::centredBottom, false);
}

// ---- ScopeView -------------------------------------------------------------
ScopeView::ScopeView()
{
    history.assign (300, 0.0f);
    peakHistory.assign (300, 0.0f);
}

void ScopeView::pushSample (float env, float peak)
{
    history[index] = juce::jlimit (0.0f, 1.0f, env);
    peakHistory[index] = juce::jlimit (0.0f, 1.0f, peak);
    index = (index + 1) % history.size();
    repaint();
}

void ScopeView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    pt::ui::glassWell (g, b, 4.f);

    g.setColour (pt::ui::col::edgeLight.withAlpha (0.12f));
    for (int i = 1; i < 10; ++i)
    {
        g.drawVerticalLine ((int) (b.getX() + b.getWidth() * (float) i / 10.f), b.getY(), b.getBottom());
        g.drawHorizontalLine ((int) (b.getY() + b.getHeight() * (float) i / 10.f), b.getX(), b.getRight());
    }

    juce::Path envPath, peakPath;
    for (size_t i = 0; i < history.size(); ++i)
    {
        const size_t j = (index + i) % history.size();
        const float x = b.getX() + b.getWidth() * (float) i / (float) history.size();
        const float ey = b.getBottom() - b.getHeight() * history[j];
        const float py = b.getBottom() - b.getHeight() * peakHistory[j];
        if (i == 0) { envPath.startNewSubPath (x, ey); peakPath.startNewSubPath (x, py); }
        else        { envPath.lineTo (x, ey);          peakPath.lineTo (x, py); }
    }
    g.setColour (pt::ui::col::cyan);
    g.strokePath (envPath, juce::PathStrokeType (1.8f));
    g.setColour (pt::ui::col::missed.brighter (0.2f));
    g.strokePath (peakPath, juce::PathStrokeType (1.0f));

    g.setFont (pt::ui::caps (9.f));
    g.setColour (pt::ui::col::cyan);
    g.drawText ("OUT", b.reduced (6.f).removeFromTop (12.f), juce::Justification::topRight, false);
    g.setColour (pt::ui::col::missed.brighter (0.2f));
    g.drawText ("PEAK", b.reduced (6.f).removeFromTop (12.f), juce::Justification::topLeft, false);
}

// ---- TweakWatcher -----------------------------------------------------------
TweakWatcher::TweakWatcher (juce::AudioProcessor& p) : proc (p)
{
    for (auto* prm : proc.getParameters()) prm->addListener (this);
}

TweakWatcher::~TweakWatcher()
{
    for (auto* prm : proc.getParameters()) prm->removeListener (this);
}

void TweakWatcher::parameterValueChanged (int i, float)
{
    if (held.load() > 0) { index.store (i); stamp.store (juce::Time::getMillisecondCounter()); }
}

void TweakWatcher::parameterGestureChanged (int i, bool starting)
{
    if (starting) { held.fetch_add (1); index.store (i); }
    else if (held.load() > 0) held.fetch_sub (1);
    stamp.store (juce::Time::getMillisecondCounter());
}

juce::AudioProcessorParameter* TweakWatcher::active() const
{
    const int i = index.load();
    const auto& params = proc.getParameters();
    if (i < 0 || i >= params.size()) return nullptr;
    const auto age = juce::Time::getMillisecondCounter() - stamp.load();
    const bool live = (held.load() > 0 && age < 15000u) || age < 1400u;   // linger briefly after release
    return live ? params[i] : nullptr;
}

// ---- WaveformView -----------------------------------------------------------
namespace
{
using GainFn = std::function<float (float)>;

// Draws min/max peaks of [f0,f1] (fractions of the sample; f1 < f0 reverses) into r.
void fillPeaks (juce::Graphics& g, const Peaks& pk, juce::Rectangle<float> r, float f0, float f1,
                float gain, juce::Colour c, float fillAlpha, const GainFn& gfn = nullptr,
                const std::function<juce::Colour (float)>& colourAt = nullptr)
{
    if (pk.empty() || r.getWidth() < 2.0f) return;
    const int n = (int) pk.lo.size();
    const int px = juce::jmax (2, (int) r.getWidth());
    const float mid = r.getCentreY(), amp = r.getHeight() * 0.5f * 0.94f;
    const float span = (f1 - f0) / (float) px;
    std::vector<juce::Point<float>> top, bot;
    top.reserve ((size_t) px);
    bot.reserve ((size_t) px);
    for (int i = 0; i < px; ++i)
    {
        const float s = (float) i / (float) (px - 1);
        const float t0 = f0 + (f1 - f0) * s;
        const float ta = juce::jmin (t0, t0 + span), tb = juce::jmax (t0, t0 + span);
        if (tb < 0.0f || ta > 1.0f) continue;
        const int ca = juce::jlimit (0, n - 1, (int) std::floor (ta * (float) n));
        const int cb = juce::jlimit (0, n - 1, (int) std::floor (tb * (float) n));
        float lo = pk.lo[(size_t) ca], hi = pk.hi[(size_t) ca];
        for (int k = ca + 1; k <= cb; ++k) { lo = juce::jmin (lo, pk.lo[(size_t) k]); hi = juce::jmax (hi, pk.hi[(size_t) k]); }
        const float gn = gain * (gfn ? gfn (s) : 1.0f);
        float a = hi * gn, b = lo * gn;
        if (a < b) std::swap (a, b);
        const float x = r.getX() + r.getWidth() * s;
        top.push_back ({ x, mid - a * amp });
        bot.push_back ({ x, mid - b * amp });
        if (colourAt)
        {
            g.setColour (colourAt (s));
            g.drawLine (x, mid - a * amp, x, mid - b * amp + 1.0f, 1.2f);
        }
    }
    if (colourAt) return;
    if (top.size() < 2) return;
    juce::Path p;
    p.startNewSubPath (top.front());
    for (size_t i = 1; i < top.size(); ++i) p.lineTo (top[i]);
    for (size_t i = bot.size(); i-- > 0;) p.lineTo (bot[i]);
    p.closeSubPath();
    g.setColour (c.withAlpha (fillAlpha));
    g.fillPath (p);
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (0.8f));
}

float filterMag (int type, float x, float q)
{
    const float d = juce::jmax (1.0e-4f, std::sqrt ((1.0f - x * x) * (1.0f - x * x) + (x / q) * (x / q)));
    switch (type)
    {
        case 2:  return (x / q) / d;
        case 3:  return x * x / d;
        case 4:  return std::abs (1.0f - x * x) / d;
        case 5:
        case 7:  { const float m = 1.0f / d; return m * m; }
        default: return 1.0f / d;
    }
}

float lfoShape (int shape, float phase)
{
    const float f = phase - std::floor (phase);
    if (shape == 1) return 4.0f * std::abs (f - 0.5f) - 1.0f;
    if (shape == 2) return f < 0.5f ? 1.0f : -1.0f;
    return std::sin (juce::MathConstants<float>::twoPi * f);
}

float tensionCurve (float t, float tension) { return std::pow (juce::jlimit (0.0f, 1.0f, t), std::exp (-tension * 1.5f)); }
} // namespace

WaveformView::WaveformView (juce::AudioProcessorValueTreeState& a) : apvts (a) {}

void WaveformView::setWave (std::shared_ptr<const WaveView> w) { wave = std::move (w); repaint(); }
void WaveformView::setDragHover (bool h) { if (hover != h) { hover = h; repaint(); } }
void WaveformView::setBusy (bool b) { if (busy != b) { busy = b; repaint(); } }

void WaveformView::setTweak (juce::AudioProcessorParameter* p)
{
    const float v = p != nullptr ? p->getValue() : -1.0f;
    if (p != tweak || v != tweakValue) { tweak = p; tweakValue = v; repaint(); }
}

float WaveformView::norm (const juce::String& id) const
{
    auto* q = apvts.getParameter (id);
    return q != nullptr ? q->getValue() : 0.0f;
}

float WaveformView::val (const juce::String& id) const
{
    auto* q = apvts.getParameter (id);
    return q != nullptr ? q->convertFrom0to1 (q->getValue()) : 0.0f;
}

static float textWidth (const juce::String& t)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (pt::ui::caps (9.f), t, 0.f, 0.f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

juce::Rectangle<float> WaveformView::areaRect() const
{
    auto inner = getLocalBounds().toFloat().reduced (10.f, 6.f);
    inner.removeFromTop (16.f);
    return inner.reduced (0.f, 2.f);
}

float WaveformView::toFrac (float x) const
{
    const auto a = areaRect();
    return z0 + (z1 - z0) * juce::jlimit (0.0f, 1.0f, (x - a.getX()) / a.getWidth());
}

float WaveformView::toX (float f, juce::Rectangle<float> a) const { return a.getX() + a.getWidth() * (f - z0) / (z1 - z0); }

int WaveformView::markerAt (float x) const
{
    if (wave == nullptr || wave->proc.empty() || tweak != nullptr || mode == Spectrum) return -1;
    const auto a = areaRect();
    int best = -1;
    float bd = 6.0f;
    auto test = [&] (int id, float f) { const float d = std::abs (toX (f, a) - x); if (d < bd) { bd = d; best = id; } };
    test (0, norm ("start_offset"));
    if (proc != nullptr)
        for (size_t i = 0; i < proc->slices().size(); ++i) test (100 + (int) i, proc->slices()[i]);
    if (wave->loop_a >= 0.0 && wave->loop_b > wave->loop_a && norm ("loop_use") > 0.5f)
    {
        test (1, (float) wave->loop_a);
        test (2, (float) wave->loop_b);
    }
    return best;
}

void WaveformView::setNorm (const juce::String& id, float v, bool begin, bool end)
{
    if (auto* p = apvts.getParameter (id))
    {
        if (begin) p->beginChangeGesture();
        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v));
        if (end) p->endChangeGesture();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    for (auto& [r, id] : hits)
        if (r.contains (e.position))
        {
            if (id < 4) { mode = id; repaint(); }
            else if (id == 10) { showRaw = ! showRaw; repaint(); }
            else if (id == 11) { grid = (grid + 1) % 3; repaint(); }
            else if (id == 20 || (id == 21 && selSlice >= 0)) dragOut = id;
            return;
        }
    drag = markerAt (e.position.x);
    panAnchor = z0;
    if (proc != nullptr && wave != nullptr && mode != Spectrum && tweak == nullptr)
    {
        if (e.mods.isPopupMenu())
        {
            const int m = drag;
            drag = -1;
            markerMenu (e.position.x, m);
            return;
        }
        if (drag < 0 && e.mods.isShiftDown())
        {
            auto v = proc->slices();
            v.push_back (toFrac (e.position.x));
            proc->setSlices (v);
            repaint();
            return;
        }
    }
    if (drag == 0) setNorm ("start_offset", toFrac (e.position.x), true, false);
    else if (drag == 1) setNorm ("loop_start", toFrac (e.position.x), true, false);
    else if (drag == 2) setNorm ("loop_end", toFrac (e.position.x), true, false);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3) moved = true;
    if (dragOut != 0)
    {
        if (e.getDistanceFromDragStart() > 6 && proc != nullptr)
        {
            const auto f = proc->dragFile (dragOut == 21 ? selSlice : -1);
            dragOut = 0;
            if (f.existsAsFile())
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
        }
        return;
    }
    if (drag == 0) setNorm ("start_offset", toFrac (e.position.x), false, false);
    else if (drag == 1) setNorm ("loop_start", juce::jmin (toFrac (e.position.x), norm ("loop_end") - 0.001f), false, false);
    else if (drag == 2) setNorm ("loop_end", juce::jmax (toFrac (e.position.x), norm ("loop_start") + 0.001f), false, false);
    else if (drag >= 100 && proc != nullptr)
    {
        auto v = proc->slices();
        const size_t i = (size_t) (drag - 100);
        if (i < v.size())
        {
            v[i] = juce::jlimit (i > 0 ? v[i - 1] + 0.001f : 0.0f, i + 1 < v.size() ? v[i + 1] - 0.001f : 0.9999f, toFrac (e.position.x));
            proc->setSlices (v);
            repaint();
        }
    }
    else if (z1 - z0 < 0.999f && mode != Spectrum)
    {
        const float span = z1 - z0;
        const float d = -(float) e.getDistanceFromDragStartX() / areaRect().getWidth() * span;
        z0 = juce::jlimit (0.0f, 1.0f - span, panAnchor + d);
        z1 = z0 + span;
        repaint();
    }
}

void WaveformView::markerMenu (float x, int marker)
{
    const float at = toFrac (x);
    juce::PopupMenu m;
    if (marker >= 100) m.addItem (1, "Remove marker " + juce::String (marker - 99));
    else m.addItem (2, "Place marker here");
    m.addItem (3, "Remove all markers", ! proc->slices().empty());
    m.showMenuAsync (juce::PopupMenu::Options(), [this, at, marker, safe = juce::Component::SafePointer<WaveformView> (this)] (int r)
    {
        if (safe == nullptr || proc == nullptr || r == 0) return;
        auto v = proc->slices();
        if (r == 1 && (size_t) (marker - 100) < v.size()) v.erase (v.begin() + (marker - 100));
        else if (r == 2) v.push_back (at);
        else if (r == 3) v.clear();
        proc->setSlices (v);
        selSlice = -1;
        repaint();
    });
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (dragOut == 0 && drag < 0 && ! moved && proc != nullptr && e.getDistanceFromDragStart() < 4 && ! e.mods.isPopupMenu() && ! e.mods.isShiftDown()
        && tweak == nullptr && mode != Spectrum)
    {
        const auto v = proc->slices();
        const float f = toFrac (e.position.x);
        int s = -1;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] <= f) s = (int) i;
        selSlice = s == selSlice ? -1 : s;
        repaint();
    }
    dragOut = 0;
    moved = false;
    if (drag == 0) setNorm ("start_offset", norm ("start_offset"), false, true);
    else if (drag == 1) setNorm ("loop_start", norm ("loop_start"), false, true);
    else if (drag == 2) setNorm ("loop_end", norm ("loop_end"), false, true);
    drag = -1;
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (markerAt (e.position.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int m = markerAt (e.position.x);
    if (m >= 100 && proc != nullptr)
    {
        auto v = proc->slices();
        v.erase (v.begin() + (m - 100));
        proc->setSlices (v);
        selSlice = -1;
        repaint();
        return;
    }
    if (m < 0) { z0 = 0.0f; z1 = 1.0f; repaint(); }
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (mode == Spectrum) return;
    const float at = toFrac (e.position.x), span = z1 - z0;
    const float ns = juce::jlimit (0.002f, 1.0f, span * std::pow (0.8f, w.deltaY * 6.0f));
    const float rel = (at - z0) / juce::jmax (1.0e-6f, span);
    z0 = juce::jlimit (0.0f, 1.0f - ns, at - rel * ns);
    z1 = z0 + ns;
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    pt::ui::glassWell (g, b, 4.f);

    auto inner = b.reduced (10.f, 6.f);
    auto header = inner.removeFromTop (16.f);
    const auto area = areaRect();

    if (grid == 0)
    {
        g.setColour (pt::ui::col::edgeLight.withAlpha (0.10f));
        for (int i = 1; i < 10; ++i)
            g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) i / 10.f), area.getY(), area.getBottom());
    }
    g.setColour (pt::ui::col::edgeLight.withAlpha (0.10f));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    if (tweak != nullptr) paintTweak (g, area, header);
    else                  paintIdle (g, area, header);

    if (hover)
    {
        g.setColour (pt::ui::col::amber.withAlpha (0.16f));
        g.fillRoundedRectangle (b.reduced (2.f), 4.f);
        g.setColour (pt::ui::col::amber);
        g.drawRoundedRectangle (b.reduced (3.f), 4.f, 2.0f);
        g.setFont (pt::ui::caps (18.f));
        g.drawText ("RELEASE TO LOAD SAMPLE", b, juce::Justification::centred, false);
    }
}

void WaveformView::paintGrid (juce::Graphics& g, juce::Rectangle<float> area)
{
    using namespace pt::ui;
    if (grid == 0 || wave == nullptr || wave->rate <= 0.0) return;
    const double total = wave->proc_frames / wave->rate, t0 = z0 * total, t1 = z1 * total;
    double step;
    if (grid == 1)
    {
        step = 0.01;
        for (double s : { 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0 })
        {
            step = s;
            if (s / (t1 - t0) * area.getWidth() >= 60.0) break;
        }
    }
    else
    {
        if (tempo <= 0.0) return;
        step = 60.0 / tempo;
        while (step / (t1 - t0) * area.getWidth() < 8.0) step *= 2.0;
    }
    g.setFont (caps (9.f));
    for (int i = (int) std::ceil (t0 / step); i * step <= t1; ++i)
    {
        const float x = area.getX() + area.getWidth() * (float) ((i * step - t0) / (t1 - t0));
        const bool bar = grid == 2 && i % 4 == 0;
        g.setColour (col::edgeLight.withAlpha (bar ? 0.35f : 0.14f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        if (grid == 1 || bar)
        {
            g.setColour (col::textFaint);
            g.drawText (grid == 1 ? juce::String (i * step, step < 0.1 ? 3 : 2) + "s" : juce::String (i / 4 + 1),
                        juce::Rectangle<float> (x + 2.f, area.getBottom() - 12.f, 50.f, 12.f), juce::Justification::centredLeft, false);
        }
    }
}

void WaveformView::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area)
{
    using namespace pt::ui;
    const auto& an = wave->ana;
    const auto plot = area.reduced (6.f, 6.f);
    const double fmin = 30.0, fmax = juce::jmin (20000.0, wave->rate * 0.5);
    auto fx = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / fmin) / std::log (fmax / fmin)); };
    g.setFont (caps (9.f));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        if (f > fmax) break;
        g.setColour (col::edgeLight.withAlpha (0.18f));
        g.drawVerticalLine ((int) fx (f), plot.getY(), plot.getBottom());
        g.setColour (col::textFaint);
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (fx (f) + 2.f, plot.getBottom() - 12.f, 40.f, 12.f), juce::Justification::centredLeft, false);
    }
    juce::Path p;
    for (int i = 0; i < an->bins; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (an->bins - 1);
        const float y = plot.getBottom() - plot.getHeight() * an->avg[(size_t) i];
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    juce::Path fill (p);
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    g.setColour (col::cyan.withAlpha (0.18f));
    g.fillPath (fill);
    g.setColour (col::cyan);
    g.strokePath (p, juce::PathStrokeType (2.0f));
}

void WaveformView::paintIdle (juce::Graphics& g, juce::Rectangle<float> area, juce::Rectangle<float> header)
{
    using namespace pt::ui;
    hits.clear();
    const bool has = wave != nullptr && ! wave->proc.empty();

    g.setFont (caps (9.f));
    auto tabs = header;
    for (const char* nm : { "WAVE", "COLOUR", "SONO", "SPECTRUM" })
    {
        const int id = (int) hits.size();
        auto r = tabs.removeFromLeft (textWidth (nm) + 14.f);
        g.setColour (id == mode ? col::amber : col::textDim);
        g.drawText (nm, r, juce::Justification::centred, false);
        if (id == mode) g.fillRect (r.getX() + 4.f, r.getBottom() - 1.f, r.getWidth() - 8.f, 1.f);
        hits.push_back ({ r, id });
    }
    tabs.removeFromLeft (10.f);
    auto toggle = [&] (const juce::String& txt, int id, bool on)
    {
        auto r = tabs.removeFromLeft (textWidth (txt) + 14.f);
        g.setColour (on ? col::cyan : col::textFaint);
        g.drawText (txt, r, juce::Justification::centred, false);
        hits.push_back ({ r, id });
    };
    toggle ("RAW", 10, showRaw);
    toggle (grid == 0 ? "GRID" : grid == 1 ? "GRID: SEC" : "GRID: BEAT", 11, grid != 0);

    if (! has)
    {
        g.setFont (caps (16.f));
        g.setColour (col::textFaint);
        g.drawText (busy ? "LOADING..." : "DROP A SAMPLE HERE", area.withTrimmedBottom (14.f), juce::Justification::centred, false);
        if (! busy)
        {
            g.setFont (sans (11.f));
            g.drawText ("WAV  AIFF  FLAC  OGG  MP3   -   or use LOAD on the SAMPLE tab", area.withTrimmedTop (area.getHeight() * 0.62f),
                        juce::Justification::centredTop, false);
        }
        return;
    }

    const double secs = wave->rate > 0.0 ? wave->proc_frames / wave->rate : 0.0;
    float pk = 0.0f;
    for (size_t i = 0; i < wave->proc.lo.size(); ++i) pk = juce::jmax (pk, std::abs (wave->proc.lo[i]), std::abs (wave->proc.hi[i]));
    juce::String info = juce::String (secs, 2) + " s  " + juce::String (wave->rate, 0) + " Hz  PK " + juce::String (20.0f * std::log10 (juce::jmax (pk, 1.0e-6f)), 1);
    if (wave->ana != nullptr)
        info << "  RMS " << juce::String (wave->ana->rms_db, 1) << "  DC " << juce::String (wave->ana->dc * 100.0f, 2) << "%";
    g.setColour (col::textDim);
    g.drawText (info + " dB", header.withTrimmedLeft (header.getWidth() * 0.5f), juce::Justification::centredRight, false);

    if (mode == Spectrum)
    {
        if (wave->ana != nullptr) paintSpectrum (g, area);
        else { g.setColour (col::textFaint); g.drawText ("NO ANALYSIS", area, juce::Justification::centred, false); }
        return;
    }
    if (mode == Sono && wave->ana != nullptr)
    {
        const auto& an = *wave->ana;
        if (sonoFor != wave->ana.get())
        {
            sonoFor = wave->ana.get();
            sonoImg = juce::Image (juce::Image::ARGB, an.cols, an.bins, false);
            for (int c = 0; c < an.cols; ++c)
                for (int b = 0; b < an.bins; ++b)
                {
                    const float v = an.img[(size_t) (c * an.bins + b)] / 255.0f;
                    const auto col = v < 0.5f ? juce::Colour::fromFloatRGBA (0.0f, 0.0f, 0.0f, 1.0f).interpolatedWith (pt::ui::col::amberDim, v * 2.0f)
                                              : pt::ui::col::amberDim.interpolatedWith (pt::ui::col::good.brighter (0.4f), (v - 0.5f) * 2.0f);
                    sonoImg.setPixelAt (c, an.bins - 1 - b, col);
                }
        }
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        const float sx = z0 * (float) an.cols, sw = (z1 - z0) * (float) an.cols;
        g.drawImage (sonoImg, (int) area.getX(), (int) area.getY(), (int) area.getWidth(), (int) area.getHeight(),
                     (int) sx, 0, juce::jmax (1, (int) sw), an.bins);
        g.restoreState();
        g.setFont (caps (9.f));
        g.setColour (col::text.withAlpha (0.7f));
        const double fmax = juce::jmin (20000.0, wave->rate * 0.5);
        for (double f : { 100.0, 1000.0, 10000.0 })
        {
            if (f > fmax) break;
            const float y = area.getBottom() - area.getHeight() * (float) (std::log (f / 30.0) / std::log (fmax / 30.0));
            g.drawHorizontalLine ((int) y, area.getX(), area.getX() + 6.f);
            g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (area.getX() + 8.f, y - 6.f, 30.f, 12.f), juce::Justification::centredLeft, false);
        }
    }
    else
    {
        if (showRaw)
            fillPeaks (g, wave->raw, area, z0, z1, 1.0f, col::textDim, 0.18f);
        if (mode == Colour && wave->ana != nullptr)
        {
            fillPeaks (g, wave->proc, area, z0, z1, 1.0f, col::cyan.withAlpha (0.0f), 0.0f,
                       nullptr, [&] (float s)
                       {
                           const float f = z0 + (z1 - z0) * s;
                           const int c = juce::jlimit (0, wave->ana->cols - 1, (int) (f * (float) wave->ana->cols));
                           const float t = wave->ana->bright[(size_t) c];
                           return juce::Colour::fromHSV (0.02f + 0.52f * t, 0.75f, 1.0f, 1.0f);
                       });
        }
        else
            fillPeaks (g, wave->proc, area, z0, z1, 1.0f, col::cyan, 0.35f);
    }
    paintGrid (g, area);

    if (wave->loop_a >= 0.0 && wave->loop_b > wave->loop_a && norm ("loop_use") > 0.5f)
    {
        const float xa = toX ((float) wave->loop_a, area), xb = toX ((float) wave->loop_b, area);
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        g.setColour (col::amber.withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (xa, area.getY(), xb - xa, area.getHeight()));
        g.setColour (col::amber.withAlpha (0.8f));
        g.drawVerticalLine ((int) xa, area.getY(), area.getBottom());
        g.drawVerticalLine ((int) xb, area.getY(), area.getBottom());
        g.restoreState();
    }
    const float so = norm ("start_offset");
    if (so > 0.0005f || drag == 0)
    {
        const float x = toX (so, area);
        g.setColour (col::good);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setFont (caps (9.f));
        g.drawText ("START", juce::Rectangle<float> (x + 3.f, area.getY(), 60.f, 12.f), juce::Justification::centredLeft, false);
    }
    if (proc != nullptr)
    {
        const auto sl = proc->slices();
        const bool on = norm ("slice_on") > 0.5f;
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        if (selSlice >= 0 && (size_t) selSlice < sl.size())
        {
            const float xa = toX (sl[(size_t) selSlice], area);
            const float xb = (size_t) selSlice + 1 < sl.size() ? toX (sl[(size_t) selSlice + 1], area) : toX (1.0f, area);
            g.setColour (col::amber.withAlpha (0.14f));
            g.fillRect (juce::Rectangle<float> (xa, area.getY(), xb - xa, area.getHeight()));
        }
        g.setFont (caps (9.f));
        for (size_t i = 0; i < sl.size(); ++i)
        {
            const float x = toX (sl[i], area);
            g.setColour (col::offDyn.withAlpha (on ? 0.95f : 0.5f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.fillRect (x - 3.f, area.getY(), 7.f, 3.f);
            g.drawText (juce::String ((int) i + 1) + (on ? juce::String (" ") + juce::String (laws::note_name (proc->sliceBase() + (int) i)) : juce::String()),
                        juce::Rectangle<float> (x + 3.f, area.getBottom() - 40.f, 60.f, 12.f), juce::Justification::centredLeft, false);
        }
        g.restoreState();
    }
    if (z1 - z0 < 0.999f)
    {
        g.setFont (caps (9.f));
        g.setColour (col::textDim);
        g.drawText (juce::String (1.0f / (z1 - z0), 1) + "x  (double-click resets)", area.reduced (4.f), juce::Justification::topRight, false);
    }
    if (proc != nullptr)
    {
        auto row = area.removeFromBottom (18.f).reduced (2.f, 1.f);
        auto chip = [&] (const juce::String& txt, int id, bool enabled)
        {
            auto r = row.removeFromRight (textWidth (txt) + 16.f);
            row.removeFromRight (4.f);
            g.setColour ((enabled ? col::amber : col::textFaint).withAlpha (0.18f));
            g.fillRoundedRectangle (r, 3.f);
            g.setColour (enabled ? col::amber : col::textFaint);
            g.drawRoundedRectangle (r, 3.f, 1.f);
            g.drawText (txt, r, juce::Justification::centred, false);
            if (enabled) hits.push_back ({ r, id });
        };
        g.setFont (caps (9.f));
        chip ("DRAG SAMPLE", 20, true);
        chip (selSlice >= 0 ? "DRAG SLICE " + juce::String (selSlice + 1) : juce::String ("CLICK A SLICE TO SELECT"), 21, selSlice >= 0);
    }
}

void WaveformView::paintTweak (juce::Graphics& g, juce::Rectangle<float> area, juce::Rectangle<float> header)
{
    using namespace pt::ui;
    auto* rp = dynamic_cast<juce::RangedAudioParameter*> (tweak);
    if (rp == nullptr) return;

    const juce::String id = rp->paramID;
    const bool have = wave != nullptr && ! wave->raw.empty();
    const Peaks empty;
    const Peaks& raw = have ? wave->raw : empty;
    const auto amber = col::amber;
    const float W = area.getWidth(), H = area.getHeight();
    auto X = [&] (float f) { return area.getX() + W * f; };
    auto yOf = [&] (float v) { return area.getCentreY() - v * H * 0.5f * 0.94f; };

    g.setFont (caps (10.f));
    g.setColour (amber);
    g.drawText (rp->getName (48).toUpperCase(), header, juce::Justification::centredLeft, false);
    g.setFont (caps (13.f));
    g.setColour (col::text);
    g.drawText (tweak->getCurrentValueAsText(), header, juce::Justification::centredRight, false);

    auto dim = [&] { fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f, col::textDim, 0.22f); };
    auto marker = [&] (float f, juce::Colour c, const juce::String& label)
    {
        g.setColour (c);
        g.drawVerticalLine ((int) X (f), area.getY(), area.getBottom());
        g.setFont (caps (9.f));
        g.drawText (label, juce::Rectangle<float> (X (f) + 3.f, area.getY(), 70.f, 12.f), juce::Justification::centredLeft, false);
    };
    auto label = [&] (const juce::String& t)
    {
        g.setFont (caps (9.f));
        g.setColour (col::textDim);
        g.drawText (t, area.reduced (4.f), juce::Justification::topLeft, false);
    };

    // ---- envelopes and LFOs (ids look like "envpitch_att", "envpan_lfo_speed") ----
    if (id.startsWith ("env") && id.containsChar ('_'))
    {
        dim();
        const auto pre = id.upToFirstOccurrenceOf ("_", false, false);
        const auto suf = id.fromFirstOccurrenceOf ("_", false, false);
        const float pad = 14.f;
        const auto plot = area.reduced (pad, pad);

        if (suf.startsWith ("lfo_"))
        {
            const int shape = (int) std::lround (val (pre + "_lfo_shape"));
            const float cycles = 2.0f + norm (pre + "_lfo_speed") * 12.0f;
            const float d = norm (pre + "_lfo_delay") * 0.4f, a = norm (pre + "_lfo_att") * 0.4f;
            const float amt = suf == "lfo_amt" ? std::abs (val (pre + "_lfo_amt")) : juce::jmax (0.25f, std::abs (val (pre + "_lfo_amt")));
            juce::Path p;
            const int steps = juce::jmax (64, (int) plot.getWidth());
            for (int i = 0; i <= steps; ++i)
            {
                const float t = (float) i / (float) steps;
                const float e = t < d ? 0.0f : (a <= 0.0f ? 1.0f : juce::jmin (1.0f, (t - d) / a));
                const float y = plot.getCentreY() - lfoShape (shape, cycles * t) * e * amt * plot.getHeight() * 0.5f;
                if (i == 0) p.startNewSubPath (plot.getX() + plot.getWidth() * t, y);
                else        p.lineTo (plot.getX() + plot.getWidth() * t, y);
            }
            g.setColour (col::textFaint);
            g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
            g.setColour (amber);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            label ("LFO  -  " + pre.substring (3).toUpperCase());
            return;
        }

        // DAHDSR
        const char* names[] = { "delay", "att", "hold", "dec", "sus", "rel" };
        float w[6];
        float total = 0.0f;
        for (int i = 0; i < 6; ++i) { w[i] = i == 4 ? 0.18f : 0.03f + 0.22f * norm (pre + "_" + names[i]); total += w[i]; }
        const float sus = norm (pre + "_sus");
        const float scale = suf == "amt" ? std::abs (val (pre + "_amt")) : 1.0f;
        int activeSeg = -1;
        if (suf == "delay") activeSeg = 0;
        else if (suf == "att" || suf == "att_t") activeSeg = 1;
        else if (suf == "hold") activeSeg = 2;
        else if (suf == "dec" || suf == "dec_t") activeSeg = 3;
        else if (suf == "sus") activeSeg = 4;
        else if (suf == "rel" || suf == "rel_t") activeSeg = 5;

        const float bottom = plot.getBottom(), hgt = plot.getHeight() * scale;
        float x0 = plot.getX(), level = 0.0f;
        for (int s = 0; s < 6; ++s)
        {
            const float x1 = x0 + plot.getWidth() * w[s] / total;
            juce::Path p;
            const int steps = 24;
            float endLevel = level;
            for (int i = 0; i <= steps; ++i)
            {
                const float t = (float) i / (float) steps;
                float y = level;
                switch (s)
                {
                    case 0: y = 0.0f; break;
                    case 1: y = tensionCurve (t, val (pre + "_att_t")); break;
                    case 2: y = 1.0f; break;
                    case 3: y = 1.0f - (1.0f - sus) * tensionCurve (t, val (pre + "_dec_t")); break;
                    case 4: y = sus; break;
                    default: y = sus * (1.0f - tensionCurve (t, val (pre + "_rel_t"))); break;
                }
                endLevel = y;
                const float px = x0 + (x1 - x0) * t, py = bottom - y * hgt;
                if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
            }
            g.setColour (s == activeSeg ? col::text : amber.withAlpha (activeSeg < 0 ? 1.0f : 0.55f));
            g.strokePath (p, juce::PathStrokeType (s == activeSeg ? 3.0f : 2.0f));
            g.setFont (caps (9.f));
            g.setColour (s == activeSeg ? col::text : col::textDim);
            static const char* tags[] = { "DLY", "ATK", "HLD", "DEC", "SUS", "REL" };
            g.drawText (tags[s], juce::Rectangle<float> (x0, bottom + 1.f, x1 - x0, 12.f), juce::Justification::centred, false);
            level = endLevel;
            x0 = x1;
        }
        g.setColour (col::textFaint);
        g.drawHorizontalLine ((int) bottom, plot.getX(), plot.getRight());
        label ("ENVELOPE  -  " + pre.substring (3).toUpperCase());
        return;
    }

    // ---- filters: frequency response ----
    const bool precomputed = id == "fx_cut" || id == "fx_res";
    const bool live = id == "f_cut" || id == "f_res" || id == "f_type";
    if (precomputed || live || id == "fx_eq")
    {
        const auto plot = area.reduced (10.f, 8.f);
        auto fx = [&] (float f) { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
        auto dbY = [&] (float db) { return plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 66.0f); };
        g.setFont (caps (9.f));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            g.setColour (col::edgeLight.withAlpha (0.2f));
            g.drawVerticalLine ((int) fx (f), plot.getY(), plot.getBottom());
            g.setColour (col::textFaint);
            g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + " k" : juce::String ((int) f),
                        juce::Rectangle<float> (fx (f) + 2.f, plot.getBottom() - 12.f, 40.f, 12.f), juce::Justification::centredLeft, false);
        }
        g.setColour (col::edgeLight.withAlpha (0.3f));
        g.drawHorizontalLine ((int) dbY (0.0f), plot.getX(), plot.getRight());

        const int type = precomputed || id == "fx_eq" ? 1 : (int) std::lround (val ("f_type"));
        const float fc = 20.0f * std::pow (1000.0f, norm (precomputed ? "fx_cut" : "f_cut"));
        const float q = 0.6f + norm (precomputed ? "fx_res" : "f_res") * 8.0f;
        const float eq = val ("fx_eq");
        const int steps = juce::jmax (64, (int) plot.getWidth());
        juce::Path p;
        for (int i = 0; i <= steps; ++i)
        {
            const float f = 20.0f * std::pow (1000.0f, (float) i / (float) steps);
            float db;
            if (id == "fx_eq") db = eq * 12.0f * std::tanh (std::log2 (f / 1000.0f) * 0.7f);
            else               db = 20.0f * std::log10 (juce::jmax (1.0e-4f, filterMag (type, f / fc, q)));
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) steps;
            if (i == 0) p.startNewSubPath (x, dbY (db)); else p.lineTo (x, dbY (db));
        }
        juce::Path fill (p);
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (amber.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (amber);
        g.strokePath (p, juce::PathStrokeType (2.2f));
        if (id != "fx_eq")
        {
            g.setColour (col::cyan.withAlpha (0.8f));
            g.drawVerticalLine ((int) fx (fc), plot.getY(), plot.getBottom());
        }
        return;
    }

    // ---- keyboard ranges: root / zone ----
    if (id == "root" || id == "zone_lo" || id == "zone_hi")
    {
        const float lo = val ("zone_lo"), hi = val ("zone_hi"), rt = val ("root");
        const auto bar = area.withSizeKeepingCentre (area.getWidth() - 20.f, 26.f);
        auto kx = [&] (float key) { return bar.getX() + bar.getWidth() * key / 131.0f; };
        g.setColour (col::edgeLight.withAlpha (0.25f));
        g.fillRoundedRectangle (bar, 3.f);
        g.setColour (amber.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (kx (lo), bar.getY(), juce::jmax (2.0f, kx (hi) - kx (lo)), bar.getHeight()));
        g.setColour (col::cyan);
        g.fillRect (juce::Rectangle<float> (kx (rt) - 1.5f, bar.getY() - 6.f, 3.f, bar.getHeight() + 12.f));
        label ("ZONE  (amber)   ROOT  (cyan)");
        return;
    }

    // ---- everything below draws over the source waveform ----
    if (id == "fx_start" || id == "fx_length")
    {
        const float s = norm ("fx_start"), e = norm ("fx_length");
        const float a = juce::jmin (s, e), z = juce::jmax (s, e);
        dim();
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<float> (X (a), area.getY(), X (z) - X (a), H).toNearestInt());
        fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f, amber, 0.35f);
        g.restoreState();
        marker (s, amber, "START " + juce::String (s * 100.0f, 1) + "%");
        g.setColour (amber);
        g.drawVerticalLine ((int) X (e), area.getY(), area.getBottom());
        g.setFont (caps (9.f));
        g.drawText ("END " + juce::String (e * 100.0f, 1) + "%", juce::Rectangle<float> (X (e) - 73.f, area.getY(), 70.f, 12.f), juce::Justification::centredRight, false);
        return;
    }
    if (id == "start_offset")
    {
        dim();
        const float so = norm ("start_offset");
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (area.getX(), area.getY(), W * so, H));
        marker (so, col::good, "PLAY FROM " + juce::String (so * 100.0f, 1) + "%");
        return;
    }
    if (id.startsWith ("loop_") || id == "fx_loop_follows" || id == "fx_crossfade")
    {
        dim();
        const float a = juce::jmin (norm ("loop_start"), norm ("loop_end")), b = juce::jmax (norm ("loop_start"), norm ("loop_end"));
        g.setColour (col::cyan.withAlpha (0.14f));
        g.fillRect (juce::Rectangle<float> (X (a), area.getY(), X (b) - X (a), H));
        marker (a, col::cyan, "LOOP A");
        g.setColour (col::cyan);
        g.drawVerticalLine ((int) X (b), area.getY(), area.getBottom());
        g.setFont (caps (9.f));
        g.drawText ("LOOP B", juce::Rectangle<float> (X (b) - 53.f, area.getY(), 50.f, 12.f), juce::Justification::centredRight, false);
        if (id == "loop_pingpong")
        {
            g.setFont (caps (22.f));
            g.setColour (norm ("loop_pingpong") > 0.5f ? amber : col::textFaint);
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x87\x84"), juce::Rectangle<float> (X (a), area.getY(), X (b) - X (a), H), juce::Justification::centred, false);
        }
        if (id == "fx_crossfade")
        {
            const float w = norm ("fx_crossfade") * 0.25f * W;
            const float x1 = X (b), x0 = x1 - w;
            g.setColour (amber);
            g.drawLine (x0, yOf (0.7f), x1, yOf (-0.7f), 2.f);
            g.setColour (col::cyan);
            g.drawLine (x0, yOf (-0.7f), x1, yOf (0.7f), 2.f);
        }
        return;
    }
    if (id == "fx_in" || id == "fx_out")
    {
        dim();
        const float fi = norm ("fx_in") * 0.5f, fo = norm ("fx_out") * 0.5f;
        g.setColour (amber);
        juce::Path env;
        env.startNewSubPath (X (0.0f), yOf (0.0f));
        env.lineTo (X (fi), yOf (0.94f));
        env.lineTo (X (1.0f - fo), yOf (0.94f));
        env.lineTo (X (1.0f), yOf (0.0f));
        g.strokePath (env, juce::PathStrokeType (2.f));
        juce::Path envLow;
        envLow.startNewSubPath (X (0.0f), yOf (0.0f));
        envLow.lineTo (X (fi), yOf (-0.94f));
        envLow.lineTo (X (1.0f - fo), yOf (-0.94f));
        envLow.lineTo (X (1.0f), yOf (0.0f));
        g.strokePath (envLow, juce::PathStrokeType (2.f));
        fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f, amber, 0.3f,
                   [fi, fo] (float t) { return juce::jmin (fi > 0.0f ? t / fi : 1.0f, fo > 0.0f ? (1.0f - t) / fo : 1.0f, 1.0f); });
        return;
    }
    if (id == "fx_boost" || id == "fx_normalize" || id == "fx_clip" || id == "fx_trim" || id == "fx_dc")
    {
        dim();
        float peak = 0.0f, mean = 0.0f;
        for (size_t i = 0; i < raw.lo.size(); ++i) { peak = juce::jmax (peak, std::abs (raw.lo[i]), std::abs (raw.hi[i])); mean += 0.5f * (raw.lo[i] + raw.hi[i]); }
        if (! raw.lo.empty()) mean /= (float) raw.lo.size();
        g.setColour (col::textFaint);
        g.drawHorizontalLine ((int) yOf (1.0f), area.getX(), area.getRight());
        g.drawHorizontalLine ((int) yOf (-1.0f), area.getX(), area.getRight());
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        if (id == "fx_boost")
            fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f + norm ("fx_boost") * 3.0f, amber, 0.3f);
        else if (id == "fx_normalize")
            fillPeaks (g, raw, area, 0.0f, 1.0f, peak > 1.0e-4f ? 1.0f / peak : 1.0f, amber, 0.3f);
        else if (id == "fx_clip")
        {
            g.reduceClipRegion (juce::Rectangle<float> (area.getX(), yOf (0.7f), W, yOf (-0.7f) - yOf (0.7f)).toNearestInt());
            fillPeaks (g, raw, area, 0.0f, 1.0f, 1.6f, amber, 0.3f);
            g.setColour (amber);
            g.drawHorizontalLine ((int) yOf (0.7f), area.getX(), area.getRight());
            g.drawHorizontalLine ((int) yOf (-0.7f), area.getX(), area.getRight());
        }
        else if (id == "fx_trim")
        {
            const float thr = norm ("fx_trim") * 0.5f;
            g.setColour (amber.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (area.getX(), yOf (thr), W, yOf (-thr) - yOf (thr)));
            g.setColour (amber);
            g.drawHorizontalLine ((int) yOf (thr), area.getX(), area.getRight());
            g.drawHorizontalLine ((int) yOf (-thr), area.getX(), area.getRight());
        }
        else
        {
            g.setColour (amber);
            g.drawHorizontalLine ((int) yOf (mean * 4.0f), area.getX(), area.getRight());   // exaggerated x4
            label ("DC OFFSET (x4)");
        }
        g.restoreState();
        return;
    }
    if (id == "fx_reverse")
    {
        dim();
        fillPeaks (g, raw, area, 1.0f, 0.0f, 1.0f, amber, 0.3f);
        return;
    }
    if (id == "fx_polarity")
    {
        dim();
        fillPeaks (g, raw, area, 0.0f, 1.0f, -1.0f, amber, 0.3f);
        return;
    }
    if (id == "fx_rm_mix" || id == "fx_rm_freq")
    {
        dim();
        const float mix = norm ("fx_rm_mix"), cycles = 2.0f + norm ("fx_rm_freq") * 60.0f;
        fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f, amber, 0.3f,
                   [=] (float t) { return (1.0f - mix) + mix * std::sin (juce::MathConstants<float>::twoPi * cycles * t); });
        return;
    }
    if (id == "fx_reverb" || id == "fx_reverb_type")
    {
        const float amt = norm ("fx_reverb");
        const auto body = area.withWidth (W * 0.6f);
        fillPeaks (g, raw, body, 0.0f, 1.0f, 1.0f, col::textDim, 0.25f);
        juce::Path tail;
        const float x0 = body.getRight(), x1 = area.getRight();
        const int steps = 120;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float e = (0.15f + 0.8f * amt) * std::exp (-t * (id == "fx_reverb_type" && norm ("fx_reverb_type") < 0.5f ? 3.0f : 2.0f));
            const float x = x0 + (x1 - x0) * t;
            if (i == 0) tail.startNewSubPath (x, yOf (e)); else tail.lineTo (x, yOf (e));
        }
        g.setColour (amber);
        g.strokePath (tail, juce::PathStrokeType (2.f));
        label ("REVERB TAIL");
        return;
    }
    if (id == "fx_sdelay")
    {
        const float d = val ("fx_sdelay") * 0.2f;
        auto top = area.removeFromTop (H * 0.5f);
        fillPeaks (g, raw, top, d < 0.0f ? -d : 0.0f, d < 0.0f ? 1.0f - d : 1.0f, 1.0f, col::cyan, 0.3f);
        fillPeaks (g, raw, area, d > 0.0f ? -d : 0.0f, d > 0.0f ? 1.0f - d : 1.0f, 1.0f, amber, 0.3f);
        return;
    }
    if (id == "fx_pogo")
    {
        const float d = std::abs (val ("fx_pogo")) * 0.15f;
        for (int i = 3; i >= 0; --i)
            fillPeaks (g, raw, area, -d * (float) i, 1.0f - d * (float) i, std::pow (0.6f, (float) i), i == 0 ? amber : amber.withAlpha (0.5f), 0.15f);
        return;
    }
    if (id == "ts_mul" || id == "ts_time")
    {
        const float factor = std::pow (2.0f, (norm (id) - 0.5f) * 4.0f) * (id == "ts_time" && norm (id) <= 0.0f ? 0.0f : 1.0f);
        const float f = factor > 0.0f ? factor : 1.0f;
        const float L = juce::jmax (1.0f, f);
        fillPeaks (g, raw, area.withWidth (W / L), 0.0f, 1.0f, 1.0f, col::textDim, 0.2f);
        fillPeaks (g, raw, area.withWidth (W * f / L), 0.0f, 1.0f, 1.0f, amber, 0.25f);
        label (juce::String (f, 2) + "x  length");
        return;
    }
    if (id == "ts_pitch" || id == "ts_mode")
    {
        dim();
        const float cycles = 8.0f * std::pow (2.0f, (norm ("ts_pitch") - 0.5f) * 4.0f);
        juce::Path p;
        const int steps = juce::jmax (64, (int) W);
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float y = area.getCentreY() - std::sin (juce::MathConstants<float>::twoPi * cycles * t) * H * 0.3f;
            if (i == 0) p.startNewSubPath (X (t), y); else p.lineTo (X (t), y);
        }
        g.setColour (amber);
        g.strokePath (p, juce::PathStrokeType (2.f));
        return;
    }

    // ---- anything else: the waveform with the value highlighted ----
    fillPeaks (g, raw, area, 0.0f, 1.0f, 1.0f, col::cyan, 0.25f);
    g.setFont (caps (26.f));
    g.setColour (amber.withAlpha (0.9f));
    g.drawText (tweak->getCurrentValueAsText(), area, juce::Justification::centred, false);
}

// ---- PresetBar ---------------------------------------------------------------
PresetBar::PresetBar (AlphaSamplerAudioProcessor& p) : proc (p)
{
    prev.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    name.onClick = [this] { menu(); };
    saveBtn.onClick = [this] { save(); };
    undoBtn.onClick = [this] { undo(); };
    redoBtn.onClick = [this] { redo(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &prev, &name, &next, &saveBtn, &undoBtn, &redoBtn })
        addAndMakeVisible (c);
    refresh();
}

void PresetBar::resized()
{
    auto a = getLocalBounds();
    prev.setBounds (a.removeFromLeft (26));
    a.removeFromLeft (2);
    redoBtn.setBounds (a.removeFromRight (46));
    a.removeFromRight (2);
    undoBtn.setBounds (a.removeFromRight (46));
    a.removeFromRight (2);
    saveBtn.setBounds (a.removeFromRight (46));
    a.removeFromRight (2);
    next.setBounds (a.removeFromRight (26));
    a.removeFromRight (2);
    name.setBounds (a);
}

void PresetBar::refresh()
{
    const auto n = proc.presetName();
    const auto txt = n.isEmpty() ? juce::String ("INIT / UNSAVED") : n.toUpperCase();
    if (name.getButtonText() != txt) name.setButtonText (txt);
    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
}

void PresetBar::undo() { proc.undoStep(); refresh(); }
void PresetBar::redo() { proc.redoStep(); refresh(); }

static void collectUser (const juce::File& dir, std::vector<juce::File>& out)
{
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml", juce::File::FollowSymlinks::no)) out.push_back (f);
    for (const auto& d : dir.findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no)) collectUser (d, out);
}

std::vector<PresetBar::Item> PresetBar::items() const
{
    std::vector<Item> v;
    for (int i = 0; i < AlphaSamplerAudioProcessor::factoryCount(); ++i) v.push_back ({ AlphaSamplerAudioProcessor::factoryName (i), i, {} });
    std::vector<juce::File> files;
    collectUser (proc.presetDir(), files);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName().compareIgnoreCase (b.getFullPathName()) < 0; });
    for (const auto& f : files) v.push_back ({ f.getFileNameWithoutExtension(), -1, f });
    return v;
}

void PresetBar::choose (const Item& it)
{
    if (it.factory >= 0) proc.applyFactory (it.factory);
    else proc.loadUserPreset (it.file);
    refresh();
}

void PresetBar::step (int dir)
{
    const auto v = items();
    if (v.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].name == proc.presetName()) cur = (int) i;
    const int n = (int) v.size();
    choose (v[(size_t) (((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir + n) % n)]);
}

void PresetBar::menu()
{
    const auto v = items();
    juce::PopupMenu root, fac;
    std::map<juce::String, juce::PopupMenu> subs;
    const auto base = proc.presetDir();
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i].factory >= 0) { fac.addItem ((int) i + 1, v[i].name, true, v[i].name == proc.presetName()); continue; }
        const auto rel = v[i].file.getParentDirectory().getRelativePathFrom (base).replaceCharacter ('\\', '/');
        auto& m = rel == "." ? root : subs[rel];
        m.addItem ((int) i + 1, v[i].name, true, v[i].name == proc.presetName());
    }
    juce::PopupMenu top;
    top.addSubMenu ("Factory", fac);
    top.addSeparator();
    for (auto& [k, m] : subs) top.addSubMenu (k, m);
    top.addItem (-2, "Open preset folder");
    top.addItem (-3, "Refresh");
    top.addSeparator();
    // user presets that sit directly in the preset folder
    root.addSeparator();
    top.addSubMenu ("User", root);
    top.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&name),
        [this, v, safe = juce::Component::SafePointer<PresetBar> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == -2) { proc.presetDir().createDirectory(); proc.presetDir().revealToUser(); return; }
            if (r > 0 && (size_t) r <= v.size()) choose (v[(size_t) r - 1]);
        });
}

void PresetBar::save()
{
    auto* w = new juce::AlertWindow ("Save preset", "Name (use folder/name for sub-folders).\nSaved in Documents/Alpha Sampler/Presets", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("n", proc.presetName());
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Save + sample", 2);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, safe = juce::Component::SafePointer<PresetBar> (this)] (int r)
    {
        if (safe != nullptr && r > 0) proc.saveUserPreset (w->getTextEditorContents ("n"), r == 2);
        if (safe != nullptr) safe->refresh();
    }), true);
}

// ---- KnobCell --------------------------------------------------------------
KnobCell::KnobCell()
{
    knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.onValueChange = [this] { updateReadout(); };

    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (pt::ui::caps (9.5f));
    caption.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    caption.setInterceptsMouseClicks (false, false);

    valueLabel.setJustificationType (juce::Justification::centred);
    valueLabel.setFont (pt::ui::sans (11.f));
    valueLabel.setColour (juce::Label::textColourId, pt::ui::col::text);
    valueLabel.setInterceptsMouseClicks (false, false);

    addAndMakeVisible (knob);
    addAndMakeVisible (caption);
    addAndMakeVisible (valueLabel);
    addAndMakeVisible (block);
}

void KnobCell::attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& text)
{
    param = apvts.getParameter (paramID);
    jassert (param != nullptr);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, knob);
    if (param != nullptr)
        knob.setDoubleClickReturnValue (true, (double) param->convertFrom0to1 (param->getDefaultValue()));
    caption.setText (text, juce::dontSendNotification);
    updateReadout();
}

void KnobCell::resized()
{
    auto a = getLocalBounds();
    caption.setBounds (a.removeFromTop (14));
    block.setBounds (a.removeFromBottom (12).reduced (6, 1));
    valueLabel.setBounds (a.removeFromBottom (16));
    const int s = juce::jmin (a.getWidth(), a.getHeight());
    knob.setBounds (a.withSizeKeepingCentre (s, s));
}

void KnobCell::updateReadout()
{
    if (param == nullptr) return;
    valueLabel.setText (param->getCurrentValueAsText(), juce::dontSendNotification);
    block.setValue (param->getValue());
}

// ---- ToggleCell ------------------------------------------------------------
ToggleCell::ToggleCell()
{
    captionLabel.setJustificationType (juce::Justification::centredLeft);
    captionLabel.setFont (pt::ui::sans (11.f));
    captionLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    captionLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (toggle);
    addAndMakeVisible (captionLabel);
}

void ToggleCell::attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& text)
{
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, paramID, toggle);
    captionLabel.setText (text, juce::dontSendNotification);
}

void ToggleCell::resized()
{
    auto a = getLocalBounds().reduced (2);
    toggle.setBounds (a.removeFromLeft (28));
    captionLabel.setBounds (a);
}

// ---- ChoiceCell ------------------------------------------------------------
ChoiceCell::ChoiceCell()
{
    caption.setFont (pt::ui::caps (9.5f));
    caption.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    caption.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (caption);
    addAndMakeVisible (combo);
}

void ChoiceCell::attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& text)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramID)))
        combo.addItemList (choice->choices, 1);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, paramID, combo);
    caption.setText (text, juce::dontSendNotification);
}

void ChoiceCell::resized()
{
    auto a = getLocalBounds().reduced (2);
    caption.setBounds (a.removeFromTop (14));
    combo.setBounds (a.removeFromTop (28));
}

// ---- layout helper -----------------------------------------------------------
void placeGrid (juce::Rectangle<int> area, int columns, int rowHeight,
                std::initializer_list<juce::Component*> items, int pad)
{
    const int cw = area.getWidth() / juce::jmax (1, columns);
    int i = 0;
    for (auto* c : items)
    {
        const int col = i % columns, row = i / columns;
        if (c != nullptr)
            c->setBounds (juce::Rectangle<int> (area.getX() + col * cw, area.getY() + row * rowHeight, cw, rowHeight).reduced (pad));
        ++i;
    }
}

// ---- SamplePage -------------------------------------------------------------
SamplePage::SamplePage (AlphaSamplerAudioProcessor& p) : processor (p)
{
    auto& ap = p.apvts();
    keepDisk.attach (ap, "keep_disk", "Keep on disk (stream)");
    resample.attach (ap, "resample", "Resample");
    loadRegions.attach (ap, "load_regions", "Load regions");
    loadSlices.attach (ap, "load_slices", "Load slices");
    declickMode.attach (ap, "declick", "DECLICKING");
    startOffset.attach (ap, "start_offset", "START OFFSET");

    loadButton.onClick = [this]
    {
        const auto last = processor.samplePath();
        const auto start = last.isNotEmpty() ? juce::File (last).getParentDirectory()
                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        juce::Component::SafePointer<SamplePage> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Load sample", start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe] (const juce::FileChooser& fc)
            {
                if (safe == nullptr) return;
                const auto f = fc.getResult();
                if (f.existsAsFile()) safe->processor.loadFile (f);
                safe->updateFileLabel();
            });
    };
    clearButton.onClick = [this] { processor.clearFile(); updateFileLabel(); };
    detectButton.onClick = [this] { processor.detectTempo(); };
    fitButton.onClick = [this] { processor.fitToHost(); };
    bpmLabel.setFont (pt::ui::sans (12.f));
    bpmLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);

    fileLabel.setFont (pt::ui::sans (12.f));
    fileLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    fileLabel.setJustificationType (juce::Justification::centredLeft);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &loadButton, &clearButton, &fileLabel,
            &keepDisk, &resample, &loadRegions, &loadSlices, &declickMode, &startOffset, &detectButton, &fitButton, &bpmLabel })
        addAndMakeVisible (c);
    updateFileLabel();
}

void SamplePage::updateFileLabel()
{
    const auto path = processor.samplePath();
    fileLabel.setText (path.isEmpty() ? "no file loaded" : juce::File (path).getFileName(), juce::dontSendNotification);
}

void SamplePage::updateTempoLabel()
{
    const auto st = processor.bpm_state.load();
    const float bpm = processor.bpm_found.load(), beats = processor.beats_found.load();
    bpmLabel.setText (st == 1 ? "analysing..." : st == 2 ? "no tempo found"
                      : bpm > 0.0f ? juce::String (bpm, 1) + " BPM  /  " + juce::String (beats, beats == std::floor (beats) ? 0 : 2) + " beats   (host "
                                       + juce::String (processor.hostTempo(), 1) + ")"
                      : juce::String(), juce::dontSendNotification);
    fitButton.setEnabled (beats > 0.0f);
}

void SamplePage::resized()
{
    auto a = getLocalBounds().reduced (12);
    auto row = a.removeFromTop (30);
    loadButton.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (6);
    clearButton.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (10);
    fileLabel.setBounds (row);
    a.removeFromTop (10);
    placeGrid (a.removeFromTop (34), 4, 34, { &keepDisk, &resample, &loadRegions, &loadSlices });
    a.removeFromTop (8);
    placeGrid (a.removeFromTop (130), 4, 130, { &startOffset, nullptr, nullptr, nullptr });
    placeGrid (a.removeFromTop (60), 2, 60, { &declickMode, nullptr });
    a.removeFromTop (10);
    auto t = a.removeFromTop (30);
    detectButton.setBounds (t.removeFromLeft (120));
    t.removeFromLeft (6);
    fitButton.setBounds (t.removeFromLeft (110));
    t.removeFromLeft (10);
    bpmLabel.setBounds (t);
}

// ---- other pages -------------------------------------------------------------
LoopPage::LoopPage (juce::AudioProcessorValueTreeState& ap)
{
    loopStart.attach (ap, "loop_start", "LOOP START");
    loopEnd.attach (ap, "loop_end", "LOOP END");
    loopUse.attach (ap, "loop_use", "Use loop points");
    loopPingpong.attach (ap, "loop_pingpong", "Ping pong");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &loopStart, &loopEnd, &loopUse, &loopPingpong })
        addAndMakeVisible (c);
}

void LoopPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    placeGrid (a.removeFromTop (140), 4, 140, { &loopStart, &loopEnd, nullptr, nullptr });
    a.removeFromTop (8);
    placeGrid (a.removeFromTop (34), 4, 34, { &loopUse, &loopPingpong, nullptr, nullptr });
}

StretchPage::StretchPage (juce::AudioProcessorValueTreeState& ap)
{
    tsPitch.attach (ap, "ts_pitch", "PITCH");
    tsMul.attach (ap, "ts_mul", "STRETCH");
    tsTime.attach (ap, "ts_time", "TIME");
    tsMode.attach (ap, "ts_mode", "METHOD");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &tsPitch, &tsMul, &tsTime, &tsMode })
        addAndMakeVisible (c);
}

void StretchPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    placeGrid (a.removeFromTop (140), 4, 140, { &tsPitch, &tsMul, &tsTime, nullptr });
    a.removeFromTop (8);
    placeGrid (a.removeFromTop (60), 2, 60, { &tsMode, nullptr });
}

FxPage::FxPage (juce::AudioProcessorValueTreeState& ap)
{
    dc.attach (ap, "fx_dc", "Remove DC");
    normalize.attach (ap, "fx_normalize", "Normalize");
    reverse.attach (ap, "fx_reverse", "Reverse");
    polarity.attach (ap, "fx_polarity", "Polarity");
    fadeStereo.attach (ap, "fx_fade_stereo", "Fade stereo");
    swap.attach (ap, "fx_swap", "Swap stereo");
    clip.attach (ap, "fx_clip", "Clip");
    loopFollows.attach (ap, "fx_loop_follows", "Loop follows");
    start.attach (ap, "fx_start", "START");
    length.attach (ap, "fx_length", "END");
    in.attach (ap, "fx_in", "FADE IN");
    out.attach (ap, "fx_out", "FADE OUT");
    crossfade.attach (ap, "fx_crossfade", "CROSSFADE");
    trim.attach (ap, "fx_trim", "TRIM");
    boost.attach (ap, "fx_boost", "BOOST");
    eq.attach (ap, "fx_eq", "EQ");
    rmMix.attach (ap, "fx_rm_mix", "RING MIX");
    rmFreq.attach (ap, "fx_rm_freq", "RING FREQ");
    cut.attach (ap, "fx_cut", "CUTOFF");
    res.attach (ap, "fx_res", "RESONANCE");
    reverb.attach (ap, "fx_reverb", "REVERB");
    sdelay.attach (ap, "fx_sdelay", "ST. DELAY");
    pogo.attach (ap, "fx_pogo", "POGO");
    reverbType.attach (ap, "fx_reverb_type", "REVERB TYPE");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &dc, &normalize, &reverse, &polarity, &fadeStereo, &swap,
            &clip, &loopFollows, &start, &length, &in, &out, &crossfade, &trim, &boost, &eq, &rmMix, &rmFreq, &cut, &res,
            &reverb, &sdelay, &pogo, &reverbType })
        addAndMakeVisible (c);
}

void FxPage::resized()
{
    auto a = getLocalBounds().reduced (8);
    placeGrid (a.removeFromTop (34), 8, 34, { &dc, &normalize, &reverse, &polarity, &fadeStereo, &swap, &clip, &loopFollows }, 1);
    a.removeFromTop (4);
    const int rowH = juce::jmax (100, (a.getHeight() - 56) / 4);
    placeGrid (a.removeFromTop (rowH * 4), 8, rowH,
               { &start, &length, &in, &out, &crossfade, &trim, &boost, &eq,
                 &rmMix, &rmFreq, &cut, &res, &reverb, &sdelay, &pogo, nullptr }, 2);
    placeGrid (a, 4, 56, { &reverbType, nullptr, nullptr, nullptr });
}

FilterPage::FilterPage (juce::AudioProcessorValueTreeState& ap)
{
    cut.attach (ap, "f_cut", "CUTOFF  (MOD X)");
    res.attach (ap, "f_res", "RESONANCE  (MOD Y)");
    type.attach (ap, "f_type", "TYPE");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &cut, &res, &type }) addAndMakeVisible (c);
}

void FilterPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    placeGrid (a.removeFromTop (140), 4, 140, { &cut, &res, nullptr, nullptr });
    a.removeFromTop (8);
    placeGrid (a.removeFromTop (60), 2, 60, { &type, nullptr });
}

MiscPage::MiscPage (AlphaSamplerAudioProcessor& pr) : proc (pr)
{
    auto& ap = pr.apvts();
    root.attach (ap, "root", "ROOT NOTE");
    zoneLo.attach (ap, "zone_lo", "ZONE LOW");
    zoneHi.attach (ap, "zone_hi", "ZONE HIGH");
    sliceBase.attach (ap, "slice_base", "FIRST SLICE NOTE");
    sliceOn.attach (ap, "slice_on", "Slice mode (notes play slices)");
    fineLabel.setText ("FINE TUNE (cents)", juce::dontSendNotification);
    fineLabel.setFont (pt::ui::caps (9.5f));
    fineLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    sliceInfo.setFont (pt::ui::sans (11.f));
    sliceInfo.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    fine.setSliderStyle (juce::Slider::LinearHorizontal);
    fine.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    fineAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (ap, "fine", fine);
    mainPitch.attach (ap, "main_pitch", "Pitch bend applies");
    addKey.attach (ap, "add_key", "Add root note to key");

    equalBtn.onClick = [this]
    {
        juce::PopupMenu m;
        for (int n : { 2, 4, 8, 16, 32, 64 }) m.addItem (n, juce::String (n) + " equal slices");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&equalBtn), [this] (int r) { if (r > 0) proc.autoSlice (0, r); });
    };
    transBtn.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (30, "Low sensitivity");
        m.addItem (60, "Medium sensitivity");
        m.addItem (90, "High sensitivity");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&transBtn), [this] (int r) { if (r > 0) proc.autoSlice (1, r); });
    };
    beatBtn.onClick = [this] { proc.autoSlice (2, 0); };
    clearBtn.onClick = [this] { proc.setSlices ({}); };
    exportBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Folder for slice files", proc.dataDir().getChildFile ("Slices"));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) proc.exportAudio (fc.getResult(), 2, false); });
    };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &root, &zoneLo, &zoneHi, &sliceBase, &sliceOn, &fineLabel, &fine, &mainPitch, &addKey,
                                                                        &equalBtn, &transBtn, &beatBtn, &clearBtn, &exportBtn, &sliceInfo })
        addAndMakeVisible (c);
}

void MiscPage::tick()
{
    const auto n = (int) proc.slices().size();
    const auto t = n == 0 ? juce::String ("no slices - Shift+click the waveform to add one, right-click a marker to remove it")
                          : juce::String (n) + " slices: notes " + juce::String (laws::note_name (proc.sliceBase())) + " to "
                              + juce::String (laws::note_name (proc.sliceBase() + n - 1));
    if (sliceInfo.getText() != t) sliceInfo.setText (t, juce::dontSendNotification);
}

void MiscPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    placeGrid (a.removeFromTop (60), 3, 60, { &root, &zoneLo, &zoneHi });
    a.removeFromTop (8);
    fineLabel.setBounds (a.removeFromTop (16));
    fine.setBounds (a.removeFromTop (28));
    a.removeFromTop (8);
    placeGrid (a.removeFromTop (34), 3, 34, { &mainPitch, &addKey, nullptr });
    a.removeFromTop (10);
    placeGrid (a.removeFromTop (60), 3, 60, { &sliceBase, nullptr, nullptr });
    sliceOn.setBounds (a.removeFromTop (30).withWidth (260));
    placeGrid (a.removeFromTop (34), 5, 34, { &equalBtn, &transBtn, &beatBtn, &clearBtn, &exportBtn });
    sliceInfo.setBounds (a.removeFromTop (24));
}

EnvelopePage::EnvelopePage (juce::AudioProcessorValueTreeState& ap, const juce::String& p)
{
    on.attach (ap, p + "_on", "Envelope on");
    tempo.attach (ap, p + "_tempo", "Tempo sync");
    delay.attach (ap, p + "_delay", "DELAY");
    att.attach (ap, p + "_att", "ATTACK");
    hold.attach (ap, p + "_hold", "HOLD");
    dec.attach (ap, p + "_dec", "DECAY");
    sus.attach (ap, p + "_sus", "SUSTAIN");
    rel.attach (ap, p + "_rel", "RELEASE");
    attT.attach (ap, p + "_att_t", "ATT TENSION");
    decT.attach (ap, p + "_dec_t", "DEC TENSION");
    relT.attach (ap, p + "_rel_t", "REL TENSION");
    amt.attach (ap, p + "_amt", "AMOUNT");
    lfoShape.attach (ap, p + "_lfo_shape", "LFO SHAPE");
    lfoDelay.attach (ap, p + "_lfo_delay", "LFO DELAY");
    lfoAtt.attach (ap, p + "_lfo_att", "LFO ATTACK");
    lfoAmt.attach (ap, p + "_lfo_amt", "LFO AMOUNT");
    lfoSpeed.attach (ap, p + "_lfo_speed", "LFO SPEED");
    lfoTempo.attach (ap, p + "_lfo_tempo", "LFO tempo sync");
    lfoGlobal.attach (ap, p + "_lfo_global", "LFO global");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &on, &tempo, &lfoTempo, &lfoGlobal, &delay, &att, &hold, &dec,
            &sus, &rel, &attT, &decT, &relT, &amt, &lfoShape, &lfoDelay, &lfoAtt, &lfoAmt, &lfoSpeed })
        addAndMakeVisible (c);
}

void EnvelopePage::resized()
{
    auto a = getLocalBounds().reduced (6);
    const int rowH = juce::jmax (96, (a.getHeight() - 34) / 3);
    placeGrid (a.removeFromTop (34), 4, 34, { &on, &tempo, &lfoTempo, &lfoGlobal }, 1);
    placeGrid (a.removeFromTop (rowH), 6, rowH, { &delay, &att, &hold, &dec, &sus, &rel }, 2);
    placeGrid (a.removeFromTop (rowH), 6, rowH, { &attT, &decT, &relT, &amt, &lfoShape, nullptr }, 2);
    placeGrid (a.removeFromTop (rowH), 6, rowH, { &lfoDelay, &lfoAtt, &lfoAmt, &lfoSpeed, nullptr, nullptr }, 2);
}

EnvTab::EnvTab (juce::AudioProcessorValueTreeState& ap)
{
    const auto bg = pt::ui::col::metalLo;
    tabs.addTab ("PAN",    bg, new EnvelopePage (ap, "envpan"),   true);
    tabs.addTab ("VOLUME", bg, new EnvelopePage (ap, "envvol"),   true);
    tabs.addTab ("MOD X",  bg, new EnvelopePage (ap, "envmodx"),  true);
    tabs.addTab ("MOD Y",  bg, new EnvelopePage (ap, "envmody"),  true);
    tabs.addTab ("PITCH",  bg, new EnvelopePage (ap, "envpitch"), true);
    tabs.setTabBarDepth (24);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);
}

EnvTab::~EnvTab() = default;
void EnvTab::resized() { tabs.setBounds (getLocalBounds()); }

// ---- ToolsPage ---------------------------------------------------------------
namespace
{
juce::String lastScript;
const char* exampleScript =
    "import sys, wave\n"
    "src, dst = sys.argv[1], sys.argv[2]\n"
    "opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)   # rate, bpm, root\n"
    "with wave.open(src, 'rb') as w:\n"
    "    p = w.getparams()\n"
    "    data = w.readframes(p.nframes)\n"
    "fs = p.sampwidth * p.nchannels\n"
    "frames = [data[i:i + fs] for i in range(0, len(data), fs)]\n"
    "frames.reverse()   # edit here\n"
    "with wave.open(dst, 'wb') as o:\n"
    "    o.setparams(p)\n"
    "    o.writeframes(b''.join(frames))\n";
}

ToolsPage::ToolsPage (AlphaSamplerAudioProcessor& p) : proc (p)
{
    if (lastScript.isNotEmpty()) script = juce::File (lastScript);
    auto head = [this] (juce::Label& l, const char* t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (pt::ui::caps (10.f));
        l.setColour (juce::Label::textColourId, pt::ui::col::amber);
        addAndMakeVisible (l);
    };
    head (h1, "RENDER TO FILE");
    head (h2, "RECORD");
    head (h3, "PYTHON SCRIPT");
    embed.setToggleState (true, juce::dontSendNotification);
    embed.setColour (juce::ToggleButton::textColourId, pt::ui::col::textDim);
    for (auto* l : { &recLabel, &scriptLabel, &pyLabel, &statusLabel })
    {
        l->setFont (pt::ui::sans (12.f));
        l->setColour (juce::Label::textColourId, pt::ui::col::textDim);
        addAndMakeVisible (l);
    }
    source.addItemList ({ "Plugin output (always works)", "Input / sidechain bus" }, 1);
    source.setSelectedId (1, juce::dontSendNotification);
    loadAfter.setColour (juce::ToggleButton::textColourId, pt::ui::col::textDim);

    renderBtn.onClick = [this] { saveAs (0); };
    noteBtn.onClick = [this] { saveAs (1); };
    recBtn.onClick = [this]
    {
        if (proc.recording()) proc.stopRecording();
        else proc.startRecording (source.getSelectedId() - 1);
    };
    scriptBtn.onClick = [this] { scriptMenu(); };
    runBtn.onClick = [this] { if (script.existsAsFile()) proc.runScript (script); else proc.setStatus ("choose a script first"); };
    revertBtn.onClick = [this] { proc.revertEdit(); };
    pyBtn.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Auto-detect (py -3, python, python3)");
        m.addItem (2, "Browse for python.exe...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pyBtn), [this] (int r)
        {
            if (r == 1) proc.setPythonPath ({});
            if (r != 2) return;
            chooser = std::make_unique<juce::FileChooser> ("Python executable", juce::File ("C:\\"), "*.exe;*");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) proc.setPythonPath (fc.getResult().getFullPathName()); });
        });
    };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &renderBtn, &noteBtn, &loadAfter, &embed, &recBtn, &source, &scriptBtn, &runBtn, &revertBtn, &pyBtn })
        addAndMakeVisible (c);
}

void ToolsPage::saveAs (int mode)
{
    const auto base = juce::File (proc.samplePath()).getFileNameWithoutExtension();
    chooser = std::make_unique<juce::FileChooser> ("Render to file", proc.dataDir().getChildFile ("Renders").getChildFile (base + (mode ? "_note.wav" : "_render.wav")), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, mode] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              proc.exportAudio (f.withFileExtension ("wav"), mode, loadAfter.getToggleState(), embed.getToggleState());
                          });
}

void ToolsPage::scriptMenu()
{
    const auto dir = proc.dataDir().getChildFile ("Scripts");
    dir.createDirectory();
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.py");
    if (files.isEmpty())
    {
        dir.getChildFile ("reverse_example.py").replaceWithText (exampleScript);
        files = dir.findChildFiles (juce::File::findFiles, false, "*.py");
    }
    juce::PopupMenu m;
    for (int i = 0; i < files.size(); ++i) m.addItem (i + 1, files[i].getFileName(), true, files[i] == script);
    m.addSeparator();
    m.addItem (-1, "Browse...");
    m.addItem (-2, "Open scripts folder");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scriptBtn), [this, files, dir] (int r)
    {
        if (r > 0) { script = files[r - 1]; lastScript = script.getFullPathName(); }
        else if (r == -2) dir.revealToUser();
        else if (r == -1)
        {
            chooser = std::make_unique<juce::FileChooser> ("Python script", dir, "*.py");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) { script = fc.getResult(); lastScript = script.getFullPathName(); } });
        }
    });
}

void ToolsPage::tick()
{
    const bool rec = proc.recording();
    recBtn.setButtonText (rec ? "STOP" : "REC");
    recLabel.setText (rec ? juce::String (proc.recSeconds(), 1) + " s   peak " + juce::String (20.0f * std::log10 (juce::jmax (proc.recPeak(), 1.0e-6f)), 1) + " dB   (max 120 s)"
                          : "Stopping saves a WAV in Documents/Alpha Sampler/Recordings and loads it as the sample.",
                      juce::dontSendNotification);
    scriptLabel.setText (script.getFileName().isEmpty() ? "no script" : script.getFileName()
                             + (proc.originPath().isNotEmpty() ? "     edited - original: " + juce::File (proc.originPath()).getFileName() : juce::String()),
                         juce::dontSendNotification);
    pyLabel.setText ("python: " + (proc.pythonPath().isEmpty() ? juce::String ("auto") : proc.pythonPath()), juce::dontSendNotification);
    statusLabel.setText (proc.statusText(), juce::dontSendNotification);
    revertBtn.setEnabled (proc.originPath().isNotEmpty());
}

void ToolsPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    h1.setBounds (a.removeFromTop (18));
    auto r = a.removeFromTop (30);
    renderBtn.setBounds (r.removeFromLeft (150));
    r.removeFromLeft (6);
    noteBtn.setBounds (r.removeFromLeft (150));
    r.removeFromLeft (14);
    loadAfter.setBounds (r.removeFromLeft (200));
    embed.setBounds (r.removeFromLeft (220));
    a.removeFromTop (18);
    h2.setBounds (a.removeFromTop (18));
    r = a.removeFromTop (30);
    source.setBounds (r.removeFromLeft (230));
    r.removeFromLeft (6);
    recBtn.setBounds (r.removeFromLeft (80));
    a.removeFromTop (4);
    recLabel.setBounds (a.removeFromTop (22));
    a.removeFromTop (14);
    h3.setBounds (a.removeFromTop (18));
    r = a.removeFromTop (30);
    scriptBtn.setBounds (r.removeFromLeft (110));
    r.removeFromLeft (6);
    runBtn.setBounds (r.removeFromLeft (70));
    r.removeFromLeft (6);
    revertBtn.setBounds (r.removeFromLeft (80));
    r.removeFromLeft (6);
    pyBtn.setBounds (r.removeFromLeft (110));
    a.removeFromTop (4);
    scriptLabel.setBounds (a.removeFromTop (20));
    pyLabel.setBounds (a.removeFromTop (20));
    a.removeFromTop (8);
    statusLabel.setBounds (a.removeFromTop (24));
}

// ---- the editor ---------------------------------------------------------------
AlphaSamplerAudioProcessorEditor::AlphaSamplerAudioProcessorEditor (AlphaSamplerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      samplePage (p), loopPage (p.apvts()), stretchPage (p.apvts()), fxPage (p.apvts()),
      filterPage (p.apvts()), miscPage (p), toolsPage (p), envTab (p.apvts())
{
    setLookAndFeel (&lookAndFeel);

    auto setup = [this] (juce::Label& l, const juce::String& text, juce::Font f, juce::Colour c, juce::Justification j)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (f);
        l.setColour (juce::Label::textColourId, c);
        l.setJustificationType (j);
        addAndMakeVisible (l);
    };
    setup (title, "ALPHA SAMPLER", pt::ui::caps (20.f), pt::ui::col::text, juce::Justification::centredLeft);
    setup (subtitle, "sample | stretch | filter", pt::ui::sans (11.f), pt::ui::col::textDim, juce::Justification::centredLeft);
    setup (status, "NO SAMPLE", pt::ui::caps (9.f), pt::ui::col::textDim, juce::Justification::centredRight);
    setup (scopeLabel, "SCOPE", pt::ui::caps (9.f), pt::ui::col::textDim, juce::Justification::centredLeft);
    setup (meterLabelOut, "OUT", pt::ui::caps (9.f), pt::ui::col::cyan, juce::Justification::centred);
    setup (meterLabelPeak, "PEAK", pt::ui::caps (9.f), pt::ui::col::missed.brighter (0.25f), juce::Justification::centred);
    addAndMakeVisible (scope);
    addAndMakeVisible (presets);
    setWantsKeyboardFocus (true);
    waveform.setProcessor (&processor);
    addAndMakeVisible (waveform);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (peakMeter);

    const auto bg = pt::ui::col::metalMid;
    tabs.addTab ("SAMPLE",  bg, &samplePage,  false);
    tabs.addTab ("LOOP",    bg, &loopPage,    false);
    tabs.addTab ("STRETCH", bg, &stretchPage, false);
    tabs.addTab ("FX",      bg, &fxPage,      false);
    tabs.addTab ("FILTER",  bg, &filterPage,  false);
    tabs.addTab ("MISC",    bg, &miscPage,    false);
    tabs.addTab ("TOOLS",   bg, &toolsPage,   false);
    tabs.addTab ("ENV",     bg, &envTab,      false);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    setResizeLimits (860, 700, 1800, 1200);
    setSize (1000, 800);
    setResizable (true, true);
    startTimerHz (30);
}

AlphaSamplerAudioProcessorEditor::~AlphaSamplerAudioProcessorEditor()
{
    stopTimer();
    tabs.clearTabs();
    setLookAndFeel (nullptr);
}

void AlphaSamplerAudioProcessorEditor::paint (juce::Graphics& g)
{
    pt::ui::brushedMetal (g, getLocalBounds().toFloat(), 0.f, true, lookAndFeel.textures());
    auto header = getLocalBounds().toFloat().reduced (10.f).removeFromTop (58.f);
    pt::ui::brushedMetal (g, header, 8.f, false, lookAndFeel.textures());
    g.setColour (pt::ui::col::amber.withAlpha (0.6f));
    g.fillRect (header.getX() + 12.f, header.getBottom() - 1.f, header.getWidth() - 24.f, 1.f);
}

void AlphaSamplerAudioProcessorEditor::resized()
{
    auto a = getLocalBounds().reduced (10);
    auto header = a.removeFromTop (58).reduced (12, 6);
    auto left = header.removeFromLeft (230);
    title.setBounds (left.removeFromTop (26));
    subtitle.setBounds (left);
    status.setBounds (header.removeFromRight (110));
    presets.setBounds (header.reduced (6, 4));
    a.removeFromTop (8);
    waveform.setBounds (a.removeFromTop (juce::jlimit (150, 260, a.getHeight() / 4)));
    a.removeFromTop (8);

    auto right = a.removeFromRight (200);
    scopeLabel.setBounds (right.removeFromTop (14));
    scope.setBounds (right.removeFromTop (180));
    right.removeFromTop (10);
    auto labels = right.removeFromTop (14);
    meterLabelOut.setBounds (labels.removeFromLeft (labels.getWidth() / 2));
    meterLabelPeak.setBounds (labels);
    auto meters = right.removeFromTop (juce::jmin (220, right.getHeight()));
    outMeter.setBounds (meters.removeFromLeft (meters.getWidth() / 2).reduced (10, 0));
    peakMeter.setBounds (meters.reduced (10, 0));

    a.removeFromRight (8);
    tabs.setBounds (a);
}

bool AlphaSamplerAudioProcessorEditor::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    if (m.isCommandDown() && k.getKeyCode() == 'Z') { m.isShiftDown() ? presets.redo() : presets.undo(); return true; }
    if (m.isCommandDown() && k.getKeyCode() == 'Y') { presets.redo(); return true; }
    return false;
}

// ---- drag & drop ------------------------------------------------------------
namespace
{
bool isSampleFile (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}
} // namespace

bool AlphaSamplerAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files) if (isSampleFile (f)) return true;
    return false;
}

void AlphaSamplerAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int) { waveform.setDragHover (true); }
void AlphaSamplerAudioProcessorEditor::fileDragExit (const juce::StringArray&)            { waveform.setDragHover (false); }

void AlphaSamplerAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDragHover (false);
    for (const auto& f : files)
        if (isSampleFile (f))
        {
            processor.loadFile (juce::File (f));
            samplePage.updateFileLabel();
            break;
        }
}

void AlphaSamplerAudioProcessorEditor::timerCallback()
{
    const float m = juce::jmax (processor.meter_l.exchange (0.0f), processor.meter_r.exchange (0.0f));
    shown = juce::jmax (m, shown * 0.88f);
    hold = juce::jmax (m, hold * 0.995f);
    outMeter.setValue (shown);
    peakMeter.setValue (hold);
    scope.pushSample (shown, hold);

    if (const auto serial = processor.wave_serial.load(); serial != shownWave)
    {
        shownWave = serial;
        waveform.setWave (processor.waveView());
    }
    waveform.setTweak (watcher.active());
    waveform.setTempo (processor.hostTempo());
    presets.refresh();
    samplePage.updateTempoLabel();
    miscPage.tick();
    toolsPage.tick();
    if (auto req = processor.takeLoadRequest())
    {
        if (req->second) processor.loadEdited (req->first); else processor.loadFile (req->first);
        samplePage.updateFileLabel();
    }
    waveform.setBusy (processor.load_state.load() == 1);

    juce::String text;
    switch (processor.load_state.load())
    {
        case 1:  text = "LOADING..."; break;
        case 2:  text = "LOAD FAILED"; break;
        default:
            text = processor.samplePath().isEmpty() ? juce::String ("NO SAMPLE")
                 : juce::String (processor.voices_active.load()) + " VOICES";
    }
    status.setText (text, juce::dontSendNotification);
    samplePage.updateFileLabel();
}

} // namespace as
