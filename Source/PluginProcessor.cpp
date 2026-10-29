// PluginProcessor.cpp — Alpha Sampler plugin shell.
//
// Parameters, sample loading / precompute worker, and the AudioProcessor
// surface. The audio-thread path never blocks: it only reads atomics and
// try_locks a mutex to pick up freshly built samples.

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace as {

namespace {
namespace unit {
inline constexpr double W = 65535.0;
inline constexpr Unit FADE          { 0.0,    1024.0,  0.0f,  W };
inline constexpr Unit BOOST         { 0.0,     256.0,  0.0f,  W };
inline constexpr Unit STEREO_DELAY  { 2048.0, 2048.0, -1.0f,  W };
inline constexpr Unit POGO          {  256.0,  256.0, -1.0f,  W };
inline constexpr Unit FEEDBACK      {  128.0,  128.0, -1.0f,  W };
inline constexpr Unit CUTOFF        {  128.0,  896.0,  0.0f,  W };
inline constexpr Unit RESONANCE     {    0.0,  640.0,  0.0f,  W };
inline constexpr Unit RING_MIX      {    0.0,  256.0,  0.0f,  W };
inline constexpr Unit RING_FREQ     {    0.0,  256.0,  0.0f,  W };
inline constexpr Unit REVERB        {    0.0,  256.0,  0.0f,  W };
inline constexpr Unit CROSSFADE     {    0.0, 1024.0,  0.0f,  static_cast<double>(0xFFFFFFFFu) };
inline constexpr Unit TRIM          {    0.0,  256.0,  0.0f,  static_cast<double>(0xFFFFFFFFu) };
} // namespace unit

// ---- display helpers -------------------------------------------------------
enum class Disp { Percent, Bipolar, Plain, Time, Cents, Mul, Seconds };

juce::String time_text(float knob)
{
    const double s = env::knob_seconds(static_cast<int32_t>(std::lround(static_cast<double>(knob) * 65536.0)));
    return s < 1.0 ? juce::String(juce::roundToInt(s * 1000.0)) + " ms" : juce::String(s, 2) + " s";
}

juce::AudioParameterFloatAttributes attrs(Disp d)
{
    juce::AudioParameterFloatAttributes a;
    switch (d) {
        case Disp::Percent:
            return a.withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; })
                    .withValueFromStringFunction([](const juce::String& t) { return juce::jlimit(0.0f, 1.0f, t.getFloatValue() / 100.0f); });
        case Disp::Bipolar:
            return a.withStringFromValueFunction([](float v, int) { return juce::String(v * 100.0f, 0) + " %"; })
                    .withValueFromStringFunction([](const juce::String& t) { return juce::jlimit(-1.0f, 1.0f, t.getFloatValue() / 100.0f); });
        case Disp::Time:
            return a.withStringFromValueFunction([](float v, int) { return time_text(v); });
        case Disp::Cents:
            return a.withStringFromValueFunction([](float v, int) { return juce::String(v * 24.0f - 12.0f, 2) + " st"; });
        case Disp::Mul:
            return a.withStringFromValueFunction([](float v, int) { return juce::String(std::pow(2.0f, 4.0f * (v - 0.5f)), 2) + "x"; });
        case Disp::Seconds:
            return a.withStringFromValueFunction([](float v, int) { return v <= 1e-6f ? juce::String("off") : juce::String(v * 36844.0f / 16.0f, 1) + " beats"; });
        default:
            return a.withStringFromValueFunction([](float v, int) { return juce::String(v, 2); });
    }
}

struct LayoutBuilder
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::vector<juce::RangedAudioParameter*>* sig = nullptr;

    template <typename P>
    P* add(std::unique_ptr<P> p, bool signature)
    {
        P* raw = p.get();
        if (signature && sig) sig->push_back(raw);
        layout.add(std::move(p));
        return raw;
    }

    juce::AudioParameterFloat* knob(const char* id, const char* name, float def, Disp d = Disp::Percent, bool sg = false)
    {
        return add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{ id, 1 }, name,
                   juce::NormalisableRange<float>(0.0f, 1.0f), def, attrs(d)), sg);
    }
    juce::AudioParameterFloat* bi(const char* id, const char* name, float def, bool sg = false)
    {
        return add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{ id, 1 }, name,
                   juce::NormalisableRange<float>(-1.0f, 1.0f), def, attrs(Disp::Bipolar)), sg);
    }
    juce::AudioParameterBool* flag(const juce::String& id, const juce::String& name, bool def, bool sg = false)
    {
        return add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{ id, 1 }, name, def), sg);
    }
    juce::AudioParameterChoice* choice(const juce::String& id, const juce::String& name,
                                       const juce::StringArray& items, int def, bool sg = false)
    {
        return add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{ id, 1 }, name, items, def), sg);
    }
};

EnvParams make_env(LayoutBuilder& l, const juce::String& p, const juce::String& g, float release_tension)
{
    EnvParams e;
    auto id = [&](const char* s) { return p + "_" + s; };
    auto fl = [&](const char* s, const char* nm, float def, Disp d) {
        return l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{ id(s), 1 }, g + " " + nm,
                     juce::NormalisableRange<float>(0.0f, 1.0f), def, attrs(d)), false);
    };
    auto bi = [&](const char* s, const char* nm, float def) {
        return l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{ id(s), 1 }, g + " " + nm,
                     juce::NormalisableRange<float>(-1.0f, 1.0f), def, attrs(Disp::Bipolar)), false);
    };
    e.on         = l.flag(id("on"), g + " envelope", false);
    e.delay      = fl("delay", "delay", 100.0f / 65536.0f, Disp::Time);
    e.att        = fl("att", "attack", 20000.0f / 65536.0f, Disp::Time);
    e.hold       = fl("hold", "hold", 20000.0f / 65536.0f, Disp::Time);
    e.dec        = fl("dec", "decay", 30000.0f / 65536.0f, Disp::Time);
    e.sus        = fl("sus", "sustain", 50.0f / 128.0f, Disp::Percent);
    e.rel        = fl("rel", "release", 20000.0f / 65536.0f, Disp::Time);
    e.att_t      = bi("att_t", "attack tension", 0.0f);
    e.dec_t      = bi("dec_t", "decay tension", 0.0f);
    e.rel_t      = bi("rel_t", "release tension", release_tension);
    e.amt        = bi("amt", "amount", 0.0f);
    e.tempo      = l.flag(id("tempo"), g + " tempo sync", false);
    e.lfo_shape  = l.choice(id("lfo_shape"), g + " LFO shape", { "Sine", "Triangle", "Pulse" }, 0);
    e.lfo_delay  = fl("lfo_delay", "LFO delay", 100.0f / 65536.0f, Disp::Time);
    e.lfo_att    = fl("lfo_att", "LFO attack", 20000.0f / 65536.0f, Disp::Time);
    e.lfo_amt    = bi("lfo_amt", "LFO amount", 0.0f);
    e.lfo_speed  = fl("lfo_speed", "LFO speed", 32950.0f / 65536.0f, Disp::Plain);
    e.lfo_tempo  = l.flag(id("lfo_tempo"), g + " LFO tempo sync", false);
    e.lfo_global = l.flag(id("lfo_global"), g + " LFO global", false);
    return e;
}

juce::StringArray note_names()
{
    juce::StringArray choices;
    choices.ensureStorageAllocated(132);
    for (int i = 0; i < 132; ++i) choices.add(juce::String(laws::note_name(i)));
    return choices;
}

} // namespace

// ============================================================================
// Unit.
// ============================================================================
uint32_t Unit::stored(float knob) const noexcept
{
    const auto k = std::clamp(static_cast<double>(knob), static_cast<double>(lo), 1.0);
    return static_cast<uint32_t>(std::clamp(base + std::nearbyint(k * span), 0.0, max));
}

float Unit::knob_of(uint32_t s) const noexcept
{
    const auto k = (static_cast<double>(s) - base) / span;
    return static_cast<float>(std::clamp(k, static_cast<double>(lo), 1.0));
}

// ============================================================================
// SampleFx <-> text and <-> knobs.
// ============================================================================
SampleFx fx_from_knobs(const FxKnobs& k, const SampleFx* file)
{
    auto keep = [&](const Unit& u, float knob, uint32_t of_file, bool has_file) -> uint32_t {
        if (has_file && u.knob_of(of_file) == knob) return of_file;
        return u.stored(knob);
    };
    auto range = [&](float knob, uint64_t file_bits, bool has_file) -> uint64_t {
        if (has_file) {
            double v;
            std::memcpy(&v, &file_bits, 8);
            if (static_cast<float>(v) == knob) return file_bits;
        }
        return std::bit_cast<uint64_t>(static_cast<double>(knob));
    };
    auto flag = [](bool on, uint16_t bit) { return on ? bit : 0u; };
    SampleFx out;
    out.flags = static_cast<uint16_t>(flag(k.fade_stereo, SampleFx::FADE_STEREO)
                                    | flag(k.reverse, SampleFx::REVERSE)
                                    | flag(k.clip, SampleFx::CLIP)
                                    | flag(k.swap, SampleFx::SWAP));
    out.boost        = static_cast<uint16_t>(keep(unit::BOOST, k.boost, file ? file->boost : 0u, file != nullptr));
    out.fade_out     = static_cast<uint16_t>(keep(unit::FADE, k.fade_out, file ? file->fade_out : 0u, file != nullptr));
    out.fade_in      = static_cast<uint16_t>(keep(unit::FADE, k.fade_in, file ? file->fade_in : 0u, file != nullptr));
    out.stereo_delay = static_cast<uint16_t>(keep(unit::STEREO_DELAY, k.stereo_delay, file ? file->stereo_delay : 0u, file != nullptr));
    out.pogo         = static_cast<uint16_t>(keep(unit::POGO, k.pogo, file ? file->pogo : 0u, file != nullptr));
    out.feedback     = static_cast<uint16_t>(keep(unit::FEEDBACK, k.eq, file ? file->feedback : 0u, file != nullptr));
    out.cutoff       = static_cast<uint16_t>(keep(unit::CUTOFF, k.cutoff, file ? file->cutoff : 0u, file != nullptr));
    out.resonance    = static_cast<uint16_t>(keep(unit::RESONANCE, k.resonance, file ? file->resonance : 0u, file != nullptr));
    out.ring_mix     = static_cast<uint16_t>(keep(unit::RING_MIX, k.ring_mix, file ? file->ring_mix : 0u, file != nullptr));
    out.ring_freq    = static_cast<uint16_t>(keep(unit::RING_FREQ, k.ring_freq, file ? file->ring_freq : 0u, file != nullptr));
    out.reverb       = static_cast<uint16_t>(keep(unit::REVERB, k.reverb, file ? file->reverb : 0u, file != nullptr));
    out.reverb_type  = static_cast<uint16_t>(std::clamp(k.reverb_type, 0, 1));
    out.ping_pong    = k.ping_pong;
    out.remove_dc    = k.remove_dc;
    out.normalize    = k.normalize;
    out.polarity     = k.polarity;
    out.crossfade    = keep(unit::CROSSFADE, k.crossfade, file ? file->crossfade : 0u, file != nullptr);
    out.trim         = keep(unit::TRIM, k.trim, file ? file->trim : 0u, file != nullptr);
    out.start_bits   = range(k.start, file ? file->start_bits : 0ull, file != nullptr);
    out.length_bits  = range(k.length, file ? file->length_bits : 0ull, file != nullptr);
    out.loop_follows_reverse = k.loop_follows;
    return out;
}

std::optional<SampleFx> fx_from_text(const std::string& t)
{
    uint64_t v[22] {};
    size_t n = 0, at = 0;
    for (size_t i = 0; i < 22; ++i) {
        if (at >= t.size()) break;
        const size_t end = t.find(',', at);
        const auto part = t.substr(at, end == std::string::npos ? std::string::npos : end - at);
        try { v[i] = static_cast<uint64_t>(std::stoull(part)); }
        catch (...) { return std::nullopt; }
        ++n;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (n != 22) return std::nullopt;
    auto w = [](uint64_t x) -> std::optional<uint16_t> { return x <= 65535u ? std::optional<uint16_t>((uint16_t)x) : std::nullopt; };
    auto d = [](uint64_t x) -> std::optional<uint32_t> { return x <= 0xFFFFFFFFu ? std::optional<uint32_t>((uint32_t)x) : std::nullopt; };
    SampleFx s;
    s.flags          = *w(v[0]);
    s.boost          = *w(v[1]);
    s.fade_out       = *w(v[2]);
    s.fade_in        = *w(v[3]);
    s.stereo_delay   = *w(v[4]);
    s.pogo           = *w(v[5]);
    s.feedback       = *w(v[6]);
    s.cutoff         = *w(v[7]);
    s.resonance      = *w(v[8]);
    s.ring_mix       = *w(v[9]);
    s.ring_freq      = *w(v[10]);
    s.reverb         = *w(v[11]);
    s.reverb_type    = *w(v[12]);
    s.ping_pong      = v[13] != 0;
    s.remove_dc      = v[14] != 0;
    s.normalize      = v[15] != 0;
    s.polarity       = v[16] != 0;
    s.crossfade      = *d(v[17]);
    s.trim           = *d(v[18]);
    s.start_bits     = v[19];
    s.length_bits    = v[20];
    s.loop_follows_reverse = v[21] != 0;
    return s;
}

std::string fx_text(const SampleFx& f)
{
    const uint64_t vals[22] = {
        f.flags, f.boost, f.fade_out, f.fade_in, f.stereo_delay, f.pogo, f.feedback,
        f.cutoff, f.resonance, f.ring_mix, f.ring_freq, f.reverb, f.reverb_type,
        f.ping_pong ? 1u : 0u, f.remove_dc ? 1u : 0u, f.normalize ? 1u : 0u, f.polarity ? 1u : 0u,
        f.crossfade, f.trim, f.start_bits, f.length_bits, f.loop_follows_reverse ? 1u : 0u
    };
    std::string s;
    for (size_t i = 0; i < 22; ++i) {
        if (i) s += ',';
        s += std::to_string(vals[i]);
    }
    return s;
}

// ============================================================================
// Parameters.
// ============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AlphaSamplerAudioProcessor::makeLayout()
{
    LayoutBuilder L;
    L.sig = &signature_params_;

    keep_disk      = L.flag("keep_disk", "Keep on disk", false, true);
    resample       = L.flag("resample", "Resample", true);
    load_regions   = L.flag("load_regions", "Load regions", true);
    load_slices    = L.flag("load_slices", "Load slice markers", false);
    declick_mode   = L.choice("declick", "Declicking mode", { "Out only (no bleeding)", "Transient (no bleeding)",
                              "Transient (bleeding)", "Generic (bleeding)", "Smooth (bleeding)", "Crossfade (bleeding)" }, 0);
    start_offset   = L.knob("start_offset", "Sample start offset", 0.0f, Disp::Percent, true);

    auto loop_param = [&](const char* id, const char* name, float def) {
        auto a = juce::AudioParameterFloatAttributes().withStringFromValueFunction(
            [f = frames](float v, int) { return juce::String(static_cast<juce::int64>(std::nearbyint(v * static_cast<float>(f->load())))); });
        return L.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{ id, 1 }, name,
                     juce::NormalisableRange<float>(0.0f, 1.0f), def, a), true);
    };
    loop_start     = loop_param("loop_start", "Loop start position", 0.0f);
    loop_end       = loop_param("loop_end", "Loop end position", 1.0f);
    loop_use       = L.flag("loop_use", "Use loop points", true, true);
    loop_pingpong  = L.flag("loop_pingpong", "Ping pong loop", false, true);

    ts_pitch       = L.knob("ts_pitch", "Pitch shift", 0.5f, Disp::Cents, true);
    ts_mul         = L.knob("ts_mul", "Time stretch (multiplicator)", 0.5f, Disp::Mul, true);
    ts_time        = L.knob("ts_time", "Time stretch (time)", 0.0f, Disp::Seconds, true);
    ts_mode        = L.choice("ts_mode", "Stretching method", { "Resample", "Stretch", "Stretch Pro", "e3 generic",
                              "e3 mono", "Slice stretch", "Slice map", "Auto", "e2 generic", "e2 transient",
                              "e2 mono", "e2 speech" }, 0, true);

    fx_dc          = L.flag("fx_dc", "Remove DC offset", false, true);
    fx_normalize   = L.flag("fx_normalize", "Normalize", false, true);
    fx_reverse     = L.flag("fx_reverse", "Reverse", false, true);
    fx_polarity    = L.flag("fx_polarity", "Reverse polarity", false, true);
    fx_fade_stereo = L.flag("fx_fade_stereo", "Fade stereo", false, true);
    fx_swap        = L.flag("fx_swap", "Swap stereo", false, true);
    fx_start       = L.knob("fx_start", "Sample start", 0.0f, Disp::Percent, true);
    fx_length      = L.knob("fx_length", "Sample end", 1.0f, Disp::Percent, true);
    fx_in          = L.knob("fx_in", "Quick fade in", 0.0f, Disp::Percent, true);
    fx_out         = L.knob("fx_out", "Quick fade out", 0.0f, Disp::Percent, true);
    fx_crossfade   = L.knob("fx_crossfade", "Crossfade loop", 0.0f, Disp::Percent, true);
    fx_trim        = L.knob("fx_trim", "Silence trimming threshold", 0.0f, Disp::Percent, true);
    fx_boost       = L.knob("fx_boost", "Boost", 0.0f, Disp::Percent, true);
    fx_eq          = L.bi("fx_eq", "EQ", 0.0f, true);
    fx_rm_mix      = L.knob("fx_rm_mix", "Ring modulation mix", 0.0f, Disp::Percent, true);
    fx_rm_freq     = L.knob("fx_rm_freq", "Ring modulation frequency", 0.5f, Disp::Percent, true);
    fx_cut         = L.knob("fx_cut", "Filter cutoff (precomputed)", 1.0f, Disp::Percent, true);
    fx_res         = L.knob("fx_res", "Filter resonance (precomputed)", 0.0f, Disp::Percent, true);
    fx_clip        = L.flag("fx_clip", "Clipping", false, true);
    fx_reverb_type = L.choice("fx_reverb_type", "Reverb type", { "A", "B" }, 1, true);
    fx_reverb      = L.knob("fx_reverb", "Reverb amount", 0.0f, Disp::Percent, true);
    fx_sdelay      = L.bi("fx_sdelay", "Stereo delay", 0.0f, true);
    fx_pogo        = L.bi("fx_pogo", "Pogo effect", 0.0f, true);
    fx_loop_follows = L.flag("fx_loop_follows", "Loop points follow a reversal", true, true);

    env[0] = make_env(L, "envpan", "Panning", 0.0f);
    env[1] = make_env(L, "envvol", "Volume", -101.0f / 128.0f);
    env[2] = make_env(L, "envmodx", "Mod X", 0.0f);
    env[3] = make_env(L, "envmody", "Mod Y", 0.0f);
    env[4] = make_env(L, "envpitch", "Pitch", 0.0f);

    f_cut          = L.knob("f_cut", "Filter frequency (modulation X)", 1.0f);
    f_res          = L.knob("f_res", "Filter bandwidth (modulation Y)", 0.0f);
    f_type         = L.choice("f_type", "Filter type", { "Fast LP", "LP", "BP", "HP", "BS", "LPx2", "SVF LP", "SVF LPx2" }, 0);

    root           = L.choice("root", "Root note", note_names(), 60);
    zone_lo        = L.choice("zone_lo", "Zone low key", note_names(), 0);
    zone_hi        = L.choice("zone_hi", "Zone high key", note_names(), 131);
    fine = L.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{ "fine", 1 }, "Fine tuning", -100, 100, 0), false);
    main_pitch     = L.flag("main_pitch", "Affected by the main pitch", true);
    add_key        = L.flag("add_key", "Add root note to key", false);

    return std::move(L.layout);
}

// ============================================================================
// Parameter reads (atomics only; safe from any thread).
// ============================================================================
Target AlphaSamplerAudioProcessor::envelope_to_target(const EnvParams& e, double tempo) const
{
    Target t;
    auto ms = [](const juce::AudioParameterFloat* p) { return static_cast<int32_t>(std::lround(p->get() * 65536.0f)); };
    auto st = [](const juce::AudioParameterFloat* p) { return static_cast<int32_t>(std::lround(p->get() * 128.0f)); };
    if (e.on->get()) {
        env::EnvelopeRecord rec;
        rec.enabled = true;
        rec.delay = ms(e.delay);   rec.attack = ms(e.att);  rec.hold = ms(e.hold);
        rec.decay = ms(e.dec);     rec.release = ms(e.rel); rec.sustain = st(e.sus);
        rec.amount = st(e.amt);
        rec.attack_tension = st(e.att_t); rec.decay_tension = st(e.dec_t); rec.release_tension = st(e.rel_t);
        t.env = env::Params::from_record(rec);
        // Tempo sync: envelope times are defined at 130 BPM and scale with the host tempo.
        if (t.env && e.tempo->get()) t.env = t.env->scaled(130.0 / std::max(tempo, 1.0));
    }
    t.amount = static_cast<double>(e.amt->get());
    t.lfo.shape  = static_cast<uint8_t>(std::clamp(e.lfo_shape->getIndex(), 0, 2));
    t.lfo.delay  = env::knob_seconds(ms(e.lfo_delay));
    t.lfo.attack = env::knob_seconds(ms(e.lfo_att));
    t.lfo.amount = static_cast<double>(e.lfo_amt->get());
    const double sp = static_cast<double>(e.lfo_speed->get());
    t.lfo.hz = e.lfo_tempo->get() ? 16.0 * tempo / (60.0 * std::pow(2.0, 8.0 * (1.0 - sp)))
                                  : 0.1 * std::pow(2.0, 8.0 * sp);
    t.lfo.global = e.lfo_global->get();
    return t;
}

std::optional<double> AlphaSamplerAudioProcessor::ts_seconds(double tempo) const
{
    const float knob = ts_time->get();
    if (knob <= 1e-9f) return std::nullopt;
    const double px = static_cast<double>(knob) * 36844.0;
    const double steps = (px < 16.0) ? px : (16.0 + (px - 16.0) / 36.0 * 16.0);
    if (steps < 1.0) return std::nullopt;
    return steps / 4.0 * 60.0 / std::max(tempo, 1.0);
}

Block AlphaSamplerAudioProcessor::make_block(double tempo, double bend) const
{
    Block b;
    b.root  = root->getIndex();
    b.zone  = { zone_lo->getIndex(), zone_hi->getIndex() };
    b.cents = static_cast<double>(fine->get()) + (main_pitch->get() ? bend : 0.0);

    const int mode = ts_mode->getIndex();
    const bool realtime = (mode == 1 || mode == 2);
    const bool resample_mode = (mode == 0);
    b.realtime = realtime;
    b.ts_cents = realtime ? static_cast<double>(ts_pitch->get()) * 2400.0 - 1200.0 : 0.0;
    b.ts_ratio = (resample_mode || realtime) ? std::pow(2.0, 4.0 * (static_cast<double>(ts_mul->get()) - 0.5)) : 1.0;
    b.ts_time  = (resample_mode || realtime) ? ts_seconds(tempo) : std::nullopt;

    for (size_t i = 0; i < 5; ++i) b.targets[i] = envelope_to_target(env[i], tempo);

    b.f_type       = static_cast<uint8_t>(std::clamp(f_type->getIndex(), 0, 7));
    b.f_cut        = f_cut->get();
    b.f_res        = f_res->get();
    b.start_offset = static_cast<double>(start_offset->get());
    b.loop_on      = loop_use->get();
    b.declick      = declick_mode->getIndex();
    b.add_key      = add_key->get();
    return b;
}

FxKnobs AlphaSamplerAudioProcessor::fx_knobs() const
{
    FxKnobs k;
    k.fade_stereo  = fx_fade_stereo->get();
    k.reverse      = fx_reverse->get();
    k.clip         = fx_clip->get();
    k.swap         = fx_swap->get();
    k.boost        = fx_boost->get();
    k.fade_out     = fx_out->get();
    k.fade_in      = fx_in->get();
    k.stereo_delay = fx_sdelay->get();
    k.pogo         = fx_pogo->get();
    k.eq           = fx_eq->get();
    k.cutoff       = fx_cut->get();
    k.resonance    = fx_res->get();
    k.ring_mix     = fx_rm_mix->get();
    k.ring_freq    = fx_rm_freq->get();
    k.reverb       = fx_reverb->get();
    k.reverb_type  = fx_reverb_type->getIndex();
    k.ping_pong    = loop_pingpong->get();
    k.remove_dc    = fx_dc->get();
    k.normalize    = fx_normalize->get();
    k.polarity     = fx_polarity->get();
    k.crossfade    = fx_crossfade->get();
    k.trim         = fx_trim->get();
    k.start        = fx_start->get();
    k.length       = fx_length->get();
    k.loop_follows = fx_loop_follows->get();
    return k;
}

std::optional<SampleFx> AlphaSamplerAudioProcessor::file_fx() const
{
    if (file_settings_.isEmpty()) return std::nullopt;
    return fx_from_text(file_settings_.toStdString());
}

SampleFx AlphaSamplerAudioProcessor::sample_fx() const
{
    const auto file = file_fx();
    return fx_from_knobs(fx_knobs(), file ? &*file : nullptr);
}

FxKey AlphaSamplerAudioProcessor::fx_key(double tempo, uint32_t rate) const
{
    FxKey k;
    k.fx = sample_fx();
    const float ls = loop_start->get(), le = loop_end->get();
    if (ls > 0.0f || le < 1.0f) k.knob_loop = std::make_pair(ls, le);
    k.rate = rate;
    k.stretch = (ts_mode->getIndex() >= 3);
    if (k.stretch) {
        k.ratio = static_cast<float>(std::pow(2.0, 4.0 * (static_cast<double>(ts_mul->get()) - 0.5)));
        k.time  = static_cast<float>(ts_seconds(tempo).value_or(0.0));
        k.shift = static_cast<float>(static_cast<double>(ts_pitch->get()) * 2400.0 - 1200.0);
    }
    return k;
}

DiskKey AlphaSamplerAudioProcessor::disk_key() const
{
    DiskKey d;
    if (!keep_disk->get()) return d;
    d.keep     = true;
    d.realtime = (ts_mode->getIndex() == 1 || ts_mode->getIndex() == 2);
    d.start    = std::bit_cast<uint32_t>(start_offset->get());
    d.loop_on  = loop_use->get();
    return d;
}

uint64_t AlphaSamplerAudioProcessor::signature() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (auto* p : signature_params_) mix(std::bit_cast<uint32_t>(p->getValue()));
    mix(rate_.load());
    if (ts_mode->getIndex() >= 3 && ts_time->get() > 0.0f)
        mix(static_cast<uint64_t>(std::llround(tempo_value() * 100.0)));
    return h;
}

// ============================================================================
// Background worker: loading, building and housekeeping.
// ============================================================================
struct AlphaSamplerAudioProcessor::Worker final : juce::Thread
{
    explicit Worker(AlphaSamplerAudioProcessor& o) : juce::Thread("AlphaSampler worker"), owner(o) {}
    ~Worker() override { stopThread(4000); }

    void wake() { event.signal(); }
    void enqueue_load(const juce::File& f, bool clear)
    {
        { const juce::ScopedLock sl(lock); pending = f; has_pending = true; pending_clear = clear; }
        event.signal();
    }

    void run() override
    {
        while (!threadShouldExit()) {
            const bool woke = event.wait(150);
            if (threadShouldExit()) break;
            owner.engine_.collect_garbage();

            bool load = false, clear = false;
            juce::File f;
            { const juce::ScopedLock sl(lock); load = has_pending; clear = pending_clear; f = pending; has_pending = false; }
            if (load) {
                if (clear) owner.clear(); else owner.load(f);
                continue;
            }
            if (woke) {
                // Debounce knob drags: rebuild only once the parameters have settled.
                auto s = owner.signature();
                for (int i = 0; i < 40 && !threadShouldExit(); ++i) {
                    juce::Thread::sleep(15);
                    const auto s2 = owner.signature();
                    if (s2 == s) break;
                    s = s2;
                }
                owner.update();
            } else {
                std::shared_ptr<const Processed> p;
                { const std::lock_guard<std::mutex> lk(owner.state_lock_); p = owner.processed_; }
                if (p && p->disk && p->disk->news()) owner.update();
            }
        }
    }

    AlphaSamplerAudioProcessor& owner;
    juce::WaitableEvent event;
    juce::CriticalSection lock;
    juce::File pending;
    bool has_pending = false, pending_clear = false;
};

// ============================================================================
// Precompute pipeline.
// ============================================================================
void AlphaSamplerAudioProcessor::publish(std::shared_ptr<const Processed> p)
{
    {
        const std::lock_guard<std::mutex> lk(state_lock_);
        processed_ = p;
    }
    const std::lock_guard<std::mutex> lk(published_lock_);
    published_ = std::move(p);
}

std::optional<std::optional<std::pair<size_t, size_t>>>
AlphaSamplerAudioProcessor::streams(const SourceInfo& src, const stream::Layout& l) const
{
    const auto dk = disk_key();
    if (!dk.keep || dk.realtime) return std::nullopt;
    return as_is(src, fx_key(tempo_value(), rate_.load()), l.rate, l.frames);
}

void AlphaSamplerAudioProcessor::update() { const std::lock_guard<std::mutex> w(work_lock_); update_impl(); }

void AlphaSamplerAudioProcessor::update_impl()
{
    const auto v = raw_version_.load();
    const auto k = fx_key(tempo_value(), rate_.load());
    const auto dk = disk_key();

    std::string path;
    SourceInfo src;
    std::optional<stream::Layout> lay;
    std::shared_ptr<const Processed> now;
    std::shared_ptr<Sample> raw;
    {
        const std::lock_guard<std::mutex> lk(state_lock_);
        path = loaded_path_; src = source_; lay = layout_; now = processed_; raw = raw_;
    }

    auto make = [&](auto&& fill) {
        auto np = std::make_shared<Processed>();
        np->version = v; np->key = k; np->disk_key = dk;
        fill(*np);
        { const std::lock_guard<std::mutex> lk(state_lock_); np->serial = ++serial_; }
        publish(std::move(np));
    };

    if (path.empty()) {
        if (!now || now->sample || now->disk) { frames->store(0); make([](Processed&) {}); }
        return;
    }

    const bool missed = now && now->disk && !now->disk->missed().empty();
    if (now && now->version == v && now->key == k && now->disk_key == dk && !missed) return;

    // ---- stream from disk when the sample needs no precomputation ----
    const auto stream_opt = lay ? streams(src, *lay) : std::nullopt;
    if (stream_opt && lay) {
        const auto start = static_cast<size_t>(std::floor(dk.start_offset() * static_cast<double>(lay->frames)));
        std::vector<size_t> pts { 0, start };
        if (dk.loop_on && stream_opt->has_value() && (*stream_opt)->first < (*stream_opt)->second)
            pts.push_back((*stream_opt)->first);
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

        std::shared_ptr<stream::Played> old_disk = now ? now->disk : nullptr;
        auto disk = (old_disk && old_disk->disk()->layout().frames == lay->frames
                  && old_disk->disk()->layout().rate == lay->rate)
            ? old_disk->disk()
            : stream::Disk::open(juce::File(path), *lay);
        if (disk) {
            auto played = std::make_shared<stream::Played>(disk, pts, old_disk.get());
            frames->store(static_cast<uint32_t>(lay->frames));
            make([&](Processed& np) { np.disk = played; np.looped = *stream_opt; np.file_frames = static_cast<double>(lay->frames); });
            const std::lock_guard<std::mutex> lk(state_lock_);
            if (raw_version_.load() == v) raw_.reset();
            return;
        }
    }

    // ---- build in memory ----
    if (now && now->version == v && now->key == k && now->sample && !now->disk) return;
    if (!raw) {
        try { raw = std::const_pointer_cast<Sample>(library_.load_shared(juce::File(path))); }
        catch (...) { load_state.store(2); return; }
        const std::lock_guard<std::mutex> lk(state_lock_);
        if (raw_version_.load() == v) raw_ = raw;
    }
    auto built = build(*raw, src, k);
    frames->store(static_cast<uint32_t>(built.sample.frames()));
    make([&](Processed& np) {
        np.sample = std::make_shared<Sample>(std::move(built.sample));
        np.looped = built.looped;
        np.file_frames = static_cast<double>(raw->frames()) * built.per_file_frame;
    });
}

void AlphaSamplerAudioProcessor::load(const juce::File& path)
{
    load_state.store(1);
    {
        const std::lock_guard<std::mutex> w(work_lock_);
        const auto src = source_info(path);
        const auto lay = stream::Layout::of(path);
        const bool disk = lay && streams(src.second, *lay).has_value();
        std::shared_ptr<const Sample> s;
        if (!disk) {
            try { s = library_.load_shared(path); }
            catch (...) { load_state.store(2); return; }
        }
        {
            const std::lock_guard<std::mutex> lk(state_lock_);
            raw_ = std::const_pointer_cast<Sample>(s);
            source_ = src.second;
            layout_ = lay;
            loaded_path_ = path.getFullPathName().toStdString();
        }
        raw_version_.fetch_add(1);
        frames->store(s ? static_cast<uint32_t>(s->frames()) : 0);
        update_impl();
    }
    load_state.store(0);
}

void AlphaSamplerAudioProcessor::clear()
{
    {
        const std::lock_guard<std::mutex> w(work_lock_);
        {
            const std::lock_guard<std::mutex> lk(state_lock_);
            raw_.reset(); source_ = {}; layout_.reset(); loaded_path_.clear();
        }
        raw_version_.fetch_add(1);
        update_impl();
    }
    load_state.store(0);
}

// ============================================================================
// Constructor / editor-facing API.
// ============================================================================
AlphaSamplerAudioProcessor::AlphaSamplerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      frames(std::make_shared<std::atomic<uint32_t>>(0)),
      apvts_(*this, nullptr, "PARAMS", makeLayout()),
      engine_(44100.0f),
      worker_(std::make_unique<Worker>(*this))
{
    previews_.reserve(256);
    worker_->startThread(juce::Thread::Priority::background);
}

AlphaSamplerAudioProcessor::~AlphaSamplerAudioProcessor()
{
    worker_->stopThread(5000);
}

void AlphaSamplerAudioProcessor::loadFile(const juce::File& path)
{
    sample_path_ = path.getFullPathName();
    file_settings_.clear();
    load_state.store(1);
    worker_->enqueue_load(path, false);
}

void AlphaSamplerAudioProcessor::clearFile()
{
    sample_path_.clear();
    file_settings_.clear();
    worker_->enqueue_load({}, true);
}

void AlphaSamplerAudioProcessor::preview(uint8_t key, float vel, bool on)
{
    const std::lock_guard<std::mutex> lk(previews_lock_);
    if (previews_.size() < previews_.capacity()) previews_.push_back({ key, on ? vel : -1.0f });
}

// ============================================================================
// Audio.
// ============================================================================
void AlphaSamplerAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    rate_.store(static_cast<uint32_t>(std::lround(sampleRate)));
    engine_.set_rate(static_cast<float>(sampleRate));
    current_serial_ = 0;   // re-adopt the published sample
    last_signature_ = 0;
    if (sample_path_.isNotEmpty()) {
        bool have;
        { const std::lock_guard<std::mutex> lk(state_lock_); have = !loaded_path_.empty(); }
        if (!have) worker_->enqueue_load(juce::File(sample_path_), false);
    }
    worker_->wake();
}

bool AlphaSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void AlphaSamplerAudioProcessor::take(const Processed& p)
{
    if (p.disk) engine_.set_disk(p.disk);
    else        engine_.set_sample(p.sample);
    engine_.set_looped(p.looped);
}

void AlphaSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) set_tempo(*bpm);

    const bool offline = isNonRealtime();
    engine_.set_quality(offline ? 1 : 0);
    engine_.set_wait(offline);

    // Notice parameter changes that need a rebuild; the worker debounces them.
    const auto sig = signature();
    if (sig != last_signature_) { last_signature_ = sig; worker_->wake(); }

    {   // adopt a freshly built sample (never blocks)
        std::unique_lock<std::mutex> lk(published_lock_, std::try_to_lock);
        if (lk.owns_lock() && published_ && published_->serial != current_serial_) {
            current_serial_ = published_->serial;
            take(*published_);
        }
    }

    const int n = buffer.getNumSamples();
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : left;
    auto blk = make_block(tempo_value(), bend_);

    {
        std::unique_lock<std::mutex> lk(previews_lock_, std::try_to_lock);
        if (lk.owns_lock()) {
            for (auto [key, vel] : previews_) {
                if (vel >= 0.0f) engine_.note_on(key, vel, blk);
                else engine_.note_off(key, blk);
            }
            previews_.clear();
        }
    }

    int at = 0;
    for (const auto meta : midi) {
        const int t = juce::jlimit(0, n, meta.samplePosition);
        if (t > at) { engine_.render(left + at, right + at, t - at, blk); at = t; }
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) {
            engine_.note_on(static_cast<uint8_t>(msg.getNoteNumber()), msg.getFloatVelocity(), blk);
        } else if (msg.isNoteOff()) {
            engine_.note_off(static_cast<uint8_t>(msg.getNoteNumber()), blk);
        } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
            engine_.all_off();
        } else if (msg.isPitchWheel()) {
            bend_ = (static_cast<double>(msg.getPitchWheelValue()) / 8192.0 - 1.0) * 200.0;
            blk = make_block(tempo_value(), bend_);
        }
    }
    if (at < n) engine_.render(left + at, right + at, n - at, blk);

    meter_l.store(std::max(buffer.getMagnitude(0, 0, n), meter_l.load()));
    if (buffer.getNumChannels() > 1)
        meter_r.store(std::max(buffer.getMagnitude(1, 0, n), meter_r.load()));
    voices_active.store(engine_.active_voices());
}

juce::AudioProcessorEditor* AlphaSamplerAudioProcessor::createEditor()
{
    return new AlphaSamplerAudioProcessorEditor(*this);
}

void AlphaSamplerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts_.copyState();
    state.setProperty("sample_path", sample_path_, nullptr);
    state.setProperty("file_settings", file_settings_, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void AlphaSamplerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return;
    const auto vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid()) return;
    apvts_.replaceState(vt);
    sample_path_ = vt.getProperty("sample_path", "").toString();
    file_settings_ = vt.getProperty("file_settings", "").toString();
    if (sample_path_.isNotEmpty()) {
        const juce::File f(sample_path_);
        if (f.existsAsFile()) load(f);   // synchronous so offline renders find the sample
    }
}

} // namespace as

// ============================================================================
// JUCE plugin entry point.
// ============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new as::AlphaSamplerAudioProcessor();
}
