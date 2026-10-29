// Engine.cpp — Alpha Sampler audio core.
//
// Resampler, channel filter, time stretch, sample library / build pipeline and
// the polyphonic voice engine. Depends on juce_core and juce_audio_formats.

#include "PluginProcessor.h"

#include "../third_party/signalsmith-stretch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

// ============================================================================
// resample — windowed sinc, variable width, table / exact paths.
// ============================================================================
namespace as::resample {
namespace {

constexpr double PI_D = 3.14159265358979323846;

constexpr int PHASES = 1024;
constexpr int SINC_RES = 1024;
constexpr int WIN_RES = 4096;
constexpr int SINC_SPAN = 17;

struct Tables
{
    std::vector<float> poly;
    std::vector<float> sinc;
    std::vector<float> win;
};

inline double window(double v) noexcept
{
    const double x = PI_D * v;
    return 0.34 + 0.5 * std::cos(x) + 0.16 * std::cos(2.0 * x);
}

inline double weight(double u, double a, int n) noexcept
{
    const double s = (u == 0.0) ? (a / PI_D) : (std::sin(a * u) / (PI_D * u));
    return s * window(u / static_cast<double>(n));
}

const Tables& tables() noexcept
{
    static Tables t;
    static std::once_flag once;
    std::call_once(once, [] {
        t.poly.resize(static_cast<size_t>(PHASES + 1) * 2 * N_MIN);
        for (int j = 0; j <= PHASES; ++j) {
            const double f = static_cast<double>(j) / static_cast<double>(PHASES);
            float* row = t.poly.data() + static_cast<size_t>(j) * 2 * N_MIN;
            for (int tap = 0; tap < 2 * N_MIN; ++tap) {
                row[tap] = static_cast<float>(weight(f + static_cast<double>(N_MIN - 1) - static_cast<double>(tap), PI_D, N_MIN));
            }
        }
        t.sinc.resize(static_cast<size_t>(SINC_SPAN * SINC_RES + 2));
        for (int i = 0; i <= SINC_SPAN * SINC_RES + 1; ++i) {
            const double x = static_cast<double>(i) / static_cast<double>(SINC_RES);
            if (x == 0.0) t.sinc[static_cast<size_t>(i)] = 1.0f;
            else {
                const double px = PI_D * x;
                t.sinc[static_cast<size_t>(i)] = static_cast<float>(std::sin(px) / px);
            }
        }
        t.win.resize(static_cast<size_t>(WIN_RES + 2));
        for (int i = 0; i <= WIN_RES + 1; ++i) {
            const double v = std::min(1.0, static_cast<double>(i) / static_cast<double>(WIN_RES));
            t.win[static_cast<size_t>(i)] = static_cast<float>(window(v));
        }
    });
    return t;
}

inline float lerp(const std::vector<float>& table, float x) noexcept
{
    const auto i = static_cast<size_t>(x < 0.0f ? 0 : x);
    if (i + 1 >= table.size()) return 0.0f;
    const float a = table[i];
    const float b = table[i + 1];
    const float frac = x - static_cast<float>(i);
    return a + (b - a) * frac;
}

} // namespace

int half_width(double ratio) noexcept
{
    const double r = std::abs(ratio);
    const double rr = std::nearbyint(std::min(r, 1e6));
    const int n = static_cast<int>((static_cast<int64_t>(rr) + 3) / 4 * 4);
    if (n < N_MIN) return N_MIN;
    if (n > N_MAX) return N_MAX;
    return n;
}

void weights(double f, double ratio, int n, Quality q, float* w) noexcept
{
    const double r = std::abs(ratio);

    if (q == Quality::Table && n == N_MIN && r <= 1.0) {
        const Tables& t = tables();
        const double x = f * static_cast<double>(PHASES);
        const int j = std::min(static_cast<int>(x), PHASES - 1);
        const float frac = static_cast<float>(x - static_cast<double>(j));
        const float* a = t.poly.data() + static_cast<size_t>(j) * 2 * N_MIN;
        const float* b = a + 2 * N_MIN;
        for (int i = 0; i < 2 * N_MIN; ++i) w[i] = a[i] + (b[i] - a[i]) * frac;
        return;
    }

    if (q == Quality::Table) {
        const Tables& t = tables();
        const double rmax = std::max(r, 1.0);
        const float su = static_cast<float>(static_cast<double>(SINC_RES) / rmax);
        const float wu = static_cast<float>(static_cast<double>(WIN_RES) / static_cast<double>(n));
        const float scale = static_cast<float>(1.0 / rmax);
        const float u0 = static_cast<float>(f + static_cast<double>(n - 1));
        for (int i = 0; i < 2 * n; ++i) {
            const float u = std::abs(u0 - static_cast<float>(i));
            w[i] = lerp(t.sinc, u * su) * scale * lerp(t.win, u * wu);
        }
        return;
    }

    const double a = (r > 1.0) ? (PI_D / r) : PI_D;
    const double b = PI_D / static_cast<double>(n);
    const double u0 = f + static_cast<double>(n - 1);

    double s, c;
    { const double v = a * u0; s = std::sin(v); c = std::cos(v); }
    double hs, h;
    { const double v = b * u0; hs = std::sin(v); h = std::cos(v); }
    double sa, ca;
    { sa = std::sin(a); ca = std::cos(a); }
    double sb, cb;
    { sb = std::sin(b); cb = std::cos(b); }

    for (int i = 0; i < 2 * n; ++i) {
        const double u = u0 - static_cast<double>(i);
        double sinc;
        const double au = std::abs(u);
        if (au == 0.0) sinc = a / PI_D;
        else if (au < 1.0) sinc = std::sin(a * u) / (PI_D * u);
        else sinc = s / (PI_D * u);
        w[i] = static_cast<float>(sinc * (0.18 + 0.5 * h + 0.32 * h * h));
        { const double ns = s * ca - c * sa; const double nc = c * ca + s * sa; s = ns; c = nc; }
        { const double nhs = hs * cb - h * sb; const double nh = h * cb + hs * sb; hs = nhs; h = nh; }
    }
}

std::pair<float, float> read(const float* left, const float* right, int len,
                             double pos, double ratio, Quality q) noexcept
{
    if (len == 0) return { 0.0f, 0.0f };
    const double k0d = std::max(0.0, pos);
    const double k0f = std::floor(k0d);
    const double f = k0d - k0f;
    const int64_t k0 = static_cast<int64_t>(k0f);
    if (f == 0.0 && std::abs(ratio) == 1.0) {
        const int i = static_cast<int>(std::min<int64_t>(k0, static_cast<int64_t>(len) - 1));
        return { left[i], right[i] };
    }
    const int n = half_width(ratio);
    alignas(64) float small[2 * N_MIN];
    alignas(64) float big[2 * N_MAX];
    float* w = (n == N_MIN) ? small : big;
    weights(f, ratio, n, q, w);
    const int64_t first = k0 - static_cast<int64_t>(n) + 1;
    float l = 0.0f, r = 0.0f;
    if (first >= 0 && first + 2 * n <= static_cast<int64_t>(len)) {
        const auto* ls = left + static_cast<size_t>(first);
        const auto* rs = right + static_cast<size_t>(first);
        for (int t = 0; t < 2 * n; ++t) { l += ls[t] * w[t]; r += rs[t] * w[t]; }
    } else {
        const int64_t last = static_cast<int64_t>(len) - 1;
        for (int t = 0; t < 2 * n; ++t) {
            const int64_t idx = std::clamp<int64_t>(first + t, 0, last);
            l += left[idx] * w[t];
            r += right[idx] * w[t];
        }
    }
    return { l, r };
}

std::pair<size_t, size_t> reach(int len, double pos, double ratio) noexcept
{
    if (len == 0) return { 0, 0 };
    const double k0d = std::max(0.0, pos);
    const double k0f = std::floor(k0d);
    const double f = k0d - k0f;
    const int64_t last = static_cast<int64_t>(len) - 1;
    const int64_t k0 = static_cast<int64_t>(k0f);
    if (f == 0.0 && std::abs(ratio) == 1.0) {
        const int64_t i = std::min<int64_t>(k0, last);
        return { static_cast<size_t>(i), static_cast<size_t>(i) };
    }
    const int64_t n = static_cast<int64_t>(half_width(ratio));
    const int64_t lo = std::clamp<int64_t>(k0 - n + 1, 0, last);
    const int64_t hi = std::clamp<int64_t>(k0 + n, 0, last);
    return { static_cast<size_t>(lo), static_cast<size_t>(hi) };
}

} // namespace as::resample

// ============================================================================
// ChannelFilter.
// ============================================================================
namespace as {
namespace {

constexpr double TINY_D = 1.0 / 16777216.0;
constexpr double PI_D = 3.14159265358979323846;

inline double e(float z, float c) noexcept
{
    return std::exp(static_cast<double>(z * c)) - 1.0;
}

} // namespace

ChannelFilter::ChannelFilter(int32_t kind, uint32_t rate)
{
    kind_ = (kind >= 0 && kind <= 7) ? kind : 0;
    k_ = static_cast<float>(44100.0 / static_cast<double>(rate == 0 ? 1u : rate));
    limit_ = static_cast<double>(glide::engine_limit(rate));
}

void ChannelFilter::fast(float x, float y, double nominal)
{
    const float ln601 = static_cast<float>(std::log(601.0));
    const float sixth = static_cast<float>(1.0 / 600.0);
    const double target = (x <= 0.0f)
        ? 0.0
        : (x < 1.0f)
            ? std::min(1.0, static_cast<double>(static_cast<float>(e(x, ln601) * sixth)) * static_cast<double>(k_))
            : 1.0;
    r_ = (y > 0.0f)
        ? 1.0 - static_cast<double>(static_cast<float>(e(static_cast<float>(1.0 - 0.625 * std::min(y, 1.0f)), ln601) * sixth))
        : 0.0;
    if (!on_ && (target != 1.0 || r_ != 0.0)) { on_ = true; st_ = {}; }
    if (first_) { c_ = target; step_ = 0.0; }
    else {
        const double nominal_d = nominal < 1.0 ? 1.0 : nominal;
        step_ = std::clamp((target - c_) / nominal_d, -limit_, limit_);
    }
}

void ChannelFilter::biquad(float x, float y)
{
    const double k = static_cast<double>(k_);
    const float ln51 = static_cast<float>(std::log(51.0));
    float w = static_cast<float>((e(x, ln51) * static_cast<double>(static_cast<float>(0.0596f)) + 0.02) * k);
    w = std::max(w, static_cast<float>(0.02 * k));
    w = std::min(w, 3.0f);
    const float q = (y > 0.0f)
        ? static_cast<float>(e(std::min(y, 1.0f), static_cast<float>(0.625 * std::log(51.0))) * static_cast<double>(static_cast<float>(0.4f)) + 0.1)
        : 0.1f;
    const float sn = static_cast<float>(std::sin(static_cast<double>(w)));
    const float cs = static_cast<float>(std::cos(static_cast<double>(w)));
    const float alpha = sn / (2.0f * q);
    const double n = 1.0 / (static_cast<double>(alpha) + 1.0);
    const float a1 = static_cast<float>(n * static_cast<double>(cs) * -2.0);
    const float a2 = static_cast<float>(n * (1.0 - static_cast<double>(alpha)));
    switch (kind_) {
        case 2: {
            const float b0 = static_cast<float>(n * static_cast<double>(alpha));
            coef_ = { b0, 0.0f, -b0, a1, a2 };
        } break;
        case 3: {
            const float b1 = static_cast<float>(-(n * (static_cast<double>(cs) + 1.0)));
            coef_ = { -b1 * 0.5f, b1, -b1 * 0.5f, a1, a2 };
        } break;
        case 4: {
            const float b0 = static_cast<float>(n);
            coef_ = { b0, a1, b0, a1, a2 };
        } break;
        default: {
            const float b1 = static_cast<float>(n * (1.0 - static_cast<double>(cs)));
            coef_ = { b1 * 0.5f, b1, b1 * 0.5f, a1, a2 };
        } break;
    }
    gain_ = (kind_ == 5)
        ? static_cast<float>(1.0 - static_cast<double>(q) * (3.2 - static_cast<double>(w)) * 0.046875)
        : 1.0f;
}

void ChannelFilter::state_variable(float x, float y)
{
    f_ = (x > 0.0f)
        ? static_cast<double>(std::min(static_cast<float>(std::exp(static_cast<double>(x) * std::log(26.0)) - 1.0) * 0.04f * k_, 1.0f))
        : 0.0;
    const double d = (y <= 0.0f) ? 1.0 : (y < 1.0f) ? static_cast<double>(1.0 - 0.8 * static_cast<double>(y)) : 0.2;
    d_[0] = d;
    d_[1] = static_cast<double>(static_cast<float>(d + (1.0 - d) * 0.25));
}

void ChannelFilter::tick(int32_t kind, float x, float y, double nominal)
{
    const int32_t k = (kind >= 0 && kind <= 7) ? kind : 0;
    if (k != kind_) { kind_ = k; st_ = {}; gain_ = 1.0f; }
    if (!first_) {
        c_ = static_cast<double>(static_cast<float>(c_ + step_ * static_cast<double>(done_)));
        if (kind_ == 6 || kind_ == 7) {
            for (auto& stage : st_) for (auto& ch : stage) for (size_t i = 0; i < 2; ++i) {
                const double s = static_cast<double>(static_cast<float>(ch[i]));
                ch[i] = (std::abs(s) < TINY_D) ? 0.0 : s;
            }
        }
    }
    done_ = 0;
    switch (kind_) {
        case 1: case 2: case 3: case 4: case 5: biquad(x, y); break;
        case 6: case 7: state_variable(x, y); break;
        default: fast(x, y, nominal); break;
    }
    first_ = false;
}

float ChannelFilter::run_one(int side, uint64_t j, float x)
{
    const size_t s = static_cast<size_t>(side & 1);
    switch (kind_) {
        case 1: case 2: case 3: case 4: case 5: {
            const auto [b0, b1, b2, a1, a2] = coef_;
            const int stages = (kind_ == 5) ? 2 : 1;
            float y = x;
            for (int stage = 0; stage < stages; ++stage) {
                auto& m = st_[static_cast<size_t>(stage)][s];
                const float out = static_cast<float>(static_cast<double>(y) * static_cast<double>(b0)
                    + (((m[1] * static_cast<double>(b2) + m[0] * static_cast<double>(b1))
                        - m[2] * static_cast<double>(a1)) - m[3] * static_cast<double>(a2)));
                m = { static_cast<double>(y), m[0], static_cast<double>(out), m[2] };
                y = out;
            }
            return y;
        }
        case 6: case 7: {
            const int stages = (kind_ == 7) ? 2 : 1;
            float y = x;
            for (int stage = 0; stage < stages; ++stage) {
                const double d = d_[static_cast<size_t>(stage)];
                auto& m = st_[static_cast<size_t>(stage)][s];
                m[0] += m[1] * f_;
                m[1] += (static_cast<double>(y) - (m[1] * d + m[0])) * f_;
                y = static_cast<float>(m[0]);
            }
            return y;
        }
        default: {
            if (!on_) return x;
            const double c = c_ + static_cast<double>(done_ + j) * step_;
            auto& m = st_[0][s];
            m[0] += (static_cast<double>(x) - m[1]) * c;
            m[1] += m[0];
            m[0] *= r_;
            return static_cast<float>(m[1]);
        }
    }
}

void ChannelFilter::run(float* buf, int frames)
{
    for (int i = 0; i < frames; ++i) {
        buf[2 * i] = run_one(0, done_ + static_cast<uint64_t>(i), buf[2 * i]);
        buf[2 * i + 1] = run_one(1, done_ + static_cast<uint64_t>(i), buf[2 * i + 1]);
    }
    done_ += static_cast<uint64_t>(frames);
}

void ChannelFilter::end(uint64_t len)
{
    switch (kind_) {
        case 1: case 2: case 3: case 4: case 5: break;
        case 6: case 7:
            for (auto& stage : st_) for (auto& ch : stage) for (size_t i = 0; i < 2; ++i) {
                const double s = static_cast<double>(static_cast<float>(ch[i]));
                ch[i] = (std::abs(s) < TINY_D) ? 0.0 : s;
            }
            break;
        default:
            if (c_ != 0.0) c_ = static_cast<double>(static_cast<float>(c_ + step_ * static_cast<double>(len)));
            break;
    }
    done_ = 0;
}

} // namespace as
// ============================================================================
// stretch — Signalsmith Stretch wrapper (planar buffers).
// ============================================================================
namespace as::stretch {

std::pair<std::vector<float>, std::vector<float>> whole(
    const std::vector<float>& left, const std::vector<float>& right,
    uint32_t rate, size_t frames, double transpose)
{
    const size_t n = std::min(left.size(), right.size());
    if (frames == 0 || n == 0)
        return { std::vector<float>(frames, 0.0f), std::vector<float>(frames, 0.0f) };

    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault(2, static_cast<float>(rate == 0 ? 1u : rate));
    st.setTransposeFactor(static_cast<float>(transpose));

    std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
    const float* in[2] = { left.data(), right.data() };
    float* out[2] = { l.data(), r.data() };

    const double speed = static_cast<double>(n) / static_cast<double>(frames);
    const size_t need = static_cast<size_t>(st.inputLatency() + std::ceil(speed * st.outputLatency())) + 2;
    if (n >= need && frames > static_cast<size_t>(st.outputLatency()) * 2) {
        st.exact(in, static_cast<int>(n), out, static_cast<int>(frames));
        return { std::move(l), std::move(r) };
    }
    // Too short for the phase vocoder: plain linear resample (still pitch-shifted by `transpose` ignored).
    for (size_t i = 0; i < frames; ++i) {
        const double p = std::min(static_cast<double>(n - 1), static_cast<double>(i) * speed);
        const size_t i0 = static_cast<size_t>(p);
        const size_t i1 = std::min(n - 1, i0 + 1);
        const float f = static_cast<float>(p - static_cast<double>(i0));
        l[i] = left[i0] + (left[i1] - left[i0]) * f;
        r[i] = right[i0] + (right[i1] - right[i0]) * f;
    }
    return { std::move(l), std::move(r) };
}

struct Stream::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> st { 12345L };
    std::array<float, MAX_OUT> outL {}, outR {};
    uint32_t rate = 44100;
    explicit Impl(uint32_t r) : rate(r == 0 ? 1u : r)
    {
        st.presetDefault(2, static_cast<float>(rate));
        st.setTransposeFactor(1.0f);
    }
};

Stream::Stream(uint32_t rate) : impl_(std::make_unique<Impl>(rate)) {}
Stream::~Stream() = default;

size_t Stream::delay() const noexcept { return static_cast<size_t>(impl_->st.outputLatency()); }

size_t Stream::seek_length(double speed) const noexcept
{
    return static_cast<size_t>(impl_->st.outputSeekLength(static_cast<float>(speed)));
}

void Stream::start(const float* inL, const float* inR, size_t frames, double /*speed*/)
{
    const float* in[2] = { inL, inR };
    impl_->st.outputSeek(in, static_cast<int>(frames));
}

void Stream::setTranspose(double factor) { impl_->st.setTransposeFactor(static_cast<float>(factor)); }

void Stream::process(const float* inL, const float* inR, size_t inFrames,
                     float* outL, float* outR, size_t outFrames)
{
    const float* in[2] = { inL, inR };
    float* out[2] = { outL, outR };
    impl_->st.process(in, static_cast<int>(inFrames), out, static_cast<int>(outFrames));
}

} // namespace as::stretch


// ============================================================================
// Sample tags and file parsing.
// ============================================================================
namespace as {
namespace {


WavTags tags_of_bytes(const void* data, size_t size)
{
    WavTags tags;
    const auto* b = static_cast<const uint8_t*>(data);
    if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) return tags;
    size_t pos = 12;
    bool acid_seen = false;
    while (pos + 8 <= size) {
        const uint8_t* id = b + pos;
        const uint32_t len = u32le(b + pos + 4);
        pos += 8;
        if ((std::memcmp(id, "smpl", 4) == 0 || std::memcmp(id, "acid", 4) == 0) && len >= 16) {
            if (size - pos < 16) break;
            const uint32_t v = u32le(b + pos + 12);
            if (std::memcmp(id, "smpl", 4) == 0) {
                if (!tags.unity_note.has_value() && v <= 127) tags.unity_note = static_cast<uint8_t>(v);
            } else if (!acid_seen) {
                acid_seen = true;
                if (v > 0 && (b[pos] & 1u) == 0) tags.beats = v;
            }
        }
        const uint64_t adv = 8u + static_cast<uint64_t>(len) + (len & 1u);
        if (adv > size - pos) break;
        pos += static_cast<size_t>(len) + (len & 1u);
    }
    return tags;
}

bool walk_wav_chunks(const juce::File& path,
                     const std::function<bool(const char (&)[5], const uint8_t*, size_t)>& on_chunk)
{
    std::ifstream in(path.getFullPathName().toStdString(), std::ios::binary);
    if (!in) return false;
    uint8_t head[12];
    if (!in.read(reinterpret_cast<char*>(head), 12)) return false;
    if (std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return false;
    for (;;) {
        uint8_t chunk[8];
        if (!in.read(reinterpret_cast<char*>(chunk), 8)) break;
        const uint32_t len = u32le(chunk + 4);
        const size_t bodyLen = std::min<size_t>(64, len);
        uint8_t body[64] = {};
        if (!in.read(reinterpret_cast<char*>(body), static_cast<std::streamsize>(bodyLen))) break;
        char id[5] = { static_cast<char>(chunk[0]), static_cast<char>(chunk[1]),
                       static_cast<char>(chunk[2]), static_cast<char>(chunk[3]), 0 };
        if (!on_chunk(id, body, bodyLen)) return true;
        const int64_t rest = static_cast<int64_t>(len) - static_cast<int64_t>(bodyLen) + static_cast<int64_t>(len & 1u);
        if (rest > 0) in.seekg(rest, std::ios::cur);
        if (!in) break;
    }
    return true;
}

} // namespace

WavTags wav_tags_from_bytes(const void* data, size_t size) { return tags_of_bytes(data, size); }

WavTags wav_tags(const juce::File& path)
{
    WavTags tags;
    bool acid_seen = false;
    walk_wav_chunks(path, [&](const char (&id)[5], const uint8_t* body, size_t len) {
        if (std::memcmp(id, "smpl", 4) == 0 && len >= 16) {
            const uint32_t v = u32le(body + 12);
            if (!tags.unity_note.has_value() && v <= 127) tags.unity_note = static_cast<uint8_t>(v);
        } else if (std::memcmp(id, "acid", 4) == 0 && len >= 16 && !acid_seen) {
            acid_seen = true;
            const bool one_shot = (body[0] & 1u) != 0;
            const uint32_t v = u32le(body + 12);
            if (v > 0 && !one_shot) tags.beats = v;
        }
        return true;
    });
    return tags;
}

std::pair<uint32_t, SourceInfo> source_info(const juce::File& path)
{
    uint32_t bits = 16;
    SourceInfo src;
    walk_wav_chunks(path, [&](const char (&id)[5], const uint8_t* body, size_t len) {
        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            const uint16_t tag = static_cast<uint16_t>(body[0] | (body[1] << 8));
            const uint16_t channels = static_cast<uint16_t>(body[2] | (body[3] << 8));
            const uint16_t bps = static_cast<uint16_t>(body[14] | (body[15] << 8));
            uint16_t effective = tag;
            if (tag == 0xFFFE && len >= 26) effective = static_cast<uint16_t>(body[24] | (body[25] << 8));
            if (effective == 3) bits = bps | 0x100u;
            else bits = bps;
            src.int16_mono = (effective == 1) && (channels == 1) && (bps == 16);
        } else if (std::memcmp(id, "smpl", 4) == 0 && len >= 36 + 24 && !src.file_loop) {
            const uint32_t nLoops = u32le(body + 28);
            if (nLoops >= 1) {
                const uint32_t start = u32le(body + 36 + 8);
                const uint32_t end = u32le(body + 36 + 12);
                src.file_loop = std::make_pair(start, end);
            }
        }
        return true;
    });
    src.bits = bits;
    return { bits, src };
}

// ============================================================================
// SampleLibrary.
// ============================================================================
SampleLibrary::SampleLibrary() { formats_.registerBasicFormats(); }

std::optional<std::pair<juce::int64, juce::Time>> SampleLibrary::stamp(const juce::File& path)
{
    if (!path.existsAsFile()) return std::nullopt;
    return std::make_pair(path.getSize(), path.getLastModificationTime());
}

std::optional<std::pair<int64_t, uint32_t>> SampleLibrary::length(const juce::File& path)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(path));
    if (reader && reader->lengthInSamples >= 0 && reader->sampleRate > 0.0) {
        return std::make_pair(reader->lengthInSamples, static_cast<uint32_t>(std::lround(reader->sampleRate)));
    }
    try {
        auto s = load_shared(path);
        if (!s) return std::nullopt;
        return std::make_pair(static_cast<int64_t>(s->frames()), s->rate);
    } catch (...) {
        return std::nullopt;
    }
}

std::shared_ptr<const Sample> SampleLibrary::decode(const juce::File& path) const
{
    if (!path.existsAsFile())
        throw std::runtime_error("no such file: " + path.getFullPathName().toStdString());
    if (path.hasFileExtension("wv"))
        throw std::runtime_error(path.getFileName().toStdString() + ": WavPack is not supported");
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(path));
    if (!reader) throw std::runtime_error(path.getFileName().toStdString() + ": could not decode");
    const auto numCh = static_cast<int>(reader->numChannels);
    const auto frames = static_cast<int>(reader->lengthInSamples);
    if (numCh <= 0 || frames <= 0) throw std::runtime_error(path.getFileName().toStdString() + ": empty");
    juce::AudioBuffer<float> buffer(juce::jmin(numCh, 2), frames);
    buffer.clear();
    if (!reader->read(&buffer, 0, frames, 0, true, numCh > 1))
        throw std::runtime_error(path.getFileName().toStdString() + ": read failed");

    auto s = std::make_shared<Sample>();
    s->rate = static_cast<uint32_t>(std::lround(reader->sampleRate));
    s->left.resize(static_cast<size_t>(frames));
    s->right.resize(static_cast<size_t>(frames));
    const float* l = buffer.getReadPointer(0);
    const float* r = (buffer.getNumChannels() > 1) ? buffer.getReadPointer(1) : l;
    std::copy(l, l + frames, s->left.begin());
    std::copy(r, r + frames, s->right.begin());
    return s;
}

std::shared_ptr<const Sample> SampleLibrary::load(const juce::File& path) const { return decode(path); }

std::shared_ptr<const Sample> SampleLibrary::load_shared(const juce::File& path)
{
    const juce::String key = path.getFullPathName();
    const auto st = stamp(path);
    if (st) {
        const std::lock_guard<std::mutex> sl(lock_);
        auto it = live_.find(key);
        if (it != live_.end()) {
            const auto alive = it->second.sample.lock();
            if (alive && it->second.size == st->first && it->second.mtime == st->second) return alive;
        }
    }
    auto s = decode(path);
    if (st) {
        const std::lock_guard<std::mutex> sl(lock_);
        if (live_.size() > 256) {
            for (auto it = live_.begin(); it != live_.end();) {
                if (it->second.sample.expired()) it = live_.erase(it);
                else ++it;
            }
        }
        live_[key] = Entry{ s, st->first, st->second };
    }
    return s;
}

bool SampleLibrary::is_held(const juce::File& path) const
{
    const std::lock_guard<std::mutex> sl(lock_);
    auto it = live_.find(path.getFullPathName());
    return it != live_.end() && !it->second.sample.expired();
}

} // namespace as

// ============================================================================
// build / as_is — the precompute pipeline.
// ============================================================================
namespace as {
namespace {

using Sides = std::array<std::vector<float>, 2>;

inline int64_t round_i(double x) noexcept { return static_cast<int64_t>(std::nearbyint(x)); }

std::optional<std::pair<size_t, size_t>> limit(std::optional<std::pair<size_t, size_t>> lp, size_t n)
{
    if (!lp) return std::nullopt;
    auto [a, b] = *lp;
    if (n == 0) return std::nullopt;
    if (b > n - 1) b = n - 1;
    if (a > b) a = b;
    return std::make_pair(a, b);
}

void declick_edge(std::vector<float>& x, size_t l0, double rate, bool front)
{
    const size_t n = x.size();
    if (n == 0 || l0 == 0) return;
    size_t e, avail;
    if (front) { e = 0; while (e < n && x[e] == 0.0f) ++e; avail = n - e; }
    else { e = n - 1; while (e > 0 && x[e] == 0.0f) --e; avail = e + 1; }
    const size_t l = std::min(l0, avail);
    if (l == 0) return;
    const size_t m = std::min<size_t>(l, static_cast<size_t>(std::max<int64_t>(round_i(rate * 0.001), 0)));
    const double w = std::log(22050.0) / (std::log(2.0) * static_cast<double>(l)) * std::log(2.0);
    double lp = 0.0;
    for (size_t i = 1; i <= l; ++i) {
        const size_t at = front ? e + l - i : e + i - l;
        const double c = std::min(1.0, (2.0 / rate) * std::exp(static_cast<double>(i) * w));
        const double xv = static_cast<double>(x[at]);
        lp += c * (xv - lp);
        const double g = (i <= l - m) ? 1.0
            : ((std::cos(static_cast<double>(i - (l - m)) / static_cast<double>(m + 1) * PI_D) + 1.0) * 0.5);
        x[at] = static_cast<float>((xv - lp) * g);
    }
}

std::optional<std::pair<size_t, size_t>> cut(Sides& s, std::optional<std::pair<size_t, size_t>> lp,
                                             double start, double length, double rate)
{
    const int64_t n = static_cast<int64_t>(s[0].size());
    const double m = static_cast<double>(n - 1);
    int64_t i0 = round_i(start * m);
    int64_t i1 = i0 + round_i(length * (1.0 - start) * m);
    if (i1 - i0 < 41) {
        if (i0 < n - 42) i1 = std::min<int64_t>(i1 + 41, n - 1);
        else i0 -= 41;
    }
    i0 = std::clamp<int64_t>(i0, 0, n - 1);
    i1 = std::clamp<int64_t>(i1, i0, n - 1);
    const size_t a = static_cast<size_t>(i0), b = static_cast<size_t>(i1);
    for (auto& side : s)
        side = std::vector<float>(side.begin() + static_cast<std::ptrdiff_t>(a), side.begin() + static_cast<std::ptrdiff_t>(b + 1));
    std::optional<std::pair<size_t, size_t>> out_lp;
    if (lp && lp->first >= a && lp->first <= b) out_lp = { lp->first - a, std::min(lp->second, b) - a };
    if (start > 0.0) {
        const auto l0 = static_cast<size_t>(std::max<int64_t>(round_i(rate * static_cast<double>(static_cast<float>(0.005)) * static_cast<double>(static_cast<float>(0.1))), 0));
        for (auto& side : s) declick_edge(side, l0, rate, true);
    }
    if (length < 1.0) {
        const auto l0 = static_cast<size_t>(std::max<int64_t>(round_i(rate * static_cast<double>(static_cast<float>(0.04)) * static_cast<double>(static_cast<float>(0.1))), 0));
        for (auto& side : s) declick_edge(side, l0, rate, false);
    }
    return out_lp;
}

constexpr int CONVERT_N = 64;

std::pair<Sides, std::optional<std::pair<size_t, size_t>>>
convert(const Sides& s, std::optional<std::pair<size_t, size_t>> lp, double ratio)
{
    const size_t n = s[0].size();
    const size_t frames = static_cast<size_t>(std::max<int64_t>(round_i(static_cast<double>(n) * ratio), 0));
    const double r = 1.0 / ratio;
    const bool wrap = lp.has_value() && lp->second == n - 1;
    auto at = [&](int64_t j) -> size_t {
        if (j < 0) return 0;
        if (static_cast<size_t>(j) < n) return static_cast<size_t>(j);
        if (wrap) {
            const size_t a = lp->first, b = lp->second;
            return a + (static_cast<size_t>(j) - (b + 1)) % (b - a + 1);
        }
        return n - 1;
    };
    std::array<float, 2 * CONVERT_N> w {};
    Sides out;
    out[0].assign(frames, 0.0f);
    out[1].assign(frames, 0.0f);
    for (size_t k = 0; k < frames; ++k) {
        const double pos = static_cast<double>(k) * r;
        const double k0 = std::floor(pos);
        resample::weights(pos - k0, r, CONVERT_N, resample::Quality::Exact, w.data());
        const int64_t first = static_cast<int64_t>(k0) - CONVERT_N + 1;
        for (size_t c = 0; c < 2; ++c) {
            float acc = 0.0f;
            for (int t = 0; t < 2 * CONVERT_N; ++t) acc += s[c][at(first + t)] * w[static_cast<size_t>(t)];
            out[c][k] = acc;
        }
    }
    std::optional<std::pair<size_t, size_t>> new_lp;
    if (lp) {
        const auto a = static_cast<size_t>(std::max<int64_t>(round_i(static_cast<double>(lp->first) * ratio), 0));
        const auto b = static_cast<size_t>(std::max<int64_t>(round_i(static_cast<double>(lp->second + 1) * ratio) - 1, 0));
        new_lp = { a, b };
    }
    return { std::move(out), limit(new_lp, frames) };
}

float pogo_read(const std::vector<float>& t, size_t k, double m, uint16_t v, float b)
{
    const double kf = static_cast<double>(k);
    double p;
    if (v < 256) {
        const double u = kf / m - 1.0;
        p = m + std::atan2(u, std::sqrt(1.0 - u * u)) * m * (2.0 / PI_D);
    } else {
        p = std::sin(kf / m * (PI_D / 2.0)) * m;
    }
    const double pp = static_cast<double>(static_cast<float>(k) * (1.0f - b)) + p * static_cast<double>(b);
    const auto i = static_cast<int64_t>(static_cast<float>(pp));
    const float f = static_cast<float>(pp - static_cast<double>(i));
    auto at = [&](int64_t j) -> float { return (j < 0 || static_cast<size_t>(j) >= t.size()) ? 0.0f : t[static_cast<size_t>(j)]; };
    const float ym = at(i - 1), y0 = at(i), y1 = at(i + 1), y2 = at(i + 2);
    const float a = static_cast<float>(static_cast<double>((y0 - y1) * 3.0f - ym + y2) * 0.5);
    const float bb = static_cast<float>(static_cast<double>(y1 * 2.0f + ym) - static_cast<double>(y0 * 5.0f + y2) * 0.5);
    const float c = static_cast<float>(static_cast<double>(y1 - ym) * 0.5);
    return y0 + ((a * f + bb) * f + c) * f;
}

std::vector<float> ring_table()
{
    std::vector<float> t(16384);
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = static_cast<float>(std::sin(2.0 * static_cast<double>(i) * PI_D / 16384.0));
    return t;
}

struct Line
{
    std::vector<float> buf;
    size_t at = 0;
    explicit Line(size_t d) : buf(std::max<size_t>(1, d - 1), 0.0f) {}
    float out() const noexcept { return buf[at]; }
    void push(float v) noexcept { buf[at] = v; at = (at + 1) % buf.size(); }
    float comb(float input, float fb) noexcept
    {
        const float t = static_cast<float>(static_cast<double>(input) + static_cast<double>(fb) * static_cast<double>(out()));
        push(t); return t;
    }
    float allpass(float input) noexcept
    {
        const float d = out();
        const float u = input + d * G7;
        push(u);
        return d - u * G7;
    }
    static constexpr float G7 = 0.7f, G3 = 0.3f;
};

void reverb(Sides& s, std::optional<std::pair<size_t, size_t>> lp, uint16_t amount, uint16_t kind)
{
    const size_t n = s[0].size();
    for (auto& side : s) side.resize(n + 22050, 0.0f);
    const double time = static_cast<double>(amount) / 256.0;
    auto fb = [&](size_t d) -> float {
        const float e = static_cast<float>(-3.0 * static_cast<double>(d) / (time * 44100.0));
        return static_cast<float>(std::exp(static_cast<double>(e) * std::log(10.0)));
    };
    for (size_t c = 0; c < 2; ++c) {
        auto& side = s[c];
        if (kind == 0) {
            constexpr size_t COMBS[4] = { 1777, 1847, 1993, 2137 };
            std::array<Line, 4> combs = { Line(COMBS[0]), Line(COMBS[1]), Line(COMBS[2]), Line(COMBS[3]) };
            std::array<float, 4> cfb = { fb(COMBS[0]), fb(COMBS[1]), fb(COMBS[2]), fb(COMBS[3]) };
            std::array<Line, 3> aps = { Line(389), Line(127), Line(43) };
            float h1 = 0.0f, h2 = 0.0f;
            for (auto& x : side) {
                const float input = x;
                float w = input;
                for (auto& a : aps) w = a.allpass(w);
                float h0 = 0.0f;
                for (size_t k = 0; k < 4; ++k) h0 += combs[k].comb(w, cfb[k]);
                const float y = static_cast<float>(static_cast<double>(h1) * 0.5 + static_cast<double>(h0 + h2) * 0.25);
                h2 = h1; h1 = h0;
                x = y * Line::G3 + input * Line::G7;
            }
        } else {
            constexpr size_t COMBS[6] = { 2467, 2767, 3217, 3533, 3877, 4127 };
            std::array<Line, 6> combs = { Line(COMBS[0]), Line(COMBS[1]), Line(COMBS[2]), Line(COMBS[3]), Line(COMBS[4]), Line(COMBS[5]) };
            std::array<float, 6> cfb = { fb(COMBS[0]), fb(COMBS[1]), fb(COMBS[2]), fb(COMBS[3]), fb(COMBS[4]), fb(COMBS[5]) };
            std::array<Line, 3> aps = { Line(599), Line(197), Line(67) };
            Line ap101(101);
            Line last(c == 0 ? 97 : 79);
            float low = 0.0f;
            for (auto& x : side) {
                const float input = x;
                float sum = 0.0f;
                for (size_t k = 0; k < 6; ++k) sum += combs[k].comb(input, cfb[k]);
                float a = sum;
                for (auto& ap : aps) a = ap.allpass(a);
                low = static_cast<float>(static_cast<double>(low) * static_cast<double>(Line::G7) + static_cast<double>(a) * static_cast<double>(Line::G3));
                const float wet = last.allpass(ap101.allpass(low)) * Line::G3;
                x = wet * Line::G3 + input * Line::G7;
            }
        }
    }
    const size_t floor_frame = lp ? std::max<size_t>(lp->second, 1) : 1;
    const float quiet = 1.0f / 512.0f;
    size_t len = s[0].size();
    while (len > floor_frame && std::abs(s[0][len - 1]) < quiet && std::abs(s[1][len - 1]) < quiet) --len;
    for (auto& side : s) side.resize(len);
}

void trim(Sides& s, std::optional<std::pair<size_t, size_t>>& lp, uint32_t v)
{
    const size_t n = s[0].size();
    if (n == 0) return;
    const float thr = static_cast<float>(static_cast<double>(v) / 32768.0);
    auto level = [&](size_t k) { return std::max(std::abs(s[0][k]), std::abs(s[1][k])); };
    const size_t front_limit = lp ? std::min(lp->first, n - 1) : n - 1;
    size_t a = 0;
    while (a < front_limit && level(a) < thr) ++a;
    const size_t back_limit = lp ? std::max(std::min(lp->second, n - 1), a) : a;
    size_t b = n - 1;
    while (b > back_limit && level(b) < thr) --b;
    for (auto& side : s) {
        side.resize(b + 1);
        side.erase(side.begin(), side.begin() + static_cast<std::ptrdiff_t>(a));
    }
    lp = limit(lp, s[0].size());
}

void effects(Sides& s, std::optional<std::pair<size_t, size_t>>& lp, const SampleFx& fx,
             const SourceInfo& src, double rs)
{
    if (fx.stereo_delay != 2048) {
        if (src.int16_mono) {
            for (auto& side : s) for (auto& x : side) {
                const float v = std::clamp(x, -1.0f, 1.0f);
                x = static_cast<float>(std::nearbyint(static_cast<double>(v) * 32767.0) / 32768.0);
            }
        }
        const auto d = static_cast<size_t>(std::max<int64_t>(
            round_i(std::abs(static_cast<int32_t>(fx.stereo_delay) - 2048) / rs), 0));
        const bool left_late = fx.stereo_delay > 2048;
        for (size_t c = 0; c < 2; ++c) {
            auto& side = s[c];
            if (c == (left_late ? 0u : 1u)) side.insert(side.begin(), d, 0.0f);
            else side.resize(side.size() + d, 0.0f);
        }
    }
    const size_t n = s[0].size();
    const size_t m = n - 1;
    if (fx.flags & SampleFx::REVERSE) {
        for (auto& side : s) std::reverse(side.begin(), side.end());
        if (fx.loop_follows_reverse && lp) lp = { m - lp->second, m - lp->first };
    }
    const bool pogo = fx.pogo != 256;
    const bool ring = fx.ring_mix > 0;
    const bool lowpass = fx.cutoff < 1024 || fx.resonance > 0;
    const bool fin = fx.fade_in > 0;
    const bool fout = fx.fade_out > 0;
    const bool boost = fx.boost > 0;
    const bool clip = fx.flags & SampleFx::CLIP;
    if (pogo || ring || lowpass || fin || fout || boost || clip) {
        const auto tab = ring ? ring_table() : std::vector<float>{};
        const double mf = static_cast<double>(m);
        const float pogo_b = static_cast<float>(static_cast<double>(std::abs(static_cast<int32_t>(fx.pogo) - 256)) / 256.0);
        const double mix = static_cast<double>(fx.ring_mix) / 256.0;
        const double inc = (static_cast<double>(fx.ring_freq) + 8.0) * 16384.0 * rs / 5760.0;
        const float ln601 = static_cast<float>(std::log(601.0));
        auto e = [&](double z) -> double { return std::exp(static_cast<double>(static_cast<float>(z * static_cast<double>(ln601)))) - 1.0; };
        const float inv600 = static_cast<float>(1.0 / 600.0);
        const double res = static_cast<double>(std::min<int32_t>(fx.resonance, 640));
        const double lp_a = static_cast<double>(static_cast<float>(static_cast<double>(static_cast<float>(e(static_cast<double>(fx.cutoff) / 1024.0) * static_cast<double>(inv600))) * rs));
        const double lp_b = static_cast<double>(1.0f - static_cast<float>(e((1024.0 - res) / 1024.0) * static_cast<double>(inv600)));
        const double q = (1.0 / mf) / 256.0;
        const float boost_g = static_cast<float>(static_cast<double>(fx.boost) / 16.0 + 1.0);
        for (size_t c = 0; c < 2; ++c) {
            auto& side = s[c];
            std::vector<float> t;
            if (pogo) t = side;
            double phase = static_cast<double>(c) * 4096.0;
            double v = 0.0, o = 0.0;
            for (size_t k = 0; k < n; ++k) {
                float x = side[k];
                if (pogo) x = pogo_read(t, k, mf, fx.pogo, pogo_b);
                if (ring) {
                    const auto idx = static_cast<size_t>(static_cast<float>(phase));
                    const float tv = tab[idx >= 16384 ? 0 : idx];
                    const float mv = static_cast<float>(static_cast<double>(tv * x) * mix);
                    phase += inc;
                    while (phase >= 16384.0) phase -= 16384.0;
                    x = static_cast<float>(static_cast<double>(mv) + static_cast<double>(x) * (1.0 - mix));
                }
                if (lowpass) { v += (static_cast<double>(x) - o) * lp_a; o += v; v *= lp_b; x = static_cast<float>(o); }
                if (fin) { const float g = static_cast<float>(1.0 - q * (mf - static_cast<double>(k)) * static_cast<double>(fx.fade_in)); x = (g <= 0.0f) ? 0.0f : x * g; }
                if (fout) { const float g = static_cast<float>(1.0 - static_cast<double>(k) * q * static_cast<double>(fx.fade_out)); x = (g <= 0.0f) ? 0.0f : x * g; }
                if (boost) x *= boost_g;
                if (clip) x = std::clamp(x, -1.0f, 1.0f);
                side[k] = x;
            }
        }
    }
    if (fx.feedback != 128) {
        const auto c = static_cast<float>(static_cast<double>(std::floor(static_cast<double>(fx.feedback) * 192.0 / 256.0) - 96.0) * static_cast<double>(static_cast<float>(1.0 / 128.0)));
        for (auto& side : s) {
            float y = 0.0f;
            for (auto& x : side) { y = x + y * c; x = y; }
        }
    }
    if (fx.flags & SampleFx::FADE_STEREO) {
        const double w = (PI_D / 2.0) / static_cast<double>(n - 1);
        for (size_t k = 0; k < n; ++k) {
            s[0][k] = static_cast<float>(static_cast<double>(s[0][k]) * std::cos(static_cast<double>(k) * w));
            s[1][k] = static_cast<float>(static_cast<double>(s[1][k]) * std::cos(static_cast<double>(n - 1 - k) * w));
        }
    }
    if (fx.remove_dc) {
        for (auto& side : s) {
            double sum = 0.0;
            for (auto x : side) sum += static_cast<double>(x);
            const double mean = sum / static_cast<double>(n);
            for (auto& x : side) x = static_cast<float>(static_cast<double>(x) - mean);
        }
    }
    if (fx.polarity) for (auto& side : s) for (auto& x : side) x = -x;
    if (fx.normalize) {
        float peak = 0.0f;
        for (auto& side : s) for (auto x : side) peak = std::max(peak, std::abs(x));
        if (peak > 0.0f) {
            const float g = 1.0f / peak;
            for (auto& side : s) for (auto& x : side) x *= g;
        }
    }
    if (fx.crossfade > 0 && lp) {
        const size_t l = static_cast<size_t>((static_cast<uint64_t>(lp->second - lp->first + 1) * fx.crossfade) / 2048);
        if (l > 0 && lp->second + 1 >= l) {
            const double inv = 1.0 / static_cast<double>(l);
            for (auto& side : s) {
                for (size_t k = 0; k < l; ++k) {
                    const double t = static_cast<double>(k) * inv;
                    const size_t d = lp->second + 1 - l + k;
                    const size_t from = lp->first + k;
                    if (from < side.size())
                        side[d] = static_cast<float>(static_cast<double>(side[d]) * (1.0 - t) + static_cast<double>(side[from]) * t);
                }
            }
            lp = { std::min(lp->first + l, lp->second), lp->second };
        }
    }
    if (fx.flags & SampleFx::SWAP) std::swap(s[0], s[1]);
    if (fx.reverb > 0) reverb(s, lp, fx.reverb, fx.reverb_type);
    if (fx.trim > 0) trim(s, lp, fx.trim);
}

} // namespace

Built build(const Sample& raw, const SourceInfo& src, const FxKey& k)
{
    const SampleFx& fx = k.fx;
    Sides s = { raw.left, raw.right };
    double rate = static_cast<double>(raw.rate == 0 ? 1u : raw.rate);
    const double out_rate = static_cast<double>(k.rate == 0 ? 1u : k.rate);
    const size_t n0 = s[0].size();
    std::optional<std::pair<size_t, size_t>> lp;
    if (src.file_loop) lp = std::make_pair(static_cast<size_t>(src.file_loop->first), static_cast<size_t>(src.file_loop->second));
    double per_file_frame = 1.0;
    const double start = fx.start();
    const double length = fx.length();
    if (n0 > 0 && (start != 0.0 || length != 1.0)) lp = cut(s, lp, start, length, rate);
    lp = limit(lp, s[0].size());
    if (rate < 8000.0 && !s[0].empty()) {
        const double ratio = out_rate / rate;
        auto [ns, nlp] = convert(s, lp, ratio);
        s = std::move(ns); lp = nlp;
        per_file_frame = ratio;
        rate = out_rate;
    }
    if (k.stretch && !s[0].empty()) {
        const double ratio = k.ratio_for(static_cast<double>(s[0].size()) / rate);
        const double shift = std::pow(2.0, static_cast<double>(k.shift) / 1200.0);
        if (std::abs(ratio - 1.0) > 1e-4 || std::abs(shift - 1.0) > 1e-4) {
            const size_t frames = static_cast<size_t>(std::max(std::nearbyint(static_cast<double>(s[0].size()) * ratio), 1.0));
            auto [l, r] = stretch::whole(s[0], s[1], static_cast<uint32_t>(rate), frames, shift);
            const double scale = static_cast<double>(frames) / static_cast<double>(s[0].size());
            if (lp) {
                lp = std::make_pair(static_cast<size_t>(std::nearbyint(static_cast<double>(lp->first) * scale)),
                                    static_cast<size_t>(std::nearbyint(static_cast<double>(lp->second + 1) * scale)) - 1);
            }
            lp = limit(lp, frames);
            s = { std::move(l), std::move(r) };
            per_file_frame *= scale;
        }
    }
    if (k.knob_loop) {
        const auto n = static_cast<double>(s[0].size());
        const auto a = static_cast<size_t>(std::floor(static_cast<double>(std::clamp(k.knob_loop->first, 0.0f, 1.0f)) * n));
        const auto e2 = static_cast<size_t>(std::floor(static_cast<double>(std::clamp(k.knob_loop->second, 0.0f, 1.0f)) * n));
        lp = (e2 >= a + 2) ? std::make_optional(std::make_pair(a, e2 - 1)) : std::nullopt;
    }
    if (!lp && fx.crossfade > 0 && !s[0].empty()) lp = std::make_pair(size_t{ 0 }, s[0].size() - 1);
    if (fx.ping_pong && lp && lp->first + 1 < lp->second) {
        const auto a = lp->first, b = lp->second;
        for (auto& side : s) {
            std::vector<float> v;
            v.reserve(side.size() + b - a - 1);
            v.insert(v.end(), side.begin(), side.begin() + static_cast<std::ptrdiff_t>(b + 1));
            for (size_t i = b - 1; i > a; --i) v.push_back(side[i]);
            v.insert(v.end(), side.begin() + static_cast<std::ptrdiff_t>(b + 1), side.end());
            side = std::move(v);
        }
        lp = { a, b + (b - a - 1) };
    }
    if (s[0].size() > 3) {
        const double rs = 44100.0 / (rate * (1.0 / out_rate) * out_rate);
        effects(s, lp, fx, src, rs);
    }
    Built b;
    b.sample.rate = static_cast<uint32_t>(rate);
    b.sample.left = std::move(s[0]);
    b.sample.right = std::move(s[1]);
    b.looped = lp;
    b.per_file_frame = per_file_frame;
    return b;
}

std::optional<std::optional<std::pair<size_t, size_t>>>
as_is(const SourceInfo& src, const FxKey& k, uint32_t rate, size_t frames)
{
    const SampleFx& fx = k.fx;
    if (frames == 0 || fx.start() != 0.0 || fx.length() != 1.0 || rate < 8000) return std::nullopt;
    if (k.stretch) {
        const double ratio = k.ratio_for(static_cast<double>(frames) / static_cast<double>(rate));
        const double shift = std::pow(2.0, static_cast<double>(k.shift) / 1200.0);
        if (std::abs(ratio - 1.0) > 1e-4 || std::abs(shift - 1.0) > 1e-4) return std::nullopt;
    }
    const bool changes = fx.stereo_delay != 2048
        || (fx.flags & (SampleFx::REVERSE | SampleFx::CLIP | SampleFx::FADE_STEREO | SampleFx::SWAP)) != 0
        || fx.pogo != 256 || fx.ring_mix > 0 || fx.cutoff < 1024 || fx.resonance > 0
        || fx.fade_in > 0 || fx.fade_out > 0 || fx.boost > 0 || fx.feedback != 128
        || fx.remove_dc || fx.polarity || fx.normalize || fx.crossfade > 0 || fx.reverb > 0 || fx.trim > 0;
    if (changes) return std::nullopt;
    std::optional<std::pair<size_t, size_t>> lp;
    if (src.file_loop) lp = std::make_pair(static_cast<size_t>(src.file_loop->first), static_cast<size_t>(src.file_loop->second));
    lp = limit(lp, frames);
    if (k.knob_loop) {
        const auto n = static_cast<double>(frames);
        const auto a = static_cast<size_t>(std::floor(static_cast<double>(std::clamp(k.knob_loop->first, 0.0f, 1.0f)) * n));
        const auto e2 = static_cast<size_t>(std::floor(static_cast<double>(std::clamp(k.knob_loop->second, 0.0f, 1.0f)) * n));
        lp = (e2 >= a + 2) ? std::make_optional(std::make_pair(a, e2 - 1)) : std::nullopt;
    }
    if (fx.ping_pong && lp && lp->first + 1 < lp->second) return std::nullopt;
    return std::make_optional(lp);
}

} // namespace as

// ============================================================================
// Voices — LFO, declick, pan, and the polyphonic engine.
// ============================================================================
namespace as {

double Lfo::wave(double phase) const noexcept
{
    const double p = phase - std::floor(phase);
    switch (shape) {
        case 1: return p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
        case 2: return p < 0.5 ? 1.0 : -1.0;
        default: return std::sin(6.283185307179586 * p);
    }
}

double Lfo::value(double t, double phase) const noexcept
{
    if (!active()) return 0.0;
    double ramp = 1.0;
    if (!global) {
        if (t < delay) return 0.0;
        ramp = attack > 0.0 ? std::min(1.0, (t - delay) / attack) : 1.0;
    }
    return amount * ramp * wave(phase);
}

Declick declick(int32_t mode)
{
    // open_ms / close_ms / flags per mode (see the "Declicking mode" choices).
    static constexpr struct { double open, close; bool step, sweep, bleed; float tension; } table[6] = {
        { 0.0,  6.0, false, false, false, 0.0f },   // Out only (no bleeding)
        { 1.0,  6.0, true,  false, false, 0.0f },   // Transient (no bleeding)
        { 1.0, 12.0, true,  false, true,  0.0f },   // Transient (bleeding)
        { 2.0, 20.0, true,  true,  true,  0.2f },   // Generic (bleeding)
        { 8.0, 40.0, true,  true,  true,  0.4f },   // Smooth (bleeding)
        { 15.0, 80.0, true, true,  true,  0.6f },   // Crossfade (bleeding)
    };
    const auto& m = table[std::clamp(mode, 0, 5)];
    return { m.open, m.close, m.step, m.sweep, m.bleed, m.tension };
}

std::pair<float, float> pan_gains(double pan)
{
    const double q = std::clamp(pan, -1.0, 1.0);
    const double a = (1.0 + q) * 0.7853981633974483;
    return { static_cast<float>(std::min(1.0, std::cos(a) * 1.4142135623730951)),
             static_cast<float>(std::min(1.0, std::sin(a) * 1.4142135623730951)) };
}

struct Voice
{
    bool active = false, released = false, killing = false;
    uint8_t key = 0;
    float vel = 1.0f;
    uint64_t serial = 0;

    std::shared_ptr<const Sample> ram;
    std::shared_ptr<const stream::Played> disk;
    stream::Feed feed;
    std::optional<std::pair<size_t, size_t>> loop;
    size_t frames = 0;

    double src_ratio = 1.0;     // source rate / host rate
    double length_ratio = 1.0;  // output length / source length
    double base_cents = 0.0;    // key tracking
    double pos = 0.0;           // playback position (source frames)

    bool realtime = false;      // phase-vocoder voice
    int stream_idx = -1;
    int64_t in_pos = 0;
    double in_acc = 0.0;
    size_t tail = 0;

    double t = 0.0, t_release = 0.0, t_kill = 0.0, kill_len = 0.002;
    std::array<double, 5> phase {};
    Declick dk;
    ChannelFilter filter;
    float gl = 0.0f, gr = 0.0f;
    bool fresh = true;
};

Engine::Engine(float rate) : rate_(rate)
{
    trash_.reserve(1024);
    set_rate(rate);
}

Engine::~Engine() = default;

void Engine::set_rate(float rate)
{
    rate_ = rate > 0.0f ? rate : 44100.0f;
    const auto r = static_cast<uint32_t>(std::lround(rate_));
    streams_.clear();
    for (int i = 0; i < STRETCH_STREAMS; ++i) streams_.push_back(std::make_unique<stretch::Stream>(r));
    stream_used_.assign(streams_.size(), 0);
    constexpr size_t SCRATCH = 1u << 16;
    in_l_.assign(SCRATCH, 0.0f);
    in_r_.assign(SCRATCH, 0.0f);
    out_l_.assign(stretch::Stream::MAX_OUT, 0.0f);
    out_r_.assign(stretch::Stream::MAX_OUT, 0.0f);
    voices_.clear();
    voices_.resize(MAX_VOICES);
    for (auto& v : voices_) v.filter = ChannelFilter(0, r);
}

void Engine::reset()
{
    for (auto& v : voices_) if (v.active) free_voice(v);
    global_time_ = 0.0;
}

void Engine::retire(std::shared_ptr<const void>&& p)
{
    if (!p) return;
    std::unique_lock<std::mutex> lk(trash_lock_, std::try_to_lock);
    if (lk.owns_lock() && trash_.size() < trash_.capacity()) trash_.push_back(std::move(p));
    // otherwise `p` is released here (rare)
}

void Engine::collect_garbage()
{
    std::vector<std::shared_ptr<const void>> doomed;
    {
        const std::lock_guard<std::mutex> lk(trash_lock_);
        doomed.swap(trash_);
        trash_.reserve(1024);
    }
}

void Engine::set_sample(std::shared_ptr<const Sample> s)
{
    retire(std::move(sample_));
    retire(std::move(disk_));
    sample_ = std::move(s);
    disk_.reset();
}

void Engine::set_disk(std::shared_ptr<const stream::Played> d)
{
    retire(std::move(sample_));
    retire(std::move(disk_));
    disk_ = std::move(d);
    sample_.reset();
}

void Engine::set_looped(std::optional<std::pair<size_t, size_t>> lp) { looped_ = lp; }

int Engine::active_voices() const noexcept
{
    int n = 0;
    for (const auto& v : voices_) if (v.active) ++n;
    return n;
}

void Engine::free_voice(Voice& v)
{
    if (v.disk) v.disk->release(v.feed);
    v.feed = stream::Feed{};
    if (v.stream_idx >= 0) stream_used_[static_cast<size_t>(v.stream_idx)] = 0;
    v.stream_idx = -1;
    retire(std::move(v.ram));
    retire(std::move(v.disk));
    v.ram.reset();
    v.disk.reset();
    v.active = false;
}

Voice* Engine::allocate()
{
    Voice* best = nullptr;
    for (auto& v : voices_) if (!v.active) return &v;
    // steal: prefer released / fading voices, then the oldest
    for (auto& v : voices_) {
        const int rank = (v.released || v.killing) ? 0 : 1;
        const int best_rank = best ? ((best->released || best->killing) ? 0 : 1) : 2;
        if (rank < best_rank || (rank == best_rank && v.serial < best->serial)) best = &v;
    }
    if (best) free_voice(*best);
    return best;
}

void Engine::note_on(uint8_t key, float vel, const Block& b)
{
    if (key < b.zone.first || key > b.zone.second) return;
    if (!sample_ && !disk_) return;

    const Declick dk = declick(b.declick);
    for (auto& v : voices_) {
        if (v.active && v.key == key && !v.killing) {
            v.killing = true;
            v.t_kill = v.t;
            v.kill_len = dk.bleed ? std::max(0.002, dk.close_ms * 0.001) : 0.002;
        }
    }

    Voice* vp = allocate();
    if (!vp) return;
    Voice& v = *vp;
    v = Voice{};
    v.filter = ChannelFilter(b.f_type, static_cast<uint32_t>(std::lround(rate_)));

    double src_rate;
    if (disk_) { v.disk = disk_; v.frames = disk_->frames(); src_rate = disk_->rate(); }
    else       { v.ram = sample_; v.frames = static_cast<size_t>(sample_->frames()); src_rate = sample_->rate; }
    if (v.frames == 0 || src_rate <= 0.0) { free_voice(v); return; }

    v.active = true;
    v.key = key;
    v.vel = std::clamp(vel, 0.0f, 1.0f);
    v.serial = ++serial_;
    v.dk = dk;
    v.loop = looped_;
    v.src_ratio = src_rate / static_cast<double>(rate_);

    const double secs = static_cast<double>(v.frames) / src_rate;
    double lr = b.ts_time ? (*b.ts_time / std::max(secs, 1e-6)) : b.ts_ratio;
    v.length_ratio = std::clamp(lr, 0.0625, 16.0);

    const double semis = b.add_key ? static_cast<double>(key) - 60.0 : static_cast<double>(key) - static_cast<double>(b.root);
    v.base_cents = semis * 100.0;

    double start = std::clamp(b.start_offset, 0.0, 1.0) * static_cast<double>(v.frames);
    if (start >= static_cast<double>(v.frames) - 1.0) start = 0.0;
    v.pos = start;
    v.in_pos = static_cast<int64_t>(start);

    if (v.disk) {
        v.disk->start_at(v.feed, v.pos, false);
    } else if (b.realtime) {
        for (size_t i = 0; i < stream_used_.size(); ++i) {
            if (stream_used_[i]) continue;
            const double speed = v.src_ratio / v.length_ratio;
            auto& st = *streams_[i];
            const size_t len = st.seek_length(speed);
            if (len == 0 || len > in_l_.size()) break;
            const auto& L = v.ram->left;
            const auto& R = v.ram->right;
            for (size_t k = 0; k < len; ++k) {
                const size_t idx = static_cast<size_t>(v.in_pos) + k;
                in_l_[k] = idx < v.frames ? L[idx] : 0.0f;
                in_r_[k] = idx < v.frames ? R[idx] : 0.0f;
            }
            st.start(in_l_.data(), in_r_.data(), len, speed);
            v.in_pos += static_cast<int64_t>(len);
            v.stream_idx = static_cast<int>(i);
            v.realtime = true;
            stream_used_[i] = 1;
            break;
        }
    }
}

void Engine::note_off(uint8_t key, const Block&)
{
    for (auto& v : voices_) {
        if (v.active && v.key == key && !v.released) { v.released = true; v.t_release = v.t; }
    }
}

void Engine::all_off()
{
    for (auto& v : voices_) if (v.active && !v.released) { v.released = true; v.t_release = v.t; }
}

namespace {

// Windowed-sinc read with the loop folded into the kernel so loop seams are seamless.
std::pair<float, float> read_looped(const Sample& s, size_t a, size_t e, double pos, double ratio,
                                    resample::Quality q) noexcept
{
    const double k0f = std::floor(pos);
    const double f = pos - k0f;
    const int n = resample::half_width(ratio);
    alignas(64) float w[2 * resample::N_MAX];
    resample::weights(f, ratio, n, q, w);
    const int64_t first = static_cast<int64_t>(k0f) - n + 1;
    const int64_t span = static_cast<int64_t>(e - a + 1);
    float l = 0.0f, r = 0.0f;
    for (int t = 0; t < 2 * n; ++t) {
        int64_t i = first + t;
        if (i < 0) i = 0;
        else if (i > static_cast<int64_t>(e)) i = static_cast<int64_t>(a) + (i - static_cast<int64_t>(e) - 1) % span;
        l += s.left[static_cast<size_t>(i)] * w[t];
        r += s.right[static_cast<size_t>(i)] * w[t];
    }
    return { l, r };
}

} // namespace

void Engine::render(float* left, float* right, int n, const Block& b)
{
    for (auto& v : voices_) if (v.active) render_voice(v, left, right, n, b);
    global_time_ += static_cast<double>(n) / static_cast<double>(rate_);
}

void Engine::render_voice(Voice& v, float* outL, float* outR, int n, const Block& b)
{
    constexpr int CH = 32;
    constexpr int MAXCH = 64;
    const double dt = 1.0 / static_cast<double>(rate_);
    float buf[2 * MAXCH];

    const bool loop_on = b.loop_on && v.loop && v.loop->first < v.loop->second;
    const auto q = quality_ == 1 ? resample::Quality::Exact : resample::Quality::Table;
    const auto& T = b.targets;

    for (int done = 0; done < n && v.active;) {
        const int len = std::min(v.realtime ? MAXCH : CH, n - done);
        const double tm = v.t + 0.5 * static_cast<double>(len) * dt;
        bool ended = false;

        // ---- modulation -------------------------------------------------
        auto lfo_phase = [&](int i) { return T[static_cast<size_t>(i)].lfo.global
                                            ? global_time_ * T[static_cast<size_t>(i)].lfo.hz : v.phase[static_cast<size_t>(i)]; };
        auto env_part = [&](int i) -> double {
            const Target& tg = T[static_cast<size_t>(i)];
            if (!tg.env) return 0.0;
            const auto lvl = tg.env->level(tm, v.released ? std::optional<double>(v.t_release) : std::nullopt);
            return lvl ? *lvl * tg.amount : 0.0;
        };
        auto lfo_part = [&](int i) { return T[static_cast<size_t>(i)].lfo.value(tm, lfo_phase(i)); };

        double vol = 1.0;
        if (T[VOL].env) {
            const auto lvl = T[VOL].env->level(tm, v.released ? std::optional<double>(v.t_release) : std::nullopt);
            if (!lvl) { ended = true; vol = 0.0; } else vol = *lvl;
        }
        if (T[VOL].lfo.active()) vol *= std::max(0.0, 1.0 + lfo_part(VOL));

        const double pan  = env_part(PAN) + lfo_part(PAN);
        const double modx = env_part(MODX) + lfo_part(MODX);
        const double mody = env_part(MODY) + lfo_part(MODY);
        const double pitch_cents = env_part(PITCH) * 2400.0 + lfo_part(PITCH) * 1200.0;

        const double pitch = std::exp2((v.base_cents + b.cents + pitch_cents) / 1200.0);
        const double time_speed = v.src_ratio / v.length_ratio;
        const double step = std::clamp(v.realtime ? time_speed : time_speed * pitch, 1.0 / 64.0, 48.0);

        // ---- read source ------------------------------------------------
        int valid = len;
        if (v.realtime) {
            auto& st = *streams_[static_cast<size_t>(v.stream_idx)];
            st.setTranspose(pitch / v.src_ratio * std::exp2(b.ts_cents / 1200.0));
            v.in_acc += static_cast<double>(len) * time_speed;
            const int k = std::max(1, static_cast<int>(v.in_acc));
            v.in_acc = std::max(0.0, v.in_acc - static_cast<double>(k));
            const auto& L = v.ram->left;
            const auto& R = v.ram->right;
            for (int i = 0; i < k; ++i) {
                int64_t idx = v.in_pos + i;
                if (loop_on && idx > static_cast<int64_t>(v.loop->second))
                    idx = static_cast<int64_t>(v.loop->first)
                        + (idx - static_cast<int64_t>(v.loop->second) - 1) % static_cast<int64_t>(v.loop->second - v.loop->first + 1);
                const bool ok = idx >= 0 && idx < static_cast<int64_t>(v.frames);
                in_l_[static_cast<size_t>(i)] = ok ? L[static_cast<size_t>(idx)] : 0.0f;
                in_r_[static_cast<size_t>(i)] = ok ? R[static_cast<size_t>(idx)] : 0.0f;
            }
            v.in_pos += k;
            if (loop_on && v.in_pos > static_cast<int64_t>(v.loop->second))
                v.in_pos = static_cast<int64_t>(v.loop->first)
                         + (v.in_pos - static_cast<int64_t>(v.loop->second) - 1) % static_cast<int64_t>(v.loop->second - v.loop->first + 1);
            st.process(in_l_.data(), in_r_.data(), static_cast<size_t>(k), out_l_.data(), out_r_.data(), static_cast<size_t>(len));
            for (int i = 0; i < len; ++i) { buf[2 * i] = out_l_[static_cast<size_t>(i)]; buf[2 * i + 1] = out_r_[static_cast<size_t>(i)]; }
            if (!loop_on && v.in_pos >= static_cast<int64_t>(v.frames)) {
                v.tail += static_cast<size_t>(len);
                if (v.tail > st.delay() * 2 + 2048) ended = true;
            }
        } else {
            for (int i = 0; i < len; ++i) {
                float l = 0.0f, r = 0.0f;
                if (v.disk) {
                    if (!v.disk->read(v.feed, v.pos, step, quality_, wait_, l, r)) { l = 0.0f; r = 0.0f; }
                } else if (loop_on && v.pos + static_cast<double>(resample::half_width(step)) >= static_cast<double>(v.loop->second)) {
                    std::tie(l, r) = read_looped(*v.ram, v.loop->first, v.loop->second, v.pos, step, q);
                } else {
                    std::tie(l, r) = resample::read(v.ram->left.data(), v.ram->right.data(),
                                                    static_cast<int>(v.frames), v.pos, step, q);
                }
                buf[2 * i] = l;
                buf[2 * i + 1] = r;
                v.pos += step;
                if (loop_on) {
                    const double e1 = static_cast<double>(v.loop->second) + 1.0;
                    if (v.pos >= e1) {
                        const double span = e1 - static_cast<double>(v.loop->first);
                        v.pos = static_cast<double>(v.loop->first) + std::fmod(v.pos - e1, span);
                    }
                } else if (v.pos >= static_cast<double>(v.frames)) {
                    valid = i + 1;
                    ended = true;
                    break;
                }
            }
            for (int i = valid; i < len; ++i) { buf[2 * i] = 0.0f; buf[2 * i + 1] = 0.0f; }
        }

        // ---- filter -----------------------------------------------------
        const float cut = std::clamp(b.f_cut + static_cast<float>(modx), 0.0f, 1.0f);
        const float res = std::clamp(b.f_res + static_cast<float>(mody), 0.0f, 1.0f);
        v.filter.tick(b.f_type, cut, res, static_cast<double>(len));
        v.filter.run(buf, len);

        // ---- gain / declick ----------------------------------------------
        double g = static_cast<double>(v.vel) * vol;
        if (v.dk.open_ms > 0.0) g *= std::min(1.0, tm / (v.dk.open_ms * 0.001));
        if (v.released && !T[VOL].env) {
            const double c = 1.0 - (tm - v.t_release) / std::max(0.0015, v.dk.close_ms * 0.001);
            if (c <= 0.0) { ended = true; g = 0.0; } else g *= c;
        }
        if (v.killing) {
            const double c = 1.0 - (tm - v.t_kill) / v.kill_len;
            if (c <= 0.0) { ended = true; g = 0.0; } else g *= c;
        }
        const auto [pl, pr] = pan_gains(pan);
        const float tl = static_cast<float>(g) * pl * v.filter.gain();
        const float tr = static_cast<float>(g) * pr * v.filter.gain();
        if (v.fresh) { v.gl = tl; v.gr = tr; v.fresh = false; }
        const float dl = (tl - v.gl) / static_cast<float>(len);
        const float dr = (tr - v.gr) / static_cast<float>(len);
        float gl = v.gl, gr = v.gr;
        for (int i = 0; i < len; ++i) {
            gl += dl; gr += dr;
            outL[done + i] += buf[2 * i] * gl;
            outR[done + i] += buf[2 * i + 1] * gr;
        }
        v.gl = tl; v.gr = tr;

        // ---- advance ----------------------------------------------------
        v.t += static_cast<double>(len) * dt;
        for (size_t i = 0; i < 5; ++i)
            if (T[i].lfo.active() && !T[i].lfo.global) v.phase[i] += T[i].lfo.hz * static_cast<double>(len) * dt;
        done += len;
        if (ended) free_voice(v);
    }
}

} // namespace as
