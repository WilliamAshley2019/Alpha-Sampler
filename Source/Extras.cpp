// Extras.cpp — spectral analysis, tempo detection, presets.

#include "PluginProcessor.h"

#include <complex>

namespace as {

// ---- analysis ---------------------------------------------------------------
namespace {
constexpr int FFT_N = 2048, SONO_COLS = 256, SONO_BINS = 96;

void fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -juce::MathConstants<float>::twoPi / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
} // namespace

// Bands are log-spaced 30 Hz .. min(20 kHz, Nyquist); the editor re-derives the same axis from `rate`.
std::shared_ptr<const Spectro> analyse(const Fetch& fetch, size_t frames, double rate)
{
    if (frames < 64 || rate <= 0.0) return nullptr;
    auto sp = std::make_shared<Spectro>();
    sp->cols = SONO_COLS;
    sp->bins = SONO_BINS;
    sp->img.assign(static_cast<size_t>(SONO_COLS * SONO_BINS), 0);
    sp->avg.assign(SONO_BINS, 0.0f);
    sp->bright.assign(SONO_COLS, 0.0f);

    const double fmin = 30.0, fmax = std::min(20000.0, rate * 0.5), bw = rate / FFT_N;
    std::vector<int> k0(SONO_BINS), k1(SONO_BINS);
    for (int b = 0; b < SONO_BINS; ++b) {
        const double lo = fmin * std::pow(fmax / fmin, (b - 0.5) / (SONO_BINS - 1));
        const double hi = fmin * std::pow(fmax / fmin, (b + 0.5) / (SONO_BINS - 1));
        k0[b] = std::clamp(static_cast<int>(lo / bw), 1, FFT_N / 2 - 1);
        k1[b] = std::clamp(std::max(k0[b], static_cast<int>(hi / bw)), 1, FFT_N / 2 - 1);
    }
    std::vector<float> win(FFT_N), x(FFT_N);
    for (int i = 0; i < FFT_N; ++i) win[i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / FFT_N);
    std::vector<std::complex<float>> buf(FFT_N);
    double sq = 0.0, sum = 0.0;
    size_t count = 0;
    std::vector<float> mag(SONO_BINS);

    for (int c = 0; c < SONO_COLS; ++c) {
        const double centre = (c + 0.5) * static_cast<double>(frames) / SONO_COLS;
        const size_t pos = frames > FFT_N ? static_cast<size_t>(std::clamp(centre - FFT_N / 2, 0.0, static_cast<double>(frames - FFT_N))) : 0;
        fetch(pos, FFT_N, x.data());
        for (int i = 0; i < FFT_N; ++i) {
            sq += static_cast<double>(x[i]) * x[i];
            sum += x[i];
            buf[i] = { x[i] * win[i], 0.0f };
        }
        count += FFT_N;
        fft(buf);
        double wsum = 0.0, psum = 0.0;
        for (int b = 0; b < SONO_BINS; ++b) {
            float m = 0.0f;
            for (int k = k0[b]; k <= k1[b]; ++k) m = std::max(m, std::abs(buf[k]));
            m /= (FFT_N / 4.0f);
            mag[b] = m;
            const float db = 20.0f * std::log10(m + 1.0e-9f);
            const float n01 = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            sp->img[static_cast<size_t>(c * SONO_BINS + b)] = static_cast<uint8_t>(n01 * 255.0f);
            sp->avg[b] += n01 / SONO_COLS;
            wsum += static_cast<double>(m) * m * b;
            psum += static_cast<double>(m) * m;
        }
        sp->bright[c] = psum > 1.0e-12 ? static_cast<float>(wsum / psum / (SONO_BINS - 1)) : -1.0f;
    }

    float lo = 1.0f, hi = 0.0f;
    for (float b : sp->bright) if (b >= 0.0f) { lo = std::min(lo, b); hi = std::max(hi, b); }
    for (float& b : sp->bright) b = b < 0.0f ? 0.0f : (hi > lo ? (b - lo) / (hi - lo) : 0.5f);

    const double ms = sq / static_cast<double>(std::max<size_t>(1, count));
    sp->rms_db = ms > 1.0e-12 ? static_cast<float>(10.0 * std::log10(ms)) : -120.0f;
    sp->dc = static_cast<float>(sum / static_cast<double>(std::max<size_t>(1, count)));
    return sp;
}

// ---- tempo ------------------------------------------------------------------
// Onset envelope = log RMS of the first difference per 2.5 ms hop; autocorrelated over 60..200 BPM
// with a mild prior around 120 BPM (octave errors are the usual failure: halve/double by hand).
std::optional<double> detect_bpm(const std::vector<float>& x, double rate, double& beats_out)
{
    beats_out = 0.0;
    const size_t hop = static_cast<size_t>(std::max(1.0, rate / 400.0));
    const double fps = rate / static_cast<double>(hop);
    const size_t n = x.size() / hop;
    if (n < static_cast<size_t>(fps * 1.5)) return std::nullopt;

    std::vector<float> f(n, 0.0f);
    float prev = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        double e = 0.0;
        for (size_t k = 1; k < hop; ++k) {
            const double d = static_cast<double>(x[i * hop + k]) - x[i * hop + k - 1];
            e += d * d;
        }
        const float l = std::log(static_cast<float>(std::sqrt(e / static_cast<double>(hop))) + 1.0e-4f);
        f[i] = i ? std::max(0.0f, l - prev) : 0.0f;
        prev = l;
    }
    const size_t w = static_cast<size_t>(fps * 0.5);
    std::vector<float> g(n);
    double run = 0.0;
    for (size_t i = 0; i < n; ++i) {
        run += f[i];
        if (i >= w) run -= f[i - w];
        g[i] = std::max(0.0f, f[i] - static_cast<float>(run / static_cast<double>(std::min(i + 1, w))));
    }

    const size_t lmin = static_cast<size_t>(60.0 * fps / 200.0), lmax = static_cast<size_t>(std::ceil(60.0 * fps / 60.0));
    if (lmax + 2 >= n) return std::nullopt;
    std::vector<double> ac(lmax * 2 + 2, 0.0);
    auto corr = [&](size_t L) {
        if (L >= n) return 0.0;
        double s = 0.0;
        for (size_t i = 0; i + L < n; ++i) s += static_cast<double>(g[i]) * g[i + L];
        return s / static_cast<double>(n - L);
    };
    for (size_t L = lmin - 1; L <= std::min(lmax * 2 + 1, n - 1); ++L) ac[L] = corr(L);

    size_t best = 0;
    double best_score = 0.0;
    std::vector<double> score(lmax + 2, 0.0);
    for (size_t L = lmin; L <= lmax; ++L) {
        const double bpm = 60.0 * fps / static_cast<double>(L);
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.0, 2.0));
        score[L] = (ac[L] + 0.5 * ac[std::min(L * 2, ac.size() - 1)]) * (0.4 + 0.6 * prior);
        if (score[L] > best_score) { best_score = score[L]; best = L; }
    }
    if (best == 0 || best_score <= 0.0) return std::nullopt;

    double lref = static_cast<double>(best);
    if (best > lmin && best < lmax) {
        const double a = score[best - 1], b = score[best], c = score[best + 1], d = a - 2.0 * b + c;
        if (std::abs(d) > 1.0e-12) lref += std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
    }
    double bpm = 60.0 * fps / lref;

    const double dur = static_cast<double>(x.size()) / rate;
    const double beats = dur * bpm / 60.0;
    const double nb = std::round(beats);
    if (nb >= 1.0 && std::abs(nb / beats - 1.0) < 0.03) { bpm = nb * 60.0 / dur; beats_out = nb; }
    else beats_out = beats >= 1.5 ? std::round(beats) : std::max(0.25, std::round(beats * 4.0) / 4.0);
    return bpm;
}

void AlphaSamplerAudioProcessor::detect_tempo_now()
{
    std::string path;
    { const std::lock_guard<std::mutex> lk(state_lock_); path = loaded_path_; }
    std::unique_ptr<juce::AudioFormatReader> r(path.empty() ? nullptr : library_.formats().createReaderFor(juce::File(path)));
    if (!r || r->lengthInSamples < 1000) { bpm_state.store(2); return; }

    const double rate = r->sampleRate;
    const auto total = static_cast<size_t>(r->lengthInSamples);
    const size_t take = std::min(total, static_cast<size_t>(rate * 180.0));
    std::vector<float> mono(take, 0.0f);
    juce::AudioBuffer<float> b(static_cast<int>(std::min<unsigned>(2, r->numChannels)), 1 << 16);
    for (size_t pos = 0; pos < take; pos += static_cast<size_t>(b.getNumSamples())) {
        const int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(b.getNumSamples()), take - pos));
        if (!r->read(&b, 0, len, static_cast<juce::int64>(pos), true, true)) break;
        for (int i = 0; i < len; ++i) {
            float s = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c) s += b.getSample(c, i);
            mono[pos + static_cast<size_t>(i)] = s / static_cast<float>(b.getNumChannels());
        }
    }
    double beats = 0.0;
    const auto bpm = detect_bpm(mono, rate, beats);
    if (!bpm) { bpm_state.store(2); return; }
    if (take < total) beats = std::max(0.25, std::round(static_cast<double>(total) / rate * *bpm / 60.0));
    bpm_found.store(static_cast<float>(*bpm));
    beats_found.store(static_cast<float>(beats));
    bpm_state.store(0);
}

// Sets "Time stretch (time)" so the whole file spans its detected beat count at the host tempo.
void AlphaSamplerAudioProcessor::fitToHost()
{
    const double beats = beats_found.load();
    if (beats <= 0.0) return;
    const double steps = beats * 4.0;
    const double px = steps < 16.0 ? steps : 16.0 + (steps - 16.0) * 36.0 / 16.0;
    push_snapshot();
    hist_suspend_ = true;
    ts_time->beginChangeGesture();
    ts_time->setValueNotifyingHost(static_cast<float>(std::clamp(px / 36844.0, 0.0, 1.0)));
    ts_time->endChangeGesture();
    hist_suspend_ = false;
}

// ---- presets ----------------------------------------------------------------
namespace {
constexpr float H0 = 0.0015f;   // ~0 s hold; the default hold knob is 0.5 s

struct Factory
{
    const char* name;
    std::vector<std::pair<const char*, float>> v;
};

const std::vector<Factory>& factories()
{
    static const std::vector<Factory> f = {
        { "Init", {} },
        { "Soft Pad", { { "envvol_on", 1 }, { "envvol_att", 0.42f }, { "envvol_hold", H0 }, { "envvol_dec", 0.4f }, { "envvol_sus", 0.85f },
                        { "envvol_rel", 0.48f }, { "f_type", 1 }, { "f_cut", 0.8f } } },
        { "Pluck", { { "envvol_on", 1 }, { "envvol_att", 0.02f }, { "envvol_hold", H0 }, { "envvol_dec", 0.22f }, { "envvol_sus", 0.0f },
                     { "envvol_rel", 0.12f }, { "f_type", 1 }, { "f_cut", 0.35f }, { "f_res", 0.2f },
                     { "envmodx_on", 1 }, { "envmodx_att", 0.02f }, { "envmodx_hold", H0 }, { "envmodx_dec", 0.25f }, { "envmodx_sus", 0.0f },
                     { "envmodx_rel", 0.2f }, { "envmodx_amt", 0.5f } } },
        { "Short Stab", { { "envvol_on", 1 }, { "envvol_att", 0.01f }, { "envvol_hold", H0 }, { "envvol_dec", 0.17f }, { "envvol_sus", 0.6f },
                          { "envvol_rel", 0.1f } } },
        { "Reverse Swell", { { "fx_reverse", 1 }, { "envvol_on", 1 }, { "envvol_att", 0.02f }, { "envvol_hold", H0 }, { "envvol_dec", 0.3f },
                             { "envvol_sus", 1.0f }, { "envvol_rel", 0.15f } } },
        { "Looped Drone", { { "loop_use", 1 }, { "loop_start", 0.25f }, { "loop_end", 0.75f }, { "fx_crossfade", 0.3f },
                            { "envvol_on", 1 }, { "envvol_att", 0.35f }, { "envvol_hold", H0 }, { "envvol_dec", 0.4f }, { "envvol_sus", 1.0f },
                            { "envvol_rel", 0.45f } } },
        { "Wide Space", { { "fx_sdelay", 0.35f }, { "fx_reverb", 0.3f }, { "fx_reverb_type", 1 } } },
        { "Lo-Fi Crunch", { { "fx_boost", 0.35f }, { "fx_clip", 1 }, { "fx_cut", 0.6f }, { "fx_res", 0.2f }, { "fx_rm_mix", 0.06f },
                            { "f_type", 1 }, { "f_cut", 0.55f } } },
        { "Pitch Dive", { { "envpitch_on", 1 }, { "envpitch_att", 0.01f }, { "envpitch_hold", H0 }, { "envpitch_dec", 0.22f }, { "envpitch_sus", 0.0f },
                          { "envpitch_rel", 0.2f }, { "envpitch_amt", -0.5f } } },
        { "Filter Wobble", { { "f_type", 1 }, { "f_cut", 0.5f }, { "f_res", 0.35f }, { "envmodx_lfo_amt", 0.5f }, { "envmodx_lfo_speed", 0.55f } } },
        { "Vibrato", { { "envpitch_lfo_amt", 0.035f }, { "envpitch_lfo_speed", 0.62f }, { "envpitch_lfo_att", 0.3f } } },
        { "Octave Down", { { "ts_mul", 0.25f } } },
        { "Octave Up", { { "ts_mul", 0.75f } } },
        { "Half Speed (keep pitch)", { { "ts_mode", 1 }, { "ts_mul", 0.25f } } },
    };
    return f;
}

void set_plain(juce::RangedAudioParameter* p, float plain)
{
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(plain));
    p->endChangeGesture();
}
} // namespace

int AlphaSamplerAudioProcessor::factoryCount() { return static_cast<int>(factories().size()); }
juce::String AlphaSamplerAudioProcessor::factoryName(int i) { return factories()[static_cast<size_t>(i)].name; }

AlphaSamplerAudioProcessor::Snap AlphaSamplerAudioProcessor::snapshot() const
{
    Snap s;
    for (auto* p : getParameters()) s.push_back(p->getValue());
    return s;
}

void AlphaSamplerAudioProcessor::push_snapshot()
{
    auto s = snapshot();
    if (!undo_stack_.empty() && undo_stack_.back() == s) return;
    undo_stack_.push_back(std::move(s));
    if (undo_stack_.size() > 100) undo_stack_.erase(undo_stack_.begin());
    redo_stack_.clear();
}

void AlphaSamplerAudioProcessor::step_history(std::vector<Snap>& from, std::vector<Snap>& to)
{
    if (from.empty()) return;
    to.push_back(snapshot());
    const auto s = std::move(from.back());
    from.pop_back();
    hist_suspend_ = true;
    const auto& ps = getParameters();
    for (int i = 0; i < ps.size() && static_cast<size_t>(i) < s.size(); ++i) {
        ps[i]->beginChangeGesture();
        ps[i]->setValueNotifyingHost(s[static_cast<size_t>(i)]);
        ps[i]->endChangeGesture();
    }
    hist_suspend_ = false;
}

void AlphaSamplerAudioProcessor::resetParams()
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(p)) {
            r->beginChangeGesture();
            r->setValueNotifyingHost(r->getDefaultValue());
            r->endChangeGesture();
        }
}

void AlphaSamplerAudioProcessor::initPatch() { applyFactory(0); }

void AlphaSamplerAudioProcessor::applyFactory(int i)
{
    if (i < 0 || i >= factoryCount()) return;
    push_snapshot();
    hist_suspend_ = true;
    resetParams();
    for (const auto& [id, v] : factories()[static_cast<size_t>(i)].v)
        if (auto* p = apvts_.getParameter(id)) set_plain(p, v);
    hist_suspend_ = false;
    preset_name_ = i == 0 ? juce::String() : factoryName(i);
}

juce::File AlphaSamplerAudioProcessor::presetDir() const
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Alpha Sampler").getChildFile("Presets");
}

// relName may contain '/' to place the preset in a sub-folder. The sample path is stored only when with_sample.
bool AlphaSamplerAudioProcessor::saveUserPreset(const juce::String& relName, bool with_sample)
{
    auto clean = relName.replaceCharacter('\\', '/').trim();
    if (clean.endsWithIgnoreCase(".xml")) clean = clean.dropLastCharacters(4);
    if (clean.isEmpty() || clean.contains("..")) return false;
    const auto f = presetDir().getChildFile(clean + ".xml");
    if (!f.getParentDirectory().createDirectory()) return false;

    auto state = apvts_.copyState();
    state.setProperty("sample_path", with_sample ? sample_path_ : juce::String(), nullptr);
    state.setProperty("file_settings", with_sample ? file_settings_ : juce::String(), nullptr);
    state.setProperty("preset", f.getFileNameWithoutExtension(), nullptr);
    auto xml = state.createXml();
    if (!xml || !xml->writeTo(f)) return false;
    preset_name_ = f.getFileNameWithoutExtension();
    return true;
}

bool AlphaSamplerAudioProcessor::loadUserPreset(const juce::File& f)
{
    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(f));
    if (!xml || !xml->hasTagName(apvts_.state.getType())) return false;
    const auto vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid()) return false;
    push_snapshot();
    const auto keep_path = sample_path_, keep_fs = file_settings_;
    apvts_.replaceState(vt);
    preset_name_ = f.getFileNameWithoutExtension();
    const auto path = vt.getProperty("sample_path", "").toString();
    if (path.isNotEmpty() && juce::File(path).existsAsFile()) {
        if (path != keep_path) loadFile(juce::File(path));
        file_settings_ = vt.getProperty("file_settings", "").toString();
    } else {
        sample_path_ = keep_path;
        file_settings_ = keep_fs;
    }
    return true;
}

} // namespace as
