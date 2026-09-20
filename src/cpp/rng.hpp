/* rng.hpp — deterministic, allocation-free pseudo random numbers.
 *
 * A single xorshift32 stream plus a couple of small helpers.  Everything the
 * game randomises (spawn lanes, trick selection, weight init, star field)
 * draws from these, so a seed reproduces an entire run exactly.
 */
#ifndef MINI_RNG_HPP
#define MINI_RNG_HPP

#include <cstdint>

namespace mss {

struct Rng {
    uint32_t s = 0x9E3779B9u;

    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }

    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    /* [0, 1) */
    float uni() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }

    /* [-1, 1) */
    float sym() { return uni() * 2.0f - 1.0f; }

    /* [0, n) */
    int range(int n) { return n <= 1 ? 0 : (int)(next() % (uint32_t)n); }

    /* Roughly normal, mean 0, stddev 1 (sum of three uniforms). */
    float gauss() { return (uni() + uni() + uni() - 1.5f) * 1.63299f; }

    bool chance(float p) { return uni() < p; }

    /* Uniform in [lo, hi). */
    float between(float lo, float hi) { return lo + uni() * (hi - lo); }
};

} /* namespace mss */

#endif /* MINI_RNG_HPP */
