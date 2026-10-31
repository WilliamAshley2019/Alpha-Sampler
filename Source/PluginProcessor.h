// PluginProcessor.h — Alpha Sampler.
//
// One header for the whole project. Declarations only; audio bodies live in
// Engine.cpp (resampler, filter, stretch, sample building, voices) and
// Stream.cpp (disk streaming); the plugin shell lives in PluginProcessor.cpp.

#pragma once

#if (defined(_MSVC_LANG) ? _MSVC_LANG : __cplusplus) < 202002L
 #error "Alpha Sampler needs C++20: set the C++ language standard to C++20 (Projucer: Exporter > C++ Language Standard; CMake does this already)."
#endif

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace as {
// Little/big endian byte readers shared by Engine.cpp and Stream.cpp.
inline uint16_t u16le(const uint8_t* b) noexcept { return static_cast<uint16_t>(b[0] | (b[1] << 8)); }
inline uint32_t u32le(const uint8_t* b) noexcept { return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24); }
inline uint16_t u16be(const uint8_t* b) noexcept { return static_cast<uint16_t>((b[0] << 8) | b[1]); }
inline uint32_t u32be(const uint8_t* b) noexcept { return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]); }
} // namespace as

// ============================================================================
// Numeric primitives.
// ============================================================================
namespace as::num {

inline constexpr float TINY = 5.9604645e-8f;

inline float s(double x) noexcept { return static_cast<float>(x); }
inline int64_t round_ties_even(double x) noexcept { return static_cast<int64_t>(std::nearbyint(x)); }
inline int64_t trunc(double x) noexcept { return static_cast<int64_t>(std::trunc(x)); }
inline float flush(float x) noexcept { return std::abs(x) < TINY ? 0.0f : x; }
inline double flush_le(double x) noexcept { return std::abs(x) <= static_cast<double>(TINY) ? 0.0 : x; }

inline float curve(float x) noexcept
{
    if (x == 1.0f) return 1.0f;
    const float p = x * static_cast<float>(std::log(11.0));
    volatile double e = std::exp(static_cast<double>(p));
    return static_cast<float>((e - 1.0) * static_cast<double>(0.1f));
}

inline float cell_gain(int32_t v) noexcept
{
    const float g = curve(std::abs(v * (1.0f / 256.0f)));
    return v < 0 ? -g : g;
}
inline float signed_curve128(int32_t v) noexcept
{
    const float g = curve(std::abs(v * (1.0f / 128.0f)));
    return v < 0 ? -g : g;
}

inline uint32_t lcg(uint32_t seed) noexcept { return seed * 0x08088405u + 1u; }

struct Expo
{
    float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f;
    static Expo of(float v) noexcept
    {
        if (v == 0.0f) return {};
        const double a = std::abs(static_cast<double>(v));
        return { v, s(1.0 / a), s(std::log(a + 1.0)) };
    }
    double eval(double u) const noexcept
    {
        if (c0 == 0.0f) return u;
        if (c0 > 0.0f) return 1.0 - ((std::exp((1.0 - u) * static_cast<double>(c2)) - 1.0) * static_cast<double>(c1));
        return ((std::exp(u * static_cast<double>(c2)) - 1.0) * static_cast<double>(c1));
    }
};

struct Reader
{
    const uint8_t* b = nullptr;
    size_t len = 0, at = 0;
    Reader() = default;
    Reader(const uint8_t* bytes, size_t length) noexcept : b(bytes), len(length) {}
    size_t left() const noexcept { return at <= len ? len - at : 0; }
    std::optional<std::string_view> bytes(size_t n)
    {
        if (at > len || n > len - at) return std::nullopt;
        std::string_view v(reinterpret_cast<const char*>(b) + at, n);
        at += n;
        return v;
    }
    std::optional<uint8_t> u8()
    {
        auto v = bytes(1);
        if (!v) return std::nullopt;
        return static_cast<uint8_t>(static_cast<unsigned char>((*v)[0]));
    }
    std::optional<int32_t> i32() { auto v = bytes(4); if (!v) return std::nullopt; int32_t x; std::memcpy(&x, (*v).data(), 4); return x; }
    std::optional<float> f32()   { auto v = bytes(4); if (!v) return std::nullopt; float x; std::memcpy(&x, (*v).data(), 4); return x; }
    std::optional<double> f64()  { auto v = bytes(8); if (!v) return std::nullopt; double x; std::memcpy(&x, (*v).data(), 8); return x; }
};

} // namespace as::num

// ============================================================================
// level — curve, knob positions, pan split.
// ============================================================================
namespace as::level {

inline constexpr float R12800 = 0x1.07AE14p-14f;
inline constexpr float R6400  = 0x1.0624DEp-13f;
inline constexpr float R65536 = 0x1p-16f;

inline float knob(int32_t value, float recip) noexcept { return static_cast<float>(value) * recip; }
inline float curve(float x) noexcept { return as::num::curve(x); }

enum class PanLaw { Circular, Triangular };

inline std::pair<float, float> pan_split(float level, float pan) noexcept
{
    const double p = (pan < -1.0f) ? -1.0 : (pan > 1.0f) ? 1.0 : static_cast<double>(pan);
    const double angle = (p + 1.0) * 3.14159265358979323846 / 4.0;
    const double l = static_cast<double>(level);
    return { static_cast<float>(l * std::cos(angle)), static_cast<float>(l * std::sin(angle)) };
}

inline std::pair<float, float> pan_split_law(float level, float pan, PanLaw law) noexcept
{
    if (law == PanLaw::Circular) return pan_split(level, pan);
    const double p = (pan < -1.0f) ? -1.0 : (pan > 1.0f) ? 1.0 : static_cast<double>(pan);
    const double centre = 3.14159265358979323846 / 4.0;
    const double l = static_cast<double>(level);
    return { static_cast<float>(l * std::cos(centre) * std::min(1.0, 1.0 - p)),
             static_cast<float>(l * std::sin(centre) * std::min(1.0, 1.0 + p)) };
}

inline float unity_at_centre(float level) noexcept
{
    return static_cast<float>(static_cast<double>(level) * 1.4142135623730951);
}

inline float audio_clip_level() noexcept
{
    return 1.41421356f / curve(knob(10000, R12800));
}

} // namespace as::level

// ============================================================================
// glide — per-sample gain ramps.
// ============================================================================
namespace as::glide {

inline float engine_limit(uint32_t rate) noexcept
{
    const uint32_t r = rate == 0 ? 1u : rate;
    const float k = static_cast<float>(44100.0 / static_cast<double>(r));
    return static_cast<float>(static_cast<double>(k) * 0.001);
}
inline float plugin_limit(uint32_t rate) noexcept
{
    const uint32_t r = rate == 0 ? 1u : rate;
    return static_cast<float>(44100.0 / static_cast<double>(r) * 0.001);
}

struct Ramp
{
    float value = 0.0f, step = 0.0f;
    inline float next() noexcept { const float v = value; value += step; return v; }
    bool is_flat() const noexcept { return step == 0.0f; }
    float first() const noexcept { return value; }
    float step_() const noexcept { return step; }
};

struct Glide
{
    float current = 0.0f;
    static Glide of(float v) noexcept { return Glide{ v }; }
    Ramp block(float target, int n, float limit) noexcept
    {
        const float start = current;
        if (start == target) return { start, 0.0f };
        const float nf = static_cast<float>(n < 1 ? 1 : n);
        float step = (target - start) / nf;
        float end;
        if (std::abs(step) <= limit) {
            end = target;
        } else {
            step = limit * ((target - start) > 0.0f ? 1.0f : -1.0f);
            const float e = start + step * nf;
            if (std::abs(e) < (1.0f / 16777216.0f)) { step = -start / nf; end = 0.0f; }
            else end = e;
        }
        current = end;
        return { start, step };
    }
};

} // namespace as::glide

// ============================================================================
// env — envelopes.
// ============================================================================
namespace as::env {

struct EnvelopeRecord
{
    bool enabled = false;
    int32_t delay = 100, attack = 100, hold = 100, decay = 100, release = 100;
    int32_t sustain = 128;
    int32_t amount = 0;
    int32_t attack_tension = 0, decay_tension = 0, release_tension = 0;
    int32_t lfo_delay = 100, lfo_attack = 20000, lfo_amount = 0, lfo_speed = 32950;
    int32_t lfo_shape = 0;
};

inline double knob_seconds(int32_t v) noexcept
{
    auto g = [](double x) { return std::pow(2.0, x / 6553.6); };
    return std::max(0.0, 0.063922 * (g(static_cast<double>(v)) - g(100.0)));
}
inline double fall_seconds(int32_t v) noexcept { return knob_seconds(v) + 0.0090; }

inline double tension_k(int32_t t) noexcept
{
    static constexpr double table[4][2] = { { 0.0, 0.0 }, { 64.0, 2.2 }, { 101.0, 4.85 }, { 128.0, 6.3 } };
    const double a = static_cast<double>(t < -128 ? -128 : t > 128 ? 128 : t);
    const double aa = std::abs(a);
    double k = table[3][1];
    for (int i = 0; i + 1 < 4; ++i) {
        if (aa <= table[i + 1][0]) {
            k = table[i][1] + (table[i + 1][1] - table[i][1]) * (aa - table[i][0]) / (table[i + 1][0] - table[i][0]);
            break;
        }
    }
    return t < 0 ? -k : k;
}

inline double shape(double u, double k) noexcept
{
    const double uu = (u < 0.0) ? 0.0 : (u > 1.0) ? 1.0 : u;
    if (std::abs(k) < 1e-6) return uu;
    return (1.0 - std::exp(-k * uu)) / (1.0 - std::exp(-k));
}

struct Params
{
    double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, release = 0.0, sustain = 0.0;
    double amount = 0.0;
    double att_k = 0.0, dec_k = 0.0, rel_k = 0.0;

    static std::optional<Params> from_record(const EnvelopeRecord& e) noexcept
    {
        if (!e.enabled) return std::nullopt;
        Params p;
        p.delay = knob_seconds(e.delay);
        p.attack = knob_seconds(e.attack);
        p.hold = knob_seconds(e.hold);
        p.decay = fall_seconds(e.decay);
        p.release = fall_seconds(e.release);
        p.sustain = static_cast<double>(e.sustain < 0 ? 0 : e.sustain > 128 ? 128 : e.sustain) / 128.0;
        p.amount = static_cast<double>(e.amount);
        p.att_k = tension_k(e.attack_tension);
        p.dec_k = e.decay > 100 ? tension_k(e.decay_tension) : 0.0;
        p.rel_k = e.release > 100 ? tension_k(e.release_tension) : 0.0;
        return p;
    }

    Params scaled(double f) const noexcept
    {
        Params p = *this;
        p.delay *= f; p.attack *= f; p.hold *= f; p.decay *= f; p.release *= f;
        return p;
    }

    double held(double t) const noexcept
    {
        if (t < delay) return 0.0;
        double a = t - delay;
        if (a < attack) return shape(a / attack, att_k);
        a -= attack;
        if (a < hold) return 1.0;
        a -= hold;
        const double span = decay * (1.0 - sustain);
        if (span > 0.0 && a < span) return 1.0 - (1.0 - sustain) * shape(a / span, -dec_k);
        return sustain;
    }

    std::optional<double> level(double t, std::optional<double> off) const noexcept
    {
        if (off && t >= *off) {
            const double from = held(*off);
            const double span = release * from;
            const double r = t - *off;
            if (span <= 0.0 || r >= span) return std::nullopt;
            return from * (1.0 - shape(r / span, -rel_k));
        }
        return held(t);
    }
};

} // namespace as::env

// ============================================================================
// laws — gain/pan/pitch and note names.
// ============================================================================
namespace as::laws {

using PanLaw = level::PanLaw;

inline double volume_curve(double x) noexcept { return (std::pow(11.0, x < 0.0 ? 0.0 : x) - 1.0) / 10.0; }
inline double channel_gain(double v) noexcept { return volume_curve(v); }
inline double velocity_gain(uint8_t v) noexcept { return volume_curve(static_cast<double>(v) / 100.0); }
inline double note_pan(uint8_t byte) noexcept { return (static_cast<double>(byte) - 64.0) / 64.0; }

inline std::pair<double, double> channel_pan(double p) noexcept
{
    const double q = (p < -1.0) ? -1.0 : (p > 1.0) ? 1.0 : p;
    const double a = (1.0 + q) * (3.14159265358979323846 / 4.0);
    return { std::cos(a), std::sin(a) };
}

inline std::pair<double, double> channel_pan_law(double p, PanLaw law) noexcept
{
    if (law == PanLaw::Circular) return channel_pan(p);
    const double q = (p < -1.0) ? -1.0 : (p > 1.0) ? 1.0 : p;
    const auto [l, r] = channel_pan(0.0);
    return { l * std::min(1.0, 1.0 - q), r * std::min(1.0, 1.0 + q) };
}

inline double playback_rate(uint16_t key, int32_t cents) noexcept
{
    return std::pow(2.0, (static_cast<double>(key) - 60.0) / 12.0 + static_cast<double>(cents) / 1200.0);
}
inline double cents_rate(double cents) noexcept { return std::pow(2.0, cents / 1200.0); }

inline float channel_pitch(double knob, int32_t fine, uint8_t root, std::optional<int32_t> main) noexcept
{
    const float p = static_cast<float>(knob + static_cast<double>(fine) - static_cast<double>(root) * 100.0);
    if (!main) return p;
    return p + static_cast<float>(*main);
}

inline int32_t auto_pitch_cents(double v) noexcept
{
    return static_cast<int32_t>(static_cast<int64_t>(std::nearbyint(9600.0 * v))) - 4800;
}
inline int32_t fine_pitch_cents(uint8_t fine) noexcept { return static_cast<int32_t>(fine) * 10 - 1200; }

inline uint32_t glide_ticks(float slide, uint16_t ppq) noexcept
{
    const float x = (slide < 0.0f) ? 0.0f : (slide > 1.0f) ? 1.0f : slide;
    const float e = static_cast<float>(std::exp(static_cast<double>(x) * std::log(27.0)) - 1.0);
    const float r32 = static_cast<float>(static_cast<double>(e) * 48.0 * (1.0 / 26.0));
    const auto r = static_cast<uint32_t>(std::nearbyint(static_cast<double>(r32)));
    return r * static_cast<uint32_t>(ppq / 4u) / 48u;
}

inline const char* const NOTE_NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

inline std::string note_name(int32_t key)
{
    const auto k = ((key % 12) + 12) % 12;
    const auto oct = (key >= 0) ? (key / 12) : ((key - 11) / 12);
    return std::string(NOTE_NAMES[k]) + std::to_string(oct);
}

inline int note_name_from(const std::string& s)
{
    auto t = s;
    for (auto& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto digit = t.find_first_of("0123456789-");
    if (digit == std::string::npos) return 60;
    const auto name = t.substr(0, digit);
    int idx = -1;
    for (int i = 0; i < 12; ++i) if (name == NOTE_NAMES[i]) { idx = i; break; }
    if (idx < 0) return 60;
    try { return std::stoi(t.substr(digit)) * 12 + idx; } catch (...) { return 60; }
}

} // namespace as::laws

// ============================================================================
// Sample.
// ============================================================================
namespace as {

struct Sample
{
    uint32_t rate = 0;
    std::vector<float> left, right;
    int frames() const noexcept { return static_cast<int>(left.size()); }
    bool empty() const noexcept { return left.empty(); }
};

// Peak summary of a sample for the editor's waveform display (min/max per column).
struct Peaks
{
    std::vector<float> lo, hi;
    bool empty() const noexcept { return lo.empty(); }
};

struct Spectro
{
    int cols = 0, bins = 0;
    std::vector<uint8_t> img;     // cols*bins, row 0 = lowest band, 0..255 = -90..0 dB
    std::vector<float> avg;       // per band 0..1
    std::vector<float> bright;    // per column 0..1 (log spectral centroid)
    float rms_db = -120.0f, dc = 0.0f;
    bool empty() const noexcept { return cols == 0; }
};

using Fetch = std::function<void(size_t pos, size_t n, float* mono)>;
std::shared_ptr<const Spectro> analyse(const Fetch& fetch, size_t frames, double rate);
std::vector<uint32_t> wav_cues(const juce::File& f);   // cue-point sample offsets, sorted
std::optional<double> detect_bpm(const std::vector<float>& mono, double rate, double& beats_out);

struct WaveView
{
    std::shared_ptr<const Spectro> ana;
    Peaks raw;                    // the file as loaded
    Peaks proc;                   // after the effects/stretch pipeline (what voices play)
    double rate = 0.0;            // sample rate of the processed sample
    double raw_frames = 0.0, proc_frames = 0.0;
    double loop_a = -1.0, loop_b = -1.0;   // loop points as fractions of `proc`, or -1
};

struct WavTags
{
    std::optional<uint8_t> unity_note;
    std::optional<uint32_t> beats;
    bool operator==(const WavTags& o) const noexcept { return unity_note == o.unity_note && beats == o.beats; }
    bool operator!=(const WavTags& o) const noexcept { return !(*this == o); }
};

WavTags wav_tags(const juce::File& path);
WavTags wav_tags_from_bytes(const void* data, size_t size);

struct SourceInfo
{
    bool int16_mono = false;
    std::optional<std::pair<uint32_t, uint32_t>> file_loop;
    uint32_t bits = 16;
};

std::pair<uint32_t, SourceInfo> source_info(const juce::File& path);

class SampleLibrary
{
public:
    SampleLibrary();
    ~SampleLibrary() = default;
    SampleLibrary(const SampleLibrary&) = delete;
    SampleLibrary& operator=(const SampleLibrary&) = delete;

    std::optional<std::pair<int64_t, uint32_t>> length(const juce::File& path);
    std::shared_ptr<const Sample> load(const juce::File& path) const;
    std::shared_ptr<const Sample> load_shared(const juce::File& path);
    bool is_held(const juce::File& path) const;
    juce::AudioFormatManager& formats() noexcept { return formats_; }

private:
    struct Entry
    {
        std::weak_ptr<const Sample> sample;
        juce::int64 size = 0;
        juce::Time mtime;
    };
    static std::optional<std::pair<juce::int64, juce::Time>> stamp(const juce::File& path);
    std::shared_ptr<const Sample> decode(const juce::File& path) const;

    mutable std::mutex lock_;
    mutable juce::AudioFormatManager formats_;
    std::unordered_map<juce::String, Entry> live_;
};

struct SampleFx
{
    uint16_t flags = 0;
    uint16_t boost = 0;
    uint16_t fade_out = 0, fade_in = 0;
    uint16_t stereo_delay = 2048;
    uint16_t pogo = 256;
    uint16_t feedback = 128;
    uint16_t cutoff = 1024, resonance = 0;
    uint16_t ring_mix = 0, ring_freq = 128;
    uint16_t reverb = 0, reverb_type = 1;
    bool ping_pong = false;
    bool remove_dc = false, normalize = false, polarity = false;
    uint32_t crossfade = 0, trim = 0;
    uint64_t start_bits = 0, length_bits = 0x3ff0000000000000ull;
    bool loop_follows_reverse = false;

    bool operator==(const SampleFx&) const = default;
    static constexpr uint16_t FADE_STEREO = 1, REVERSE = 2, CLIP = 4, SWAP = 256;
    double start() const noexcept { double v; std::memcpy(&v, &start_bits, 8); return v; }
    double length() const noexcept { double v; std::memcpy(&v, &length_bits, 8); return v; }
};

struct FxKey
{
    SampleFx fx;
    std::optional<std::pair<float, float>> knob_loop;
    uint32_t rate = 44100;
    bool stretch = false;
    float ratio = 1.0f, time = 0.0f, shift = 0.0f;

    bool operator==(const FxKey&) const = default;
    static FxKey neutral() noexcept { return {}; }
    double ratio_for(double secs) const noexcept
    {
        if (time > 0.0f && secs > 0.0) return static_cast<double>(time) / secs;
        return static_cast<double>(ratio);
    }
};

struct DiskKey
{
    bool keep = false, realtime = false;
    uint32_t start = 0;
    bool loop_on = false;
    double start_offset() const noexcept
    {
        float v; std::memcpy(&v, &start, 4);
        const double d = static_cast<double>(v);
        return d < 0.0 ? 0.0 : d > 1.0 ? 1.0 : d;
    }
    bool operator==(const DiskKey& o) const noexcept
    { return keep == o.keep && realtime == o.realtime && start == o.start && loop_on == o.loop_on; }
    bool operator!=(const DiskKey& o) const noexcept { return !(*this == o); }
};

struct Built
{
    Sample sample;
    std::optional<std::pair<size_t, size_t>> looped;
    double per_file_frame = 1.0;
};

Built build(const Sample& raw, const SourceInfo& src, const FxKey& k);
std::optional<std::optional<std::pair<size_t, size_t>>>
    as_is(const SourceInfo& src, const FxKey& k, uint32_t rate, size_t frames);

} // namespace as

// ============================================================================
// Resampler.
// ============================================================================
namespace as::resample {

enum class Quality { Table, Exact };

inline constexpr int N_MIN = 16;
inline constexpr int N_MAX = 512;

int half_width(double ratio) noexcept;
void weights(double f, double ratio, int n, Quality q, float* w) noexcept;

std::pair<float, float> read(const float* left, const float* right, int len,
                             double pos, double ratio, Quality q) noexcept;

std::pair<size_t, size_t> reach(int len, double pos, double ratio) noexcept;

// Same as read() but samples are fetched through `at(index)` (disk rings,
// loop-wrapped reads, ...). Header-only so the lambda inlines.
template <typename At>
inline std::pair<float, float> read_by_t(int len, double pos, double ratio, Quality q, At&& at) noexcept
{
    if (len == 0) return { 0.0f, 0.0f };
    const double k0d = std::max(0.0, pos);
    const double k0f = std::floor(k0d);
    const double f = k0d - k0f;
    const int64_t k0 = static_cast<int64_t>(k0f);
    if (f == 0.0 && std::abs(ratio) == 1.0) {
        const int i = static_cast<int>(std::min<int64_t>(k0, static_cast<int64_t>(len) - 1));
        return at(i);
    }
    const int n = half_width(ratio);
    alignas(64) float small[2 * N_MIN];
    alignas(64) float big[2 * N_MAX];
    float* w = (n == N_MIN) ? small : big;
    weights(f, ratio, n, q, w);
    const int64_t first = k0 - static_cast<int64_t>(n) + 1;
    float l = 0.0f, r = 0.0f;
    for (int t = 0; t < 2 * n; ++t) {
        const int64_t idx = std::clamp<int64_t>(first + t, 0, static_cast<int64_t>(len) - 1);
        const auto [a, b] = at(static_cast<int>(idx));
        l += a * w[t];
        r += b * w[t];
    }
    return { l, r };
}

} // namespace as::resample

// ============================================================================
// Disk streaming.
// ============================================================================
namespace as::stream {

inline constexpr size_t HEAD = 1u << 15;
inline constexpr size_t BEFORE = static_cast<size_t>(resample::N_MAX);
inline constexpr size_t RING = 1u << 16;
inline constexpr size_t CHUNK = 1u << 13;
inline constexpr size_t OVERLAP = 2 * BEFORE + 2;
inline constexpr size_t KEEP = 2 * BEFORE;

struct Feed
{
    int slot = -1;
    uint32_t req = 0;
    uint64_t start = 0;
    uint64_t released = 0;
    bool active() const noexcept { return slot >= 0; }
};

struct Layout
{
    uint32_t rate = 0;
    uint16_t channels = 0;
    size_t frames = 0;
    uint64_t data = 0;
    uint8_t kind = 0;
    bool big = false;

    static std::optional<Layout> of(const juce::File& path);
    size_t block() const noexcept;
    void read(juce::File& file, size_t at, float* left, float* right, size_t frames) const;
};

std::optional<std::pair<size_t, uint32_t>> length(const juce::File& path);

struct Held
{
    size_t start = 0;
    std::vector<float> left, right;
    size_t end() const noexcept { return start + left.size(); }
    std::pair<float, float> at(size_t x) const noexcept { return { left[x - start], right[x - start] }; }
};

class Disk;
class Played;

struct Summary
{
    static constexpr size_t POINTS = 1u << 18;
    size_t step = 1;
    std::vector<float> left, right;
    bool stereo = false;
};

Summary read_summary(const juce::File& path, const Layout& l);

class Disk
{
public:
    static std::shared_ptr<Disk> open(const juce::File& path, Layout l);
    ~Disk();

    const Layout& layout() const noexcept { return l_; }
    void wake() const;
    void read_frames(size_t start, float* left, float* right, size_t frames) const;
    std::shared_ptr<std::atomic<bool>> reader_alive() const noexcept { return alive_; }

    size_t rings() const noexcept { return ring_count_.load(); }
    size_t busy() const noexcept;
    size_t frames_read() const noexcept { return frames_read_.load(); }

    bool settled() const;
    size_t slots_count() const noexcept { return slots_.size(); }
    int take_slot();
    void release_slot(int i);

private:
    friend class Played;
    Disk(juce::File file, Layout l, std::shared_ptr<std::atomic<bool>> alive);

    struct Fill
    {
        uint32_t req = 0;
        size_t start = 0;
        size_t filled = 0;
        std::vector<uint8_t> bytes;
        std::vector<float> left, right;
    };
    struct Slot
    {
        std::atomic<bool> busy { false };
        std::atomic<uint64_t> want { 0 };
        std::atomic<uint64_t> have { 0 };
        std::atomic<uint64_t> used { 0 };
        std::mutex fill_lock;
        Fill fill;
        std::mutex ring_lock;
        std::vector<float> ring_left, ring_right;
    };

    Layout l_;
    mutable juce::File file_;
    mutable std::mutex file_lock_;
    mutable std::atomic<bool> closed_ { false };
    std::shared_ptr<std::atomic<bool>> alive_;
    std::thread reader_;
    std::mutex cv_lock_;
    mutable std::condition_variable cv_;
    std::vector<Slot> slots_;
    std::atomic<size_t> ring_count_ { 0 };
    std::atomic<size_t> frames_read_ { 0 };

    void run();
    bool fill_some(Slot& slot, Fill& f, size_t min);
    bool fill_to(Slot& slot, uint32_t req, size_t need);

    static uint64_t pack(uint32_t req, size_t x) noexcept
    { return (static_cast<uint64_t>(req) << 32) | (static_cast<uint64_t>(x) & 0xFFFFFFFFull); }
    static std::pair<uint32_t, size_t> unpack(uint64_t v) noexcept
    { return { static_cast<uint32_t>(v >> 32), static_cast<size_t>(v & 0xFFFFFFFFull) }; }
};

class Played
{
public:
    Played() = default;
    Played(std::shared_ptr<Disk> disk, const std::vector<size_t>& named, const Played* old);

    std::shared_ptr<Disk> disk() const noexcept { return disk_; }
    size_t frames() const noexcept;
    uint32_t rate() const noexcept;

    const std::vector<size_t>& points() const noexcept { return points_; }
    const std::vector<size_t>& named() const noexcept { return named_; }

    std::vector<size_t> missed() const;
    bool news() noexcept { return news_.exchange(false); }

    void start_at(Feed& feed, double pos, bool learn) const;
    size_t held_frames() const noexcept;
    bool read(Feed& feed, double pos, double ratio, int quality, bool wait, float& l, float& r) const;
    void release(Feed& feed) const;

private:
    friend class Disk;
    std::shared_ptr<Disk> disk_;
    std::vector<std::shared_ptr<Held>> held_;
    std::vector<size_t> named_, learned_, points_;
    mutable std::array<std::atomic<uint64_t>, 8> missed_ {};
    mutable std::atomic<bool> news_ { false };
};

} // namespace as::stream

// ============================================================================
// ChannelFilter.
// ============================================================================
namespace as {

class ChannelFilter
{
public:
    ChannelFilter(int32_t kind = 0, uint32_t rate = 44100);

    int32_t kind() const noexcept { return kind_; }
    float gain() const noexcept { return gain_; }

    void tick(int32_t kind, float x, float y, double nominal);
    void run(float* interleaved, int frames);
    float run_one(int side, uint64_t j, float x);
    void end(uint64_t len);

private:
    int32_t kind_ = 0;
    float k_ = 1.0f;
    double limit_ = 0.0;
    uint64_t done_ = 0;
    bool first_ = true, on_ = false;
    double c_ = 1.0, step_ = 0.0, r_ = 0.0;
    std::array<float, 5> coef_ { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float gain_ = 1.0f;
    double f_ = 0.0;
    std::array<double, 2> d_ { 1.0, 1.0 };
    std::array<std::array<std::array<double, 4>, 2>, 2> st_ {};

    void fast(float x, float y, double nominal);
    void biquad(float x, float y);
    void state_variable(float x, float y);
};

} // namespace as

// ============================================================================
// Time stretch (Signalsmith Stretch, MIT).
// ============================================================================
namespace as::stretch {

// Offline: stretch a whole stereo buffer to exactly `frames` output frames,
// optionally transposed by `transpose` (frequency multiplier).
std::pair<std::vector<float>, std::vector<float>> whole(
    const std::vector<float>& left, const std::vector<float>& right,
    uint32_t rate, size_t frames, double transpose);

// Real-time: one instance per voice, preallocated, no allocation in process().
class Stream
{
public:
    static constexpr size_t MAX_OUT = 128;

    explicit Stream(uint32_t rate);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    size_t delay() const noexcept;                   // output latency in frames
    size_t seek_length(double speed) const noexcept; // input frames needed by start()
    void start(const float* inL, const float* inR, size_t frames, double speed);
    void setTranspose(double factor);
    // outFrames <= MAX_OUT.
    void process(const float* inL, const float* inR, size_t inFrames,
                 float* outL, float* outR, size_t outFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace as::stretch

// ============================================================================
// Engine.
// ============================================================================
namespace as {

struct Lfo
{
    uint8_t shape = 0;   // 0 sine, 1 triangle, 2 pulse
    double delay = 0.0, attack = 0.0, amount = 0.0, hz = 0.0;
    bool global = false;

    bool active() const noexcept { return std::abs(amount) > 1e-4; }
    double wave(double phase) const noexcept;                 // phase in cycles
    double value(double t, double phase) const noexcept;      // includes amount and delay/attack ramp
};

struct Target
{
    std::optional<env::Params> env;
    double amount = 0.0;
    Lfo lfo;
};

constexpr int PAN = 0, VOL = 1, MODX = 2, MODY = 3, PITCH = 4;

struct Block
{
    int32_t root = 60;
    double cents = 0.0;            // fine tune + pitch bend
    double ts_cents = 0.0;         // realtime-stretch pitch shift
    double ts_ratio = 1.0;         // length multiplier
    std::optional<double> ts_time; // target length in seconds
    bool realtime = false;
    std::array<Target, 5> targets = {};
    uint8_t f_type = 0;
    float f_cut = 1.0f;
    float f_res = 0.0f;
    double start_offset = 0.0;
    bool loop_on = false;
    int32_t declick = 0;
    std::pair<int32_t, int32_t> zone { 0, 131 };
    bool add_key = false;
};

struct Declick
{
    double open_ms = 0.0, close_ms = 0.0;
    bool step_removal = false, sweep = false, bleed = false;
    float tension = 0.0f;
};

Declick declick(int32_t mode);
std::pair<float, float> pan_gains(double pan);

struct Voice;

class Engine
{
public:
    static constexpr int MAX_VOICES = 32;
    static constexpr int STRETCH_STREAMS = 12;

    explicit Engine(float rate);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Not real-time safe: allocate stretch streams and scratch buffers.
    void set_rate(float rate);
    void reset();

    // Real-time safe (called from the audio thread when a new Processed arrives).
    void set_sample(std::shared_ptr<const Sample> s);
    void set_looped(std::optional<std::pair<size_t, size_t>> lp);
    void set_disk(std::shared_ptr<const stream::Played> disk);
    void set_wait(bool wait) noexcept { wait_ = wait; }
    void set_quality(int quality) noexcept { quality_ = quality; }

    void note_on(uint8_t key, float velocity, const Block& b);
    void note_off(uint8_t key, const Block& b);
    void all_off();
    void render(float* left, float* right, int n, const Block& b);

    int active_voices() const noexcept;
    bool has_voices() const noexcept { return active_voices() > 0; }

    // Call from a non-audio thread now and then: frees samples retired by voices.
    void collect_garbage();

private:
    float rate_;
    std::shared_ptr<const Sample> sample_;
    std::shared_ptr<const stream::Played> disk_;
    std::optional<std::pair<size_t, size_t>> looped_;
    bool wait_ = false;
    int quality_ = 0;
    uint64_t serial_ = 0;
    double global_time_ = 0.0;

    std::vector<Voice> voices_;
    std::vector<std::unique_ptr<stretch::Stream>> streams_;
    std::vector<uint8_t> stream_used_;
    std::vector<float> in_l_, in_r_, out_l_, out_r_;

    std::mutex trash_lock_;
    std::vector<std::shared_ptr<const void>> trash_;

    void retire(std::shared_ptr<const void>&& p);
    void free_voice(Voice& v);
    Voice* allocate();
    void render_voice(Voice& v, float* left, float* right, int n, const Block& b);
};

} // namespace as

// ============================================================================
// The plugin.
// ============================================================================
namespace as {

struct Unit
{
    double base = 0.0, span = 0.0;
    float lo = 0.0f;
    double max = 0.0;
    uint32_t stored(float knob) const noexcept;
    float knob_of(uint32_t s) const noexcept;
};

struct EnvParams
{
    juce::AudioParameterBool* on = nullptr;
    juce::AudioParameterFloat* delay = nullptr;
    juce::AudioParameterFloat* att = nullptr;
    juce::AudioParameterFloat* hold = nullptr;
    juce::AudioParameterFloat* dec = nullptr;
    juce::AudioParameterFloat* sus = nullptr;
    juce::AudioParameterFloat* rel = nullptr;
    juce::AudioParameterFloat* att_t = nullptr;
    juce::AudioParameterFloat* dec_t = nullptr;
    juce::AudioParameterFloat* rel_t = nullptr;
    juce::AudioParameterFloat* amt = nullptr;
    juce::AudioParameterBool* tempo = nullptr;
    juce::AudioParameterChoice* lfo_shape = nullptr;
    juce::AudioParameterFloat* lfo_delay = nullptr;
    juce::AudioParameterFloat* lfo_att = nullptr;
    juce::AudioParameterFloat* lfo_amt = nullptr;
    juce::AudioParameterFloat* lfo_speed = nullptr;
    juce::AudioParameterBool* lfo_tempo = nullptr;
    juce::AudioParameterBool* lfo_global = nullptr;
};

struct FxKnobs
{
    bool fade_stereo = false, reverse = false, clip = false, swap = false;
    float boost = 0.0f, fade_out = 0.0f, fade_in = 0.0f, stereo_delay = 0.0f, pogo = 0.0f;
    float eq = 0.0f, cutoff = 1.0f, resonance = 0.0f, ring_mix = 0.0f, ring_freq = 0.5f;
    float reverb = 0.0f;
    int reverb_type = 1;
    bool ping_pong = false, remove_dc = false, normalize = false, polarity = false;
    float crossfade = 0.0f, trim = 0.0f, start = 0.0f, length = 1.0f;
    bool loop_follows = true;
};

SampleFx fx_from_knobs(const FxKnobs& k, const SampleFx* file);
std::optional<SampleFx> fx_from_text(const std::string& t);
std::string fx_text(const SampleFx& f);

class AlphaSamplerAudioProcessor final : public juce::AudioProcessor
{
public:
    AlphaSamplerAudioProcessor();
    ~AlphaSamplerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Alpha Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& apvts() noexcept { return apvts_; }

    juce::String samplePath() const { return sample_path_; }
    juce::String fileSettings() const { return file_settings_; }
    void setFileSettings(const juce::String& s) { file_settings_ = s; }

    // Message-thread API (decoding happens on the worker thread).
    void loadFile(const juce::File& path, bool keep_slices = false);
    void clearFile();
    void preview(uint8_t key, float vel, bool on);

    bool canUndo() const { return !undo_stack_.empty(); }
    bool canRedo() const { return !redo_stack_.empty(); }
    void undoStep() { step_history(undo_stack_, redo_stack_); }
    void redoStep() { step_history(redo_stack_, undo_stack_); }
    static int factoryCount();
    static juce::String factoryName(int i);
    void applyFactory(int i);
    void initPatch();
    juce::File presetDir() const;
    bool saveUserPreset(const juce::String& relName, bool with_sample);
    bool loadUserPreset(const juce::File& f);
    juce::String presetName() const { return preset_name_; }
    // ---- slices: start markers (fractions of the processed sample), note = slice_base + index ----
    static constexpr int MAX_SLICES = 64;
    std::vector<float> slices() const;
    void setSlices(std::vector<float> v);
    void autoSlice(int kind, int arg);   // 0 equal parts, 1 transients (arg = sensitivity 0..100), 2 whole beats
    int sliceBase() const { return slice_base->getIndex(); }

    // ---- background jobs, export, scripts, recording ----
    void runJob(std::function<void()> f);
    juce::String statusText() const;
    void setStatus(const juce::String& s);
    void exportAudio(const juce::File& dest, int mode, bool load_after, bool markers = false);   // 0 processed sample, 1 played note, 2 slices into folder dest
    void runScript(const juce::File& script);
    juce::File dragFile(int slice);   // slice < 0: whole processed sample with cue markers; message thread, synchronous
    void revertEdit();
    void loadEdited(const juce::File& f);
    std::optional<std::pair<juce::File, bool>> takeLoadRequest();   // bool: result of an edit (original stays revertible)
    juce::File dataDir() const;
    juce::String pythonPath() const { return python_path_; }
    void setPythonPath(const juce::String& s) { python_path_ = s; }
    juce::String originPath() const { return origin_path_; }
    void startRecording(int source);   // 0 own output, 1 input bus
    void stopRecording();
    bool recording() const { return rec_on_.load(); }
    float recSeconds() const { return static_cast<float>(rec_n_.load()) / static_cast<float>(std::max<uint32_t>(1, rec_rate_)); }
    float recPeak() const { return rec_peak_.load(); }

    void detectTempo();
    void fitToHost();
    double hostTempo() const { return tempo_value(); }
    std::atomic<float> bpm_found { 0.0f };
    std::atomic<float> beats_found { 0.0f };
    std::atomic<int> bpm_state { 0 };   // 0 idle, 1 working, 2 failed

    // Read by the editor.
    std::shared_ptr<std::atomic<uint32_t>> frames;
    std::atomic<float> meter_l { 0.0f }, meter_r { 0.0f };
    std::atomic<int> voices_active { 0 };
    std::atomic<int> load_state { 0 }; // 0 idle, 1 working, 2 failed

    // Waveform summary, rebuilt on the worker thread whenever the sample or its effects change.
    // wave_serial increments on every change; waveView() is safe from the message thread.
    std::atomic<uint32_t> wave_serial { 0 };
    std::shared_ptr<const WaveView> waveView() const;

private:
    struct Worker;

    struct Processed
    {
        uint64_t version = 0, serial = 0;
        FxKey key;
        DiskKey disk_key;
        std::shared_ptr<const Sample> sample;
        std::shared_ptr<stream::Played> disk;
        std::optional<std::pair<size_t, size_t>> looped;
        double file_frames = 0.0;
    };

    // ---- parameters (declared BEFORE apvts_: makeLayout() fills them) ----
    juce::AudioParameterBool* keep_disk = nullptr;
    juce::AudioParameterBool* resample = nullptr;
    juce::AudioParameterBool* load_regions = nullptr;
    juce::AudioParameterBool* load_slices = nullptr;
    juce::AudioParameterChoice* declick_mode = nullptr;
    juce::AudioParameterFloat* start_offset = nullptr;
    juce::AudioParameterFloat* loop_start = nullptr;
    juce::AudioParameterFloat* loop_end = nullptr;
    juce::AudioParameterBool* loop_use = nullptr;
    juce::AudioParameterBool* loop_pingpong = nullptr;
    juce::AudioParameterFloat* ts_pitch = nullptr;
    juce::AudioParameterFloat* ts_mul = nullptr;
    juce::AudioParameterFloat* ts_time = nullptr;
    juce::AudioParameterChoice* ts_mode = nullptr;
    juce::AudioParameterBool* fx_dc = nullptr;
    juce::AudioParameterBool* fx_normalize = nullptr;
    juce::AudioParameterBool* fx_reverse = nullptr;
    juce::AudioParameterBool* fx_polarity = nullptr;
    juce::AudioParameterBool* fx_fade_stereo = nullptr;
    juce::AudioParameterBool* fx_swap = nullptr;
    juce::AudioParameterFloat* fx_start = nullptr;
    juce::AudioParameterFloat* fx_length = nullptr;
    juce::AudioParameterFloat* fx_in = nullptr;
    juce::AudioParameterFloat* fx_out = nullptr;
    juce::AudioParameterFloat* fx_crossfade = nullptr;
    juce::AudioParameterFloat* fx_trim = nullptr;
    juce::AudioParameterFloat* fx_boost = nullptr;
    juce::AudioParameterFloat* fx_eq = nullptr;
    juce::AudioParameterFloat* fx_rm_mix = nullptr;
    juce::AudioParameterFloat* fx_rm_freq = nullptr;
    juce::AudioParameterFloat* fx_cut = nullptr;
    juce::AudioParameterFloat* fx_res = nullptr;
    juce::AudioParameterBool* fx_clip = nullptr;
    juce::AudioParameterChoice* fx_reverb_type = nullptr;
    juce::AudioParameterFloat* fx_reverb = nullptr;
    juce::AudioParameterFloat* fx_sdelay = nullptr;
    juce::AudioParameterFloat* fx_pogo = nullptr;
    juce::AudioParameterBool* fx_loop_follows = nullptr;
    std::array<EnvParams, 5> env {};
    juce::AudioParameterFloat* f_cut = nullptr;
    juce::AudioParameterFloat* f_res = nullptr;
    juce::AudioParameterChoice* f_type = nullptr;
    juce::AudioParameterChoice* root = nullptr;
    juce::AudioParameterChoice* zone_lo = nullptr;
    juce::AudioParameterChoice* zone_hi = nullptr;
    juce::AudioParameterInt* fine = nullptr;
    juce::AudioParameterBool* main_pitch = nullptr;
    juce::AudioParameterBool* add_key = nullptr;
    juce::AudioParameterBool* slice_on = nullptr;
    juce::AudioParameterChoice* slice_base = nullptr;
    std::vector<juce::RangedAudioParameter*> signature_params_;

    juce::String file_settings_;
    juce::String sample_path_;
    juce::String preset_name_, origin_path_, python_path_;
    std::array<std::atomic<float>, MAX_SLICES> slice_pos_ {};
    std::atomic<int> slice_n_ { 0 };
    struct Off { uint64_t at; uint8_t key; };
    std::array<Off, 64> offs_ {};
    int noff_ = 0;
    uint64_t clock_ = 0;
    std::mutex jobs_lock_;
    std::vector<std::function<void()>> jobs_;
    mutable std::mutex status_lock_;
    juce::String status_;
    std::optional<std::pair<juce::File, bool>> load_request_;
    std::vector<float> rec_l_, rec_r_;
    std::atomic<size_t> rec_n_ { 0 };
    std::atomic<bool> rec_on_ { false };
    std::atomic<float> rec_peak_ { 0.0f };
    int rec_src_ = 0;
    uint32_t rec_rate_ = 44100;
    bool processed_buffer(juce::AudioBuffer<float>& out, double& rate);
    void trigger(uint8_t note, float vel, const Block& b, int at);
    int slice_index(int note) const;
    std::vector<uint32_t> cue_frames(int total) const;
    void record(const float* l, const float* r, int n) noexcept;
    void render_note(const juce::AudioBuffer<float>& src, double rate, juce::AudioBuffer<float>& out);
    std::atomic<int64_t> sample_mtime_ { 0 };
    std::atomic<bool> tempo_req_ { false };
    void resetParams();
    using Snap = std::vector<float>;
    std::vector<Snap> undo_stack_, redo_stack_;
    bool hist_suspend_ = false;
    struct HistListener;
    std::unique_ptr<HistListener> hist_;
    Snap snapshot() const;
    void push_snapshot();
    void step_history(std::vector<Snap>& from, std::vector<Snap>& to);
    void apply_state(const juce::ValueTree& vt, bool load_sample);
    void detect_tempo_now();

    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    // ---- parameter reads (any thread; atomics only) ----
    Block make_block(double tempo, double bend) const;
    FxKey fx_key(double tempo, uint32_t rate) const;
    DiskKey disk_key() const;
    SampleFx sample_fx() const;
    FxKnobs fx_knobs() const;
    std::optional<SampleFx> file_fx() const;
    std::optional<double> ts_seconds(double tempo) const;
    Target envelope_to_target(const EnvParams& e, double tempo) const;
    uint64_t signature() const;

    // ---- shared sample state (worker thread + message thread) ----
    SampleLibrary library_;
    std::mutex work_lock_;            // serialises load()/update()
    std::mutex state_lock_;           // guards the fields below
    std::shared_ptr<Sample> raw_;
    SourceInfo source_;
    std::optional<stream::Layout> layout_;
    std::string loaded_path_;
    std::shared_ptr<const Processed> processed_;
    std::atomic<uint64_t> raw_version_ { 0 };
    uint64_t serial_ = 0;

    mutable std::mutex wave_lock_;    // message thread <-> worker; never touched by the audio thread
    std::shared_ptr<const WaveView> wave_;
    std::shared_ptr<const Peaks> raw_peaks_;      // work_lock_
    uint64_t raw_peaks_version_ = ~uint64_t { 0 };
    std::shared_ptr<const Spectro> ana_cache_;   // work_lock_
    uint64_t ana_version_ = ~uint64_t { 0 };
    void set_wave(std::shared_ptr<const WaveView> w);
    std::shared_ptr<const Peaks> raw_peaks_for(uint64_t version, const juce::File& path, const Sample* raw);

    std::mutex published_lock_;       // audio thread only try_locks this
    std::shared_ptr<const Processed> published_;

    std::atomic<uint32_t> rate_ { 44100 };
    std::atomic<uint64_t> tempo_bits_ { std::bit_cast<uint64_t>(130.0) };

    std::mutex previews_lock_;
    std::vector<std::pair<uint8_t, float>> previews_;

    void load(const juce::File& path);
    void clear();
    void update();
    void update_impl();
    void publish(std::shared_ptr<const Processed> p);
    std::optional<std::optional<std::pair<size_t, size_t>>>
        streams(const SourceInfo& src, const stream::Layout& l) const;
    void take(const Processed& p);

    void set_tempo(double t) { if (t > 0.0) tempo_bits_.store(std::bit_cast<uint64_t>(t)); }
    double tempo_value() const { return std::bit_cast<double>(tempo_bits_.load()); }

    // ---- audio-thread state ----
    juce::AudioProcessorValueTreeState apvts_;
    Engine engine_;
    std::unique_ptr<Worker> worker_;
    uint64_t last_signature_ = 0;
    uint64_t current_serial_ = 0;
    double bend_ = 0.0;

    friend class AlphaSamplerAudioProcessorEditor;
};

} // namespace as
