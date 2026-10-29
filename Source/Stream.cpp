// Stream.cpp — disk-backed playback (WAV / AIFF streaming with a reader thread).

#include "PluginProcessor.h"

// ============================================================================
// Stream — disk-backed playback.
// ============================================================================
namespace as::stream {
namespace {

double extended(const uint8_t* b) noexcept
{
    const int exp = ((b[0] & 0x7F) << 8 | b[1]) - 16383 - 63;
    const uint64_t mant = (static_cast<uint64_t>(b[2]) << 56) | (static_cast<uint64_t>(b[3]) << 48)
                         | (static_cast<uint64_t>(b[4]) << 40) | (static_cast<uint64_t>(b[5]) << 32)
                         | (static_cast<uint64_t>(b[6]) << 24) | (static_cast<uint64_t>(b[7]) << 16)
                         | (static_cast<uint64_t>(b[8]) << 8) | static_cast<uint64_t>(b[9]);
    double v = static_cast<double>(mant) * std::pow(2.0, exp);
    return (b[0] & 0x80) ? -v : v;
}

std::optional<Layout> wav(juce::File& f)
{
    std::ifstream in(f.getFullPathName().toStdString(), std::ios::binary);
    if (!in) return std::nullopt;
    uint8_t head[12];
    if (!in.read(reinterpret_cast<char*>(head), 12)) return std::nullopt;
    if (std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return std::nullopt;
    uint16_t tag = 0, channels = 0, bits = 0, align = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t chunk[8];
        if (!in.read(reinterpret_cast<char*>(chunk), 8)) break;
        const uint64_t len = u32le(chunk + 4);
        const bool isFmt = std::memcmp(chunk, "fmt ", 4) == 0;
        const bool isData = std::memcmp(chunk, "data", 4) == 0;
        if (isFmt) {
            if (len < 16 || len > (1u << 16)) return std::nullopt;
            std::vector<uint8_t> d(static_cast<size_t>(len));
            if (!in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(len))) return std::nullopt;
            if (len & 1) in.seekg(1, std::ios::cur);
            tag = u16le(d.data());
            channels = u16le(d.data() + 2);
            rate = u32le(d.data() + 4);
            align = u16le(d.data() + 12);
            bits = u16le(d.data() + 14);
            if (tag == 0xFFFE) { if (d.size() < 26) return std::nullopt; tag = u16le(d.data() + 24); }
        } else if (isData) {
            if (!tag || !channels || !rate) return std::nullopt;
            const size_t block = static_cast<size_t>(bits / 8) * channels;
            if (align != block) return std::nullopt;
            Layout l;
            l.rate = rate; l.channels = channels;
            l.frames = static_cast<size_t>(len) / block;
            l.data = static_cast<uint64_t>(in.tellg());
            switch (tag) {
                case 1:
                    switch (bits) { case 8: l.kind = 0; break; case 16: l.kind = 2; break; case 24: l.kind = 3; break; case 32: l.kind = 4; break; default: return std::nullopt; }
                    break;
                case 3:
                    switch (bits) { case 32: l.kind = 5; break; case 64: l.kind = 6; break; default: return std::nullopt; }
                    break;
                default: return std::nullopt;
            }
            return l;
        } else {
            in.seekg(static_cast<std::streamoff>(len + (len & 1)), std::ios::cur);
            if (!in) break;
        }
    }
    return std::nullopt;
}

std::optional<Layout> aiff(juce::File& f, bool aifc)
{
    std::ifstream in(f.getFullPathName().toStdString(), std::ios::binary);
    if (!in) return std::nullopt;
    uint8_t head[12];
    if (!in.read(reinterpret_cast<char*>(head), 12)) return std::nullopt;
    if (std::memcmp(head, "FORM", 4) != 0) return std::nullopt;
    if (std::memcmp(head + 8, "AIFF", 4) != 0 && std::memcmp(head + 8, "AIFC", 4) != 0) return std::nullopt;
    uint8_t kind = 0;
    bool big = true;
    uint16_t channels = 0, size = 0;
    uint32_t frames = 0, rate = 0;
    for (;;) {
        uint8_t chunk[8];
        if (!in.read(reinterpret_cast<char*>(chunk), 8)) break;
        const uint64_t len = u32be(chunk + 4);
        if (std::memcmp(chunk, "COMM", 4) == 0) {
            if (len < 18 || len > (1u << 12)) return std::nullopt;
            std::vector<uint8_t> d(static_cast<size_t>(len));
            if (!in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(len))) return std::nullopt;
            if (len & 1) in.seekg(1, std::ios::cur);
            channels = u16be(d.data());
            frames = u32be(d.data() + 2);
            size = u16be(d.data() + 6);
            const double r = extended(d.data() + 8);
            if (!(r >= 1.0 && r < 1e7)) return std::nullopt;
            rate = static_cast<uint32_t>(r);
            if (aifc) {
                const char* comp = reinterpret_cast<const char*>(d.data() + 18);
                if (std::memcmp(comp, "NONE", 4) == 0) {
                    switch (size) { case 8: kind = 1; break; case 16: kind = 2; break; case 24: kind = 3; break; case 32: kind = 4; break; default: return std::nullopt; }
                } else if (std::memcmp(comp, "sowt", 4) == 0 && size == 16) {
                    kind = 2; big = false;
                } else return std::nullopt;
            } else {
                switch (size) { case 8: kind = 1; break; case 16: kind = 2; break; case 24: kind = 3; break; case 32: kind = 4; break; default: return std::nullopt; }
            }
        } else if (std::memcmp(chunk, "SSND", 4) == 0) {
            std::vector<uint8_t> d(8);
            if (!in.read(reinterpret_cast<char*>(d.data()), 8)) return std::nullopt;
            const uint32_t offset = u32be(d.data());
            const uint64_t data_at = static_cast<uint64_t>(in.tellg()) + offset;
            const size_t block = static_cast<size_t>(size / 8) * channels;
            const uint64_t data_frames = (len - 8 - offset) / block;
            if (data_frames != frames) return std::nullopt;
            Layout l;
            l.rate = rate; l.channels = channels; l.frames = frames; l.data = data_at; l.kind = kind; l.big = big;
            return l;
        } else {
            in.seekg(static_cast<std::streamoff>(len + (len & 1)), std::ios::cur);
            if (!in) break;
        }
    }
    return std::nullopt;
}

} // namespace

size_t Layout::block() const noexcept
{
    const size_t w = (kind == 0 || kind == 1) ? 1 : (kind == 2) ? 2 : (kind == 3) ? 3 : (kind == 4 || kind == 5) ? 4 : 8;
    return w * channels;
}

std::optional<Layout> Layout::of(const juce::File& path)
{
    if (!path.existsAsFile()) return std::nullopt;
    juce::File f(path);
    uint8_t head[12];
    {
        std::ifstream in(f.getFullPathName().toStdString(), std::ios::binary);
        if (!in) return std::nullopt;
        if (!in.read(reinterpret_cast<char*>(head), 12)) return std::nullopt;
    }
    std::optional<Layout> l;
    if (std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0) {
        l = wav(f);
    } else if (std::memcmp(head, "FORM", 4) == 0) {
        const bool aifc = std::memcmp(head + 8, "AIFC", 4) == 0;
        if (std::memcmp(head + 8, "AIFF", 4) == 0 || aifc) l = aiff(f, aifc);
    }
    if (!l) return std::nullopt;
    if (l->frames == 0 || l->rate == 0 || l->data + l->frames * l->block() > static_cast<uint64_t>(path.getSize())) return std::nullopt;
    return l;
}

void Layout::read(juce::File& file, size_t at, float* left, float* right, size_t frames) const
{
    const size_t w = (kind == 0 || kind == 1) ? 1 : (kind == 2) ? 2 : (kind == 3) ? 3 : (kind == 4 || kind == 5) ? 4 : 8;
    const size_t block = w * channels;
    std::vector<uint8_t> bytes(frames * block);
    {
        std::ifstream in(file.getFullPathName().toStdString(), std::ios::binary);
        if (!in) { std::fill(left, left + frames, 0.0f); std::fill(right, right + frames, 0.0f); return; }
        in.seekg(static_cast<std::streamoff>(data + at * block));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            std::fill(left, left + frames, 0.0f); std::fill(right, right + frames, 0.0f); return;
        }
    }
    auto value = [&](const uint8_t* b) -> float {
        switch (kind) {
            case 0: return static_cast<float>(b[0]) / 128.0f - 1.0f;
            case 1: return static_cast<float>(static_cast<int8_t>(b[0])) / 128.0f;
            case 2: {
                const int16_t v = big ? static_cast<int16_t>((b[0] << 8) | b[1]) : static_cast<int16_t>((b[1] << 8) | b[0]);
                return static_cast<float>(v) / 32768.0f;
            }
            case 3: {
                const int32_t v = big ? static_cast<int32_t>((b[0] << 24) | (b[1] << 16) | (b[2] << 8))
                                      : static_cast<int32_t>((b[2] << 24) | (b[1] << 16) | (b[0] << 8));
                return static_cast<float>(v >> 8) / 8388608.0f;
            }
            case 4: {
                const int32_t v = big ? static_cast<int32_t>((static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3])
                                      : static_cast<int32_t>((static_cast<uint32_t>(b[3]) << 24) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[1]) << 8) | b[0]);
                return static_cast<float>(static_cast<double>(v) / 2147483648.0);
            }
            case 5: {
                const uint32_t u = big ? ((static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3])
                                       : ((static_cast<uint32_t>(b[3]) << 24) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[1]) << 8) | b[0]);
                float f; std::memcpy(&f, &u, 4); return f;
            }
            case 6: {
                uint64_t u = 0;
                for (int i = 0; i < 8; ++i) u = (u << 8) | b[big ? i : 7 - i];
                double d; std::memcpy(&d, &u, 8); return static_cast<float>(d);
            }
            default: return 0.0f;
        }
    };
    for (size_t i = 0; i < frames; ++i) {
        const auto* f = bytes.data() + i * block;
        left[i] = value(f);
        right[i] = (channels > 1) ? value(f + w) : left[i];
    }
}

std::optional<std::pair<size_t, uint32_t>> length(const juce::File& path)
{
    if (auto l = Layout::of(path)) return std::make_pair(l->frames, l->rate);
    return std::nullopt;
}

// ---- Disk ----------------------------------------------------------------

Disk::Disk(juce::File file, Layout l, std::shared_ptr<std::atomic<bool>> alive)
    : l_(l), file_(std::move(file)), alive_(std::move(alive)), slots_(128) {}

std::shared_ptr<Disk> Disk::open(const juce::File& path, Layout l)
{
    if (!path.existsAsFile()) return nullptr;
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto d = std::shared_ptr<Disk>(new Disk(path, l, alive));
    d->reader_ = std::thread([raw = d.get()]() { raw->run(); });  // raw: the destructor joins
    return d;
}

Disk::~Disk()
{
    closed_.store(true);
    wake();
    if (reader_.joinable()) reader_.join();
    alive_->store(false);
}

void Disk::wake() const { cv_.notify_all(); }

void Disk::read_frames(size_t start, float* left, float* right, size_t frames) const
{
    const auto k = std::min(frames, l_.frames > start ? l_.frames - start : 0);
    std::fill(left + k, left + frames, 0.0f);
    std::fill(right + k, right + frames, 0.0f);
    if (k == 0) return;
    const std::lock_guard<std::mutex> sl(file_lock_);
    l_.read(const_cast<juce::File&>(file_), start, left, right, k);
}

size_t Disk::busy() const noexcept
{
    size_t n = 0;
    for (const auto& s : slots_) if (s.busy.load()) ++n;
    return n;
}

bool Disk::settled() const
{
    for (const auto& s : slots_) {
        if (!s.busy.load()) continue;
        const auto [req, start] = unpack(s.want.load());
        const auto [g, filled] = unpack(s.have.load());
        const auto [ug, used] = unpack(s.used.load());
        const auto u = (ug == req) ? used : 0;
        const auto rest = l_.frames > start + filled ? l_.frames - start - filled : 0;
        if (!(g == req && (rest == 0 || u + RING - filled < CHUNK))) return false;
    }
    return true;
}

int Disk::take_slot()
{
    for (size_t i = 0; i < slots_.size(); ++i) {
        bool expected = false;
        if (slots_[i].busy.compare_exchange_strong(expected, true)) return static_cast<int>(i);
    }
    return -1;
}

void Disk::release_slot(int i)
{
    if (i >= 0 && static_cast<size_t>(i) < slots_.size()) slots_[static_cast<size_t>(i)].busy.store(false);
}

void Disk::run()
{
    while (!closed_.load()) {
        bool progress = false;
        for (auto& s : slots_) {
            if (!s.busy.load()) continue;
            std::unique_lock<std::mutex> lk(s.fill_lock, std::try_to_lock);
            if (!lk.owns_lock()) continue;
            progress |= fill_some(s, s.fill, CHUNK);
        }
        if (!progress) {
            std::unique_lock<std::mutex> lk(cv_lock_);
            cv_.wait_for(lk, std::chrono::milliseconds(10));
        }
    }
}

bool Disk::fill_some(Slot& slot, Fill& f, size_t min)
{
    const auto [req, start] = unpack(slot.want.load());
    if (req != f.req) { f.req = req; f.start = start; f.filled = 0; slot.have.store(pack(req, 0)); }
    const size_t at = f.start + f.filled;
    if (at >= l_.frames) return false;
    const auto [ug, used] = unpack(slot.used.load());
    const size_t u = (ug == req) ? used : 0;
    const size_t k = std::min<size_t>(u + RING - f.filled, std::min<size_t>(CHUNK, l_.frames - at));
    const size_t need = std::min<size_t>(min, l_.frames - at);
    if (k == 0 || k < need) return false;
    if (slot.ring_left.empty()) {
        slot.ring_left.assign(RING, 0.0f);
        slot.ring_right.assign(RING, 0.0f);
        ring_count_.fetch_add(1);
    }
    f.left.resize(CHUNK, 0.0f);
    f.right.resize(CHUNK, 0.0f);
    read_frames(at, f.left.data(), f.right.data(), k);
    for (size_t i = 0; i < k; ++i) {
        const size_t j = (f.filled + i) & (RING - 1);
        slot.ring_left[j] = f.left[i];
        slot.ring_right[j] = f.right[i];
    }
    f.filled += k;
    frames_read_.fetch_add(k);
    slot.have.store(pack(req, f.filled));
    return true;
}

bool Disk::fill_to(Slot& slot, uint32_t req, size_t need)
{
    for (;;) {
        const auto [g, filled] = unpack(slot.have.load());
        if (g == req && filled >= need) return true;
        if (!fill_some(slot, slot.fill, 1)) return false;
    }
}

// ---- Played --------------------------------------------------------------

Played::Played(std::shared_ptr<Disk> disk, const std::vector<size_t>& named, const Played* old)
    : disk_(std::move(disk))
{
    const size_t n = disk_ ? disk_->layout().frames : 0;
    std::vector<size_t> nn;
    for (auto p : named) if (p < n && std::find(nn.begin(), nn.end(), p) == nn.end()) nn.push_back(p);
    named_ = nn;
    if (old && old->disk_ == disk_) {
        for (auto p : old->learned_)
            if (p < n && std::find(named_.begin(), named_.end(), p) == named_.end()
             && std::find(learned_.begin(), learned_.end(), p) == learned_.end() && learned_.size() < 16)
                learned_.push_back(p);
        for (auto p : old->missed())
            if (p < n && std::find(named_.begin(), named_.end(), p) == named_.end()
             && std::find(learned_.begin(), learned_.end(), p) == learned_.end() && learned_.size() < 16)
                learned_.push_back(p);
    }
    std::vector<size_t> all = named_;
    all.insert(all.end(), learned_.begin(), learned_.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    points_ = all;
    std::vector<std::pair<size_t, size_t>> spans;
    for (auto p : points_) {
        const size_t a = (p > BEFORE) ? p - BEFORE : 0;
        const size_t b = std::min(n, p + HEAD);
        if (!spans.empty() && a <= spans.back().second) spans.back().second = std::max(spans.back().second, b);
        else spans.emplace_back(a, b);
    }
    for (auto [a, b] : spans) {
        auto h = std::make_shared<Held>();
        h->start = a;
        h->left.resize(b - a);
        h->right.resize(b - a);
        disk_->read_frames(a, h->left.data(), h->right.data(), b - a);
        held_.push_back(h);
    }
}

size_t Played::frames() const noexcept { return disk_ ? disk_->layout().frames : 0; }
uint32_t Played::rate() const noexcept { return disk_ ? disk_->layout().rate : 0; }

std::vector<size_t> Played::missed() const
{
    std::vector<size_t> out;
    for (const auto& m : missed_) {
        const auto v = m.load();
        if (v > 0) out.push_back(static_cast<size_t>(v - 1));
    }
    return out;
}

void Played::start_at(Feed& feed, double pos, bool learn) const
{
    if (!disk_) return;
    const size_t n = disk_->layout().frames;
    const size_t x = static_cast<size_t>(std::max(0.0, pos));
    if (x >= n) return;
    for (const auto& h : held_)
        if (h->start <= x && x + OVERLAP <= h->end() + 1) return;
    const int slot = disk_->take_slot();
    if (slot < 0) return;
    auto& s = disk_->slots_[static_cast<size_t>(slot)];
    const uint32_t req = static_cast<uint32_t>(Disk::unpack(s.want.load()).first + 1u) | 1u;
    s.used.store(Disk::pack(req, 0));
    s.want.store(Disk::pack(req, x > BEFORE ? x - BEFORE : 0));
    feed = Feed{ slot, req, x > BEFORE ? x - BEFORE : 0, 0 };
    disk_->wake();
    if (learn && x + 1 > 0) {
        for (auto& m : missed_) {
            uint64_t expected = 0;
            if (m.compare_exchange_strong(expected, static_cast<uint64_t>(x + 1))) { news_.store(true); break; }
            if (m.load() == static_cast<uint64_t>(x + 1)) break;
        }
    }
}

size_t Played::held_frames() const noexcept
{
    size_t n = 0;
    for (const auto& h : held_) n += h->left.size();
    return n;
}

bool Played::read(Feed& feed, double pos, double ratio, int quality, bool wait, float& l, float& r) const
{
    if (!disk_) return false;
    const size_t n = disk_->layout().frames;
    const auto q = (quality == 1) ? resample::Quality::Exact : resample::Quality::Table;
    const auto [lo, hi] = resample::reach(static_cast<int>(n), pos, ratio);
    for (;;) {
        if (feed.slot >= 0) {
            auto& s = disk_->slots_[static_cast<size_t>(feed.slot)];
            const auto [g, filled] = Disk::unpack(s.have.load());
            if (g == feed.req && lo >= feed.start + feed.released && hi < feed.start + filled && !s.ring_left.empty()) {
                const auto rel = (lo > KEEP) ? (lo - KEEP - feed.start) : 0;
                if (rel > feed.released) { feed.released = rel; s.used.store(Disk::pack(feed.req, rel)); }
                const auto s0 = feed.start;
                auto [a, bb] = resample::read_by_t(static_cast<int>(n), pos, ratio, q, [&](int i) {
                    const size_t j = (static_cast<size_t>(i) - s0) & (RING - 1);
                    return std::make_pair(s.ring_left[j], s.ring_right[j]);
                });
                l = a; r = bb;
                return true;
            }
        }
        for (const auto& h : held_) {
            if (h->start <= lo && hi < h->end()) {
                if (h->end() < n && feed.slot < 0) start_at(feed, static_cast<double>(h->end() > OVERLAP ? h->end() - OVERLAP : 0), false);
                auto [a, bb] = resample::read_by_t(static_cast<int>(n), pos, ratio, q, [&](int i) { return h->at(static_cast<size_t>(i)); });
                l = a; r = bb;
                return true;
            }
        }
        if (feed.slot < 0) start_at(feed, static_cast<double>(lo > BEFORE ? lo - BEFORE : 0), false);
        if (feed.slot < 0) return false;
        if (!wait) return false;
        auto& s = disk_->slots_[static_cast<size_t>(feed.slot)];
        const size_t need = hi + 1 - feed.start;
        std::lock_guard<std::mutex> lk(s.fill_lock);
        if (!disk_->fill_to(s, feed.req, need)) return false;
    }
}

void Played::release(Feed& feed) const
{
    if (feed.slot >= 0 && disk_) { disk_->release_slot(feed.slot); feed.slot = -1; }
}

Summary read_summary(const juce::File& path, const Layout& l)
{
    Summary s;
    const size_t n = l.frames;
    s.step = n > Summary::POINTS ? (n + Summary::POINTS - 1) / Summary::POINTS : 1;
    s.left.reserve(n / s.step + 1);
    s.right.reserve(n / s.step + 1);
    constexpr size_t PART = 1u << 16;
    std::vector<float> left(PART), right(PART);
    juce::File f(path);
    size_t at = 0;
    while (at < n) {
        const size_t k = std::min<size_t>(PART, n - at);
        l.read(f, at, left.data(), right.data(), k);
        if (left[0] != right[0]) s.stereo = true;
        size_t j = (at + s.step - 1) / s.step * s.step - at;
        while (j < k) { s.left.push_back(left[j]); s.right.push_back(right[j]); j += s.step; }
        at += k;
    }
    return s;
}

} // namespace as::stream