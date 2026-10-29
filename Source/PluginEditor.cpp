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

    fileLabel.setFont (pt::ui::sans (12.f));
    fileLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    fileLabel.setJustificationType (juce::Justification::centredLeft);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &loadButton, &clearButton, &fileLabel,
            &keepDisk, &resample, &loadRegions, &loadSlices, &declickMode, &startOffset })
        addAndMakeVisible (c);
    updateFileLabel();
}

void SamplePage::updateFileLabel()
{
    const auto path = processor.samplePath();
    fileLabel.setText (path.isEmpty() ? "no file loaded" : juce::File (path).getFileName(), juce::dontSendNotification);
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

MiscPage::MiscPage (juce::AudioProcessorValueTreeState& ap)
{
    root.attach (ap, "root", "ROOT NOTE");
    zoneLo.attach (ap, "zone_lo", "ZONE LOW");
    zoneHi.attach (ap, "zone_hi", "ZONE HIGH");
    fineLabel.setText ("FINE TUNE (cents)", juce::dontSendNotification);
    fineLabel.setFont (pt::ui::caps (9.5f));
    fineLabel.setColour (juce::Label::textColourId, pt::ui::col::textDim);
    fine.setSliderStyle (juce::Slider::LinearHorizontal);
    fine.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    fineAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (ap, "fine", fine);
    mainPitch.attach (ap, "main_pitch", "Pitch bend applies");
    addKey.attach (ap, "add_key", "Add root note to key");
    for (juce::Component* c : std::initializer_list<juce::Component*> { &root, &zoneLo, &zoneHi, &fineLabel, &fine, &mainPitch, &addKey })
        addAndMakeVisible (c);
}

void MiscPage::resized()
{
    auto a = getLocalBounds().reduced (12);
    placeGrid (a.removeFromTop (60), 3, 60, { &root, &zoneLo, &zoneHi });
    a.removeFromTop (12);
    fineLabel.setBounds (a.removeFromTop (16));
    fine.setBounds (a.removeFromTop (28));
    a.removeFromTop (12);
    placeGrid (a.removeFromTop (34), 3, 34, { &mainPitch, &addKey, nullptr });
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

// ---- the editor ---------------------------------------------------------------
AlphaSamplerAudioProcessorEditor::AlphaSamplerAudioProcessorEditor (AlphaSamplerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      samplePage (p), loopPage (p.apvts()), stretchPage (p.apvts()), fxPage (p.apvts()),
      filterPage (p.apvts()), miscPage (p.apvts()), envTab (p.apvts())
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
    setup (subtitle, "sample · stretch · filter", pt::ui::sans (11.f), pt::ui::col::textDim, juce::Justification::centredLeft);
    setup (status, "NO SAMPLE", pt::ui::caps (9.f), pt::ui::col::textDim, juce::Justification::centredRight);
    setup (scopeLabel, "SCOPE", pt::ui::caps (9.f), pt::ui::col::textDim, juce::Justification::centredLeft);
    setup (meterLabelOut, "OUT", pt::ui::caps (9.f), pt::ui::col::cyan, juce::Justification::centred);
    setup (meterLabelPeak, "PEAK", pt::ui::caps (9.f), pt::ui::col::missed.brighter (0.25f), juce::Justification::centred);
    addAndMakeVisible (scope);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (peakMeter);

    const auto bg = pt::ui::col::metalMid;
    tabs.addTab ("SAMPLE",  bg, &samplePage,  false);
    tabs.addTab ("LOOP",    bg, &loopPage,    false);
    tabs.addTab ("STRETCH", bg, &stretchPage, false);
    tabs.addTab ("FX",      bg, &fxPage,      false);
    tabs.addTab ("FILTER",  bg, &filterPage,  false);
    tabs.addTab ("MISC",    bg, &miscPage,    false);
    tabs.addTab ("ENV",     bg, &envTab,      false);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    setResizeLimits (860, 560, 1800, 1100);
    setSize (1000, 680);
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
    auto left = header.removeFromLeft (header.getWidth() / 2);
    title.setBounds (left.removeFromTop (26));
    subtitle.setBounds (left);
    status.setBounds (header.removeFromRight (200));
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

void AlphaSamplerAudioProcessorEditor::timerCallback()
{
    const float m = juce::jmax (processor.meter_l.exchange (0.0f), processor.meter_r.exchange (0.0f));
    shown = juce::jmax (m, shown * 0.88f);
    hold = juce::jmax (m, hold * 0.995f);
    outMeter.setValue (shown);
    peakMeter.setValue (hold);
    scope.pushSample (shown, hold);

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
