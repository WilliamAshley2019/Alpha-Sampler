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
// Onset strength per hop: positive jumps in log RMS of the first difference, minus a 0.5 s running mean.
static std::vector<float> onset_curve(const std::vector<float>& x, size_t hop, double fps)
{
    const size_t n = x.size() / hop;
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
    const size_t w = std::max<size_t>(1, static_cast<size_t>(fps * 0.5));
    std::vector<float> g(n);
    double run = 0.0;
    for (size_t i = 0; i < n; ++i) {
        run += f[i];
        if (i >= w) run -= f[i - w];
        g[i] = std::max(0.0f, f[i] - static_cast<float>(run / static_cast<double>(std::min(i + 1, w))));
    }

    return g;
}

// Onset envelope = log RMS of the first difference per 2.5 ms hop; autocorrelated over 60..200 BPM
// with a mild prior around 120 BPM (octave errors are the usual failure: halve/double by hand).
std::optional<double> detect_bpm(const std::vector<float>& x, double rate, double& beats_out)
{
    beats_out = 0.0;
    const size_t hop = static_cast<size_t>(std::max(1.0, rate / 400.0));
    const double fps = rate / static_cast<double>(hop);
    const size_t n = x.size() / hop;
    if (n < static_cast<size_t>(fps * 1.5)) return std::nullopt;

    const auto g = onset_curve(x, hop, fps);

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


// ---- slices -----------------------------------------------------------------
std::vector<float> AlphaSamplerAudioProcessor::slices() const
{
    std::vector<float> v;
    for (int i = 0, n = slice_n_.load(); i < n; ++i) v.push_back(slice_pos_[static_cast<size_t>(i)].load());
    return v;
}

void AlphaSamplerAudioProcessor::setSlices(std::vector<float> v)
{
    for (float& f : v) f = std::clamp(f, 0.0f, 0.9999f);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(), [](float a, float b) { return b - a < 1.0e-4f; }), v.end());
    if (v.size() > static_cast<size_t>(MAX_SLICES)) v.resize(MAX_SLICES);
    slice_n_.store(0);
    for (size_t i = 0; i < v.size(); ++i) slice_pos_[i].store(v[i]);
    slice_n_.store(static_cast<int>(v.size()));
}

int AlphaSamplerAudioProcessor::slice_index(int note) const
{
    if (!slice_on->get()) return -1;
    const int i = note - slice_base->getIndex();
    return i >= 0 && i < slice_n_.load() ? i : -1;
}

void AlphaSamplerAudioProcessor::trigger(uint8_t note, float vel, const Block& b, int at)
{
    const int idx = slice_index(note);
    if (idx < 0) { engine_.note_on(note, vel, b); return; }

    const int n = slice_n_.load();
    const double s = slice_pos_[static_cast<size_t>(idx)].load();
    const double e = idx + 1 < n ? static_cast<double>(slice_pos_[static_cast<size_t>(idx) + 1].load()) : 1.0;
    Block sb = b;
    sb.root = note;
    sb.add_key = false;
    sb.loop_on = false;
    sb.start_offset = s;
    sb.zone = { 0, 131 };
    for (int i = 0; i < noff_; ++i)
        if (offs_[static_cast<size_t>(i)].key == note) offs_[static_cast<size_t>(i)] = offs_[static_cast<size_t>(--noff_)], --i;
    engine_.note_on(note, vel, sb);

    // The slice is a one-shot: a note-off is scheduled at its end (length scaled like Engine::note_on does).
    const double rate = static_cast<double>(rate_.load());
    const double total = static_cast<double>(frames->load()) / rate;
    const double lr = sb.ts_time ? *sb.ts_time / std::max(total, 1.0e-6) : sb.ts_ratio;
    const double secs = (e - s) * total * std::clamp(lr, 0.0625, 16.0) / std::exp2(sb.cents / 1200.0);
    if (noff_ < static_cast<int>(offs_.size()))
        offs_[static_cast<size_t>(noff_++)] = { clock_ + static_cast<uint64_t>(std::max(0, at)) + static_cast<uint64_t>(std::max(0.001, secs) * rate), note };
}

void AlphaSamplerAudioProcessor::autoSlice(int kind, int arg)
{
    if (kind == 0) {
        std::vector<float> v;
        for (int i = 0; i < std::clamp(arg, 1, MAX_SLICES); ++i) v.push_back(static_cast<float>(i) / static_cast<float>(arg));
        setSlices(std::move(v));
        return;
    }
    if (kind == 2) {
        const float beats = beats_found.load();
        if (beats < 1.0f) { setStatus("run DETECT TEMPO first"); return; }
        std::vector<float> v;
        const int n = std::min(MAX_SLICES, static_cast<int>(std::round(beats)));
        for (int i = 0; i < n; ++i) v.push_back(static_cast<float>(i) / static_cast<float>(n));
        setSlices(std::move(v));
        return;
    }
    runJob([this, arg] {
        juce::AudioBuffer<float> b;
        double rate = 0.0;
        if (!processed_buffer(b, rate) || b.getNumSamples() < 2048) { setStatus("nothing to slice"); return; }
        std::vector<float> mono(static_cast<size_t>(b.getNumSamples()));
        for (int i = 0; i < b.getNumSamples(); ++i) {
            float s = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c) s += b.getSample(c, i);
            mono[static_cast<size_t>(i)] = s / static_cast<float>(b.getNumChannels());
        }
        const size_t hop = static_cast<size_t>(std::max(1.0, rate / 400.0));
        const double fps = rate / static_cast<double>(hop);
        const auto g = onset_curve(mono, hop, fps);
        double mean = 0.0, var = 0.0;
        for (float x : g) mean += x;
        mean /= static_cast<double>(std::max<size_t>(1, g.size()));
        for (float x : g) var += (x - mean) * (x - mean);
        const double sd = std::sqrt(var / static_cast<double>(std::max<size_t>(1, g.size())));
        const double thr = mean + sd * (3.0 - 2.5 * std::clamp(arg, 0, 100) / 100.0);
        const size_t gap = static_cast<size_t>(fps * 0.06);
        std::vector<float> v { 0.0f };
        size_t last = 0;
        for (size_t i = 2; i + 2 < g.size(); ++i)
            if (g[i] > thr && g[i] >= g[i - 1] && g[i] >= g[i + 1] && i - last > gap) {
                v.push_back(static_cast<float>(static_cast<double>(i) / static_cast<double>(g.size())));
                last = i;
            }
        setSlices(std::move(v));
        setStatus(juce::String(slice_n_.load()) + " slices");
    });
}

// ---- jobs / status ------------------------------------------------------------
juce::String AlphaSamplerAudioProcessor::statusText() const { const std::lock_guard<std::mutex> lk(status_lock_); return status_; }
void AlphaSamplerAudioProcessor::setStatus(const juce::String& s) { const std::lock_guard<std::mutex> lk(status_lock_); status_ = s; }

std::optional<std::pair<juce::File, bool>> AlphaSamplerAudioProcessor::takeLoadRequest()
{
    const std::lock_guard<std::mutex> lk(status_lock_);
    auto r = std::move(load_request_);
    load_request_.reset();
    return r;
}

juce::File AlphaSamplerAudioProcessor::dataDir() const
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Alpha Sampler");
}

namespace {
bool write_wav(const juce::File& f, const juce::AudioBuffer<float>& b, double rate)
{
    if (b.getNumSamples() <= 0) return false;
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(f);
    if (static_cast<juce::FileOutputStream*>(os.get())->failedToOpen()) return false;
    juce::WavAudioFormat fmt;
    auto w = fmt.createWriterFor(os, juce::AudioFormatWriterOptions {}.withSampleRate(rate).withNumChannels(b.getNumChannels()).withBitsPerSample(24));
    return w && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}
} // namespace


namespace {
juce::AudioBuffer<float> slice_part(const juce::AudioBuffer<float>& b, int a, int z)
{
    juce::AudioBuffer<float> part(2, z - a);
    for (int c = 0; c < 2; ++c) part.copyFrom(c, 0, b, c, a, z - a);
    part.applyGainRamp(std::max(0, z - a - 64), 64, 1.0f, 0.0f);
    return part;
}

// Appends a RIFF 'cue ' chunk plus 'LIST adtl' labels so DAWs/editors show the slice markers.
bool embed_cues(const juce::File& f, const std::vector<uint32_t>& pos)
{
    if (pos.empty()) return true;
    juce::MemoryBlock mb;
    if (!f.loadFileAsData(mb) || mb.getSize() < 12) return false;
    if (mb.getSize() & 1u) mb.append("\0", 1);
    juce::MemoryOutputStream os(mb, true);
    auto id = [&](const char* t) { os.write(t, 4); };
    id("cue ");
    os.writeInt(static_cast<int>(4 + 24 * pos.size()));
    os.writeInt(static_cast<int>(pos.size()));
    for (size_t i = 0; i < pos.size(); ++i) {
        os.writeInt(static_cast<int>(i + 1)); os.writeInt(static_cast<int>(pos[i]));
        id("data"); os.writeInt(0); os.writeInt(0); os.writeInt(static_cast<int>(pos[i]));
    }
    juce::MemoryOutputStream lab;
    for (size_t i = 0; i < pos.size(); ++i) {
        const auto txt = "Slice " + juce::String(static_cast<int>(i) + 1);
        const int len = 4 + static_cast<int>(txt.getNumBytesAsUTF8()) + 1;
        lab.write("labl", 4); lab.writeInt(len); lab.writeInt(static_cast<int>(i + 1));
        lab.write(txt.toRawUTF8(), txt.getNumBytesAsUTF8()); lab.writeByte(0);
        if (len & 1) lab.writeByte(0);
    }
    id("LIST");
    os.writeInt(static_cast<int>(4 + lab.getDataSize()));
    id("adtl");
    os.write(lab.getData(), lab.getDataSize());
    os.flush();
    const uint32_t riff = static_cast<uint32_t>(mb.getSize() - 8);
    mb.copyFrom(&riff, 4, 4);   // little-endian host assumed (x86/ARM Windows)
    return f.replaceWithData(mb.getData(), mb.getSize());
}
} // namespace

std::vector<uint32_t> wav_cues(const juce::File& f)
{
    std::vector<uint32_t> out;
    std::ifstream in(f.getFullPathName().toStdString(), std::ios::binary);
    char head[12];
    if (!in.read(head, 12) || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return out;
    auto u32 = [](const unsigned char* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); };
    for (;;) {
        unsigned char ch[8];
        if (!in.read(reinterpret_cast<char*>(ch), 8)) break;
        const uint32_t len = u32(ch + 4);
        if (std::memcmp(ch, "cue ", 4) == 0 && len >= 4 && len < (1u << 20)) {
            std::vector<unsigned char> d(len);
            if (!in.read(reinterpret_cast<char*>(d.data()), len)) break;
            const uint32_t n = std::min(u32(d.data()), (len - 4) / 24);
            for (uint32_t i = 0; i < n; ++i) out.push_back(u32(d.data() + 4 + 24 * i + 20));
            break;
        }
        in.seekg(static_cast<std::streamoff>(len) + (len & 1u), std::ios::cur);
        if (!in) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint32_t> AlphaSamplerAudioProcessor::cue_frames(int total) const
{
    std::vector<uint32_t> v;
    for (float s : slices()) v.push_back(static_cast<uint32_t>(s * static_cast<float>(total)));
    return v;
}

juce::File AlphaSamplerAudioProcessor::dragFile(int slice)
{
    juce::AudioBuffer<float> b;
    double rate = 0.0;
    if (!processed_buffer(b, rate)) return {};
    const auto dir = dataDir().getChildFile("Drag");
    const auto base = juce::File(sample_path_).getFileNameWithoutExtension();
    const auto sl = slices();
    const int N = b.getNumSamples();
    if (slice < 0) {
        const auto f = dir.getChildFile(base + "_sliced.wav");
        return write_wav(f, b, rate) && embed_cues(f, cue_frames(N)) ? f : juce::File();
    }
    if (slice >= static_cast<int>(sl.size())) return {};
    const int a = static_cast<int>(sl[static_cast<size_t>(slice)] * static_cast<float>(N));
    const int z = static_cast<size_t>(slice) + 1 < sl.size() ? static_cast<int>(sl[static_cast<size_t>(slice) + 1] * static_cast<float>(N)) : N;
    if (z - a < 2) return {};
    const auto f = dir.getChildFile(base + "_slice_" + juce::String(slice + 1).paddedLeft('0', 2) + ".wav");
    return write_wav(f, slice_part(b, a, z), rate) ? f : juce::File();
}

// Processed sample as the voices play it at their root note (disk-streamed samples are read from the file).
bool AlphaSamplerAudioProcessor::processed_buffer(juce::AudioBuffer<float>& out, double& rate)
{
    std::shared_ptr<const Processed> p;
    std::string path;
    { const std::lock_guard<std::mutex> lk(state_lock_); p = processed_; path = loaded_path_; }
    if (p && p->sample && !p->sample->empty()) {
        const auto& s = *p->sample;
        const bool st = s.right.size() == s.left.size();
        out.setSize(2, s.frames());
        out.copyFrom(0, 0, s.left.data(), s.frames());
        out.copyFrom(1, 0, st ? s.right.data() : s.left.data(), s.frames());
        rate = static_cast<double>(s.rate);
        return true;
    }
    std::unique_ptr<juce::AudioFormatReader> r(path.empty() ? nullptr : library_.formats().createReaderFor(juce::File(path)));
    if (!r || r->lengthInSamples <= 0) return false;
    const int n = static_cast<int>(r->lengthInSamples);
    juce::AudioBuffer<float> tmp(2, n);
    r->read(&tmp, 0, n, 0, true, true);
    if (r->numChannels < 2) tmp.copyFrom(1, 0, tmp, 0, 0, n);
    out = std::move(tmp);
    rate = r->sampleRate;
    return true;
}

// Plays the sample through a private engine at its root note (envelopes, filter, loop, stretch included).
void AlphaSamplerAudioProcessor::render_note(const juce::AudioBuffer<float>& src, double rate, juce::AudioBuffer<float>& out)
{
    std::shared_ptr<const Processed> p;
    { const std::lock_guard<std::mutex> lk(state_lock_); p = processed_; }
    auto smp = std::make_shared<Sample>();
    smp->rate = static_cast<uint32_t>(std::lround(rate));
    smp->left.assign(src.getReadPointer(0), src.getReadPointer(0) + src.getNumSamples());
    smp->right.assign(src.getReadPointer(1), src.getReadPointer(1) + src.getNumSamples());

    Engine eng(static_cast<float>(rate));
    eng.set_quality(1);
    eng.set_wait(true);
    eng.set_sample(smp);
    eng.set_looped(p ? p->looped : std::nullopt);
    Block blk = make_block(tempo_value(), 0.0);
    blk.zone = { 0, 131 };
    const uint8_t key = static_cast<uint8_t>(blk.root);
    const double secs = static_cast<double>(src.getNumSamples()) / rate;
    const double hold = (p && p->looped && blk.loop_on) ? 4.0 : secs * (blk.ts_time ? *blk.ts_time / secs : blk.ts_ratio);
    const int hold_n = static_cast<int>(std::min(hold, 120.0) * rate), max_n = hold_n + static_cast<int>(30.0 * rate);
    out.setSize(2, max_n);
    out.clear();
    eng.note_on(key, 1.0f, blk);
    int pos = 0;
    bool released = false;
    while (pos < max_n) {
        if (!released && pos >= hold_n) { eng.note_off(key, blk); released = true; }
        const int len = std::min(512, (released ? max_n : hold_n) - pos);
        if (len <= 0) break;
        eng.render(out.getWritePointer(0) + pos, out.getWritePointer(1) + pos, len, blk);
        pos += len;
        if (released && !eng.has_voices()) break;
    }
    int end = pos;
    while (end > 1 && std::max(std::abs(out.getSample(0, end - 1)), std::abs(out.getSample(1, end - 1))) < 3.0e-5f) --end;
    out.setSize(2, std::max(1, end), true);
}

void AlphaSamplerAudioProcessor::exportAudio(const juce::File& dest, int mode, bool load_after, bool markers)
{
    setStatus("rendering...");
    runJob([this, dest, mode, load_after, markers] {
        juce::AudioBuffer<float> b;
        double rate = 0.0;
        if (!processed_buffer(b, rate)) { setStatus("nothing to render"); return; }
        if (mode == 2) {
            const auto sl = slices();
            if (sl.empty()) { setStatus("no slices"); return; }
            dest.createDirectory();
            const auto base = juce::File(sample_path_).getFileNameWithoutExtension();
            int ok = 0;
            for (size_t i = 0; i < sl.size(); ++i) {
                const int a = static_cast<int>(sl[i] * static_cast<float>(b.getNumSamples()));
                const int z = i + 1 < sl.size() ? static_cast<int>(sl[i + 1] * static_cast<float>(b.getNumSamples())) : b.getNumSamples();
                if (z - a < 2) continue;
                const auto part = slice_part(b, a, z);
                ok += write_wav(dest.getChildFile(base + "_slice_" + juce::String(static_cast<int>(i) + 1).paddedLeft('0', 2) + ".wav"), part, rate) ? 1 : 0;
            }
            setStatus("exported " + juce::String(ok) + " slices");
            return;
        }
        if (mode == 1) { juce::AudioBuffer<float> o; render_note(b, rate, o); b = std::move(o); }
        if (!write_wav(dest, b, rate)) { setStatus("write failed: " + dest.getFileName()); return; }
        if (markers && mode == 0) embed_cues(dest, cue_frames(b.getNumSamples()));
        setStatus("saved " + dest.getFileName());
        if (load_after) { const std::lock_guard<std::mutex> lk(status_lock_); load_request_ = { dest, true }; }
    });
}

// ---- scripts ------------------------------------------------------------------
// Contract: <python> script.py <in.wav> <out.wav> rate=<Hz> bpm=<host> root=<note>
// in.wav is a 24-bit WAV copy of the loaded file (the original is never modified); out.wav is swapped in on exit code 0.
void AlphaSamplerAudioProcessor::runScript(const juce::File& script)
{
    std::string path;
    { const std::lock_guard<std::mutex> lk(state_lock_); path = loaded_path_; }
    if (path.empty()) { setStatus("load a sample first"); return; }
    setStatus("running " + script.getFileName() + "...");
    const juce::File in(path);
    const auto py = python_path_;
    const double bpm = tempo_value();
    const int rt = root->getIndex();
    juce::StringArray sls;
    for (float s : slices()) sls.add(juce::String(s, 6));
    const auto sl_arg = sls.joinIntoString(",");
    runJob([this, script, in, py, bpm, rt, sl_arg] {
        const auto tmp_in = dataDir().getChildFile("Edits").getChildFile("_script_input.wav");
        {
            std::unique_ptr<juce::AudioFormatReader> r(library_.formats().createReaderFor(in));
            if (!r || r->lengthInSamples <= 0) { setStatus("cannot read " + in.getFileName()); return; }
            const int n = static_cast<int>(r->lengthInSamples);
            juce::AudioBuffer<float> b(2, n);
            r->read(&b, 0, n, 0, true, true);
            if (r->numChannels < 2) b.copyFrom(1, 0, b, 0, 0, n);
            if (!write_wav(tmp_in, b, r->sampleRate)) { setStatus("cannot write script input"); return; }
        }
        const auto out = dataDir().getChildFile("Edits").getChildFile(in.getFileNameWithoutExtension() + "_" + script.getFileNameWithoutExtension() + ".wav").getNonexistentSibling();
        out.getParentDirectory().createDirectory();
        juce::StringArray tail { script.getFullPathName(), tmp_in.getFullPathName(), out.getFullPathName(),
                                 "rate=" + juce::String(rate_.load()), "slices=" + sl_arg, "bpm=" + juce::String(bpm, 3), "root=" + juce::String(rt) };
        std::vector<juce::StringArray> cands;
        if (py.isNotEmpty()) cands.push_back({ py });
        else { cands.push_back({ "py", "-3" }); cands.push_back({ "python" }); cands.push_back({ "python3" }); }
        juce::String log;
        for (auto head : cands) {
            head.addArray(tail);
            juce::ChildProcess proc;
            if (!proc.start(head)) continue;
            const bool done = proc.waitForProcessToFinish(120000);
            log = proc.readAllProcessOutput().trim();
            if (!done) { setStatus("script timed out"); return; }
            if (proc.getExitCode() == 0 && out.existsAsFile()) {
                setStatus("script ok -> " + out.getFileName());
                const std::lock_guard<std::mutex> lk(status_lock_);
                load_request_ = { out, true };
                return;
            }
            if (log.containsIgnoreCase("Python was not found") || log.isEmpty()) continue;
            break;
        }
        setStatus("script failed: " + (log.isEmpty() ? juce::String("no python found (set PYTHON path)") : log.substring(std::max(0, log.length() - 160))));
    });
}

void AlphaSamplerAudioProcessor::loadEdited(const juce::File& f)
{
    if (origin_path_.isEmpty()) origin_path_ = sample_path_;
    const auto keep = origin_path_;
    loadFile(f, true);
    origin_path_ = keep;
}

void AlphaSamplerAudioProcessor::revertEdit()
{
    if (origin_path_.isEmpty()) return;
    const juce::File o(origin_path_);
    if (!o.existsAsFile()) { setStatus("original not found"); return; }
    loadFile(o, true);
    origin_path_.clear();
}

// ---- recording ------------------------------------------------------------------
void AlphaSamplerAudioProcessor::startRecording(int source)
{
    if (rec_on_.load()) return;
    rec_rate_ = rate_.load();
    const size_t cap = static_cast<size_t>(rec_rate_) * 120u;
    rec_l_.assign(cap, 0.0f);
    rec_r_.assign(cap, 0.0f);
    rec_n_.store(0);
    rec_peak_.store(0.0f);
    rec_src_ = source;
    rec_on_.store(true, std::memory_order_release);
}

void AlphaSamplerAudioProcessor::record(const float* l, const float* r, int n) noexcept
{
    const size_t at = rec_n_.load(std::memory_order_relaxed);
    const size_t len = std::min(static_cast<size_t>(n), rec_l_.size() - at);
    float pk = rec_peak_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < len; ++i) {
        const float a = l ? l[i] : 0.0f, b = r ? r[i] : 0.0f;
        rec_l_[at + i] = a;
        rec_r_[at + i] = b;
        pk = std::max({ pk, std::abs(a), std::abs(b) });
    }
    rec_peak_.store(pk, std::memory_order_relaxed);
    rec_n_.store(at + len, std::memory_order_release);
    if (at + len >= rec_l_.size()) rec_on_.store(false, std::memory_order_release);
}

void AlphaSamplerAudioProcessor::stopRecording()
{
    if (!rec_on_.exchange(false)) return;
    juce::Thread::sleep(30);   // let an in-flight audio block finish writing
    const size_t n = rec_n_.load(std::memory_order_acquire);
    if (n < 64) { setStatus("nothing recorded"); return; }
    const auto dest = dataDir().getChildFile("Recordings").getChildFile("rec_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav");
    const bool silent = rec_peak_.load() < 1.0e-6f;
    auto l = std::make_shared<std::vector<float>>(rec_l_.begin(), rec_l_.begin() + static_cast<std::ptrdiff_t>(n));
    auto r = std::make_shared<std::vector<float>>(rec_r_.begin(), rec_r_.begin() + static_cast<std::ptrdiff_t>(n));
    const double rate = rec_rate_;
    setStatus("saving recording...");
    runJob([this, dest, l, r, rate, silent] {
        juce::AudioBuffer<float> b(2, static_cast<int>(l->size()));
        b.copyFrom(0, 0, l->data(), b.getNumSamples());
        b.copyFrom(1, 0, r->data(), b.getNumSamples());
        if (!write_wav(dest, b, rate)) { setStatus("recording write failed"); return; }
        setStatus(silent ? "recorded silence: is the input routed? (" + dest.getFileName() + ")" : "recorded " + dest.getFileName());
        const std::lock_guard<std::mutex> lk(status_lock_);
        load_request_ = { dest, false };
    });
}

} // namespace as
