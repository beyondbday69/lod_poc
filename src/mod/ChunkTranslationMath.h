#pragma once

// Pure math used by the camera-origin tracker.  Keeping this separate from
// the OpenGL code makes the matching logic testable on a host build and keeps
// per-frame allocations out of the render path's hot logging code.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lod_poc::chunkmath {

struct Vec3f {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/// POSIX-style modulo in [0, 16).  Chunk origins repeat every 16 blocks.
inline float positiveMod16(float value) {
    float r = std::fmod(value, 16.0f);
    if (r < 0.0f) {
        r += 16.0f;
    }
    if (r >= 16.0f) {
        r -= 16.0f;
    }
    return r;
}

inline float circularDistance16(float a, float b) {
    const float d = std::fabs(positiveMod16(a) - positiveMod16(b));
    return std::min(d, 16.0f - d);
}

/// Signed circular difference in [-8, +8). target - current
inline float circularDifference16(float target, float current) {
    float diff = positiveMod16(target) - positiveMod16(current);
    if (diff > 8.0f) diff -= 16.0f;
    if (diff < -8.0f) diff += 16.0f;
    return diff;
}

struct ShiftMatch {
    bool found{false};
    Vec3f shift{};
    int matches{0};
    int previousCount{0};
    int currentCount{0};
    int candidateCount{0};
};

namespace detail {

struct BinKey {
    int64_t x{0};
    int64_t y{0};
    int64_t z{0};

    bool operator==(const BinKey &other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BinKeyHash {
    std::size_t operator()(const BinKey &key) const {
        // 64-bit mix; collision handling is still done by unordered_map.
        auto mix = [](uint64_t v) -> uint64_t {
            v += 0x9e3779b97f4a7c15ULL;
            v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
            v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
            return v ^ (v >> 31);
        };
        uint64_t h = mix(static_cast<uint64_t>(key.x));
        h ^= mix(static_cast<uint64_t>(key.y)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= mix(static_cast<uint64_t>(key.z)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

struct BinAccum {
    double sx{0.0};
    double sy{0.0};
    double sz{0.0};
    int count{0};
};

struct GridKey {
    int64_t x{0};
    int64_t y{0};
    int64_t z{0};

    bool operator==(const GridKey &other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridKeyHash {
    std::size_t operator()(const GridKey &key) const {
        return BinKeyHash{}(BinKey{key.x, key.y, key.z});
    }
};

inline BinKey binKey(const Vec3f &v, float inverseBinSize) {
    return BinKey{
        static_cast<int64_t>(std::llround(static_cast<double>(v.x) * inverseBinSize)),
        static_cast<int64_t>(std::llround(static_cast<double>(v.y) * inverseBinSize)),
        static_cast<int64_t>(std::llround(static_cast<double>(v.z) * inverseBinSize)),
    };
}

inline GridKey gridKey(const Vec3f &v, float inverseCellSize) {
    return GridKey{
        static_cast<int64_t>(std::floor(static_cast<double>(v.x) * inverseCellSize)),
        static_cast<int64_t>(std::floor(static_cast<double>(v.y) * inverseCellSize)),
        static_cast<int64_t>(std::floor(static_cast<double>(v.z) * inverseCellSize)),
    };
}

inline float lengthSquared(const Vec3f &v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

} // namespace detail

/**
 * Find the 3-D translation shift that maps the largest number of previous
 * chunk-origin translations onto the current frame.
 *
 * Chunk translations are camera-relative and move together as the camera
 * moves, while chunks enter/leave the visible set.  We therefore sample a few
 * nearby previous translations, histogram their pair differences with every
 * current translation to obtain candidate shifts, and then score the best
 * candidates against the full previous set using a small spatial hash with a
 * tolerance.  This avoids requiring all chunks to survive between frames.
 */
inline ShiftMatch findBestShift(const std::vector<Vec3f> &previous,
                                const std::vector<Vec3f> &current) {
    ShiftMatch result;
    result.previousCount = static_cast<int>(previous.size());
    result.currentCount = static_cast<int>(current.size());

    if (previous.size() < 3 || current.size() < 3) {
        return result;
    }

    constexpr std::size_t kMaxPreviousSamples = 32;
    constexpr std::size_t kMaxCandidates = 12;
    constexpr float kBinSize = 0.05f;
    constexpr float kMatchEpsilon = 0.02f;
    constexpr float kCellSize = 0.25f;
    constexpr float kInverseBinSize = 1.0f / kBinSize;
    constexpr float kInverseCellSize = 1.0f / kCellSize;

    // Prefer previous chunks that were near the camera: they are the ones
    // most likely to remain visible on the next frame.
    std::vector<std::size_t> order(previous.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const std::size_t sampleCount = std::min(kMaxPreviousSamples, previous.size());
    if (sampleCount < previous.size()) {
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(sampleCount),
                          order.end(), [&](std::size_t a, std::size_t b) {
                              return detail::lengthSquared(previous[a]) <
                                     detail::lengthSquared(previous[b]);
                          });
    }

    std::unordered_map<detail::BinKey, detail::BinAccum, detail::BinKeyHash> bins;
    bins.reserve(sampleCount * current.size() * 2 + 16);

    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
        const Vec3f &p = previous[order[sample]];
        for (const Vec3f &q : current) {
            const Vec3f delta{q.x - p.x, q.y - p.y, q.z - p.z};
            // REJECT any delta > 2.5 blocks. Chunks are spaced by multiples of 16 blocks.
            // Any pair with |delta| >= 2.5 is pairing different chunks (grid aliasing)
            // which caused catastrophic 64-block and 80-block jumps!
            if (std::fabs(delta.x) > 2.5f || std::fabs(delta.y) > 2.5f || std::fabs(delta.z) > 2.5f) {
                continue;
            }
            auto &accum = bins[detail::binKey(delta, kInverseBinSize)];
            accum.sx += delta.x;
            accum.sy += delta.y;
            accum.sz += delta.z;
            ++accum.count;
        }
    }

    struct Candidate {
        Vec3f shift{};
        int votes{0};
    };

    std::vector<Candidate> candidates;
    candidates.reserve(bins.size());
    for (const auto &entry : bins) {
        const auto &accum = entry.second;
        if (accum.count < 2) {
            continue;
        }
        const float inverseCount = 1.0f / static_cast<float>(accum.count);
        candidates.push_back(Candidate{
            Vec3f{
                static_cast<float>(accum.sx * inverseCount),
                static_cast<float>(accum.sy * inverseCount),
                static_cast<float>(accum.sz * inverseCount),
            },
            accum.count,
        });
    }

    if (candidates.empty()) {
        return result;
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.votes != b.votes) {
            return a.votes > b.votes;
        }
        return detail::lengthSquared(a.shift) < detail::lengthSquared(b.shift);
    });
    if (candidates.size() > kMaxCandidates) {
        candidates.resize(kMaxCandidates);
    }
    result.candidateCount = static_cast<int>(candidates.size());

    std::unordered_map<detail::GridKey, std::vector<Vec3f>, detail::GridKeyHash> currentGrid;
    currentGrid.reserve(current.size() * 2 + 16);
    for (const Vec3f &q : current) {
        currentGrid[detail::gridKey(q, kInverseCellSize)].push_back(q);
    }

    auto countMatches = [&](const Vec3f &shift) -> int {
        int matches = 0;
        for (const Vec3f &p : previous) {
            const Vec3f target{p.x + shift.x, p.y + shift.y, p.z + shift.z};
            const detail::GridKey base = detail::gridKey(target, kInverseCellSize);
            bool found = false;
            for (int dx = -1; dx <= 1 && !found; ++dx) {
                for (int dy = -1; dy <= 1 && !found; ++dy) {
                    for (int dz = -1; dz <= 1 && !found; ++dz) {
                        const auto it = currentGrid.find(detail::GridKey{
                            base.x + dx, base.y + dy, base.z + dz});
                        if (it == currentGrid.end()) {
                            continue;
                        }
                        for (const Vec3f &q : it->second) {
                            if (std::fabs(q.x - target.x) <= kMatchEpsilon &&
                                std::fabs(q.y - target.y) <= kMatchEpsilon &&
                                std::fabs(q.z - target.z) <= kMatchEpsilon) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (found) {
                ++matches;
            }
        }
        return matches;
    };

    int bestMatches = 0;
    float bestLengthSquared = 0.0f;
    const Candidate *best = nullptr;
    for (const Candidate &candidate : candidates) {
        const int matches = countMatches(candidate.shift);
        const float lengthSq = detail::lengthSquared(candidate.shift);
        if (matches > bestMatches || (matches == bestMatches && (best == nullptr || lengthSq < bestLengthSquared))) {
            best = &candidate;
            bestMatches = matches;
            bestLengthSquared = lengthSq;
        }
    }

    result.matches = bestMatches;
    if (best != nullptr) {
        result.shift = best->shift;
    }

    const int smallerSet = std::min(result.previousCount, result.currentCount);
    result.found = bestMatches >= 3 && bestMatches * 4 >= smallerSet;
    return result;
}

} // namespace lod_poc::chunkmath
