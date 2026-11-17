#include "../ngx/DiagnosticSampling.h"

#include <array>
#include <cstdio>

int main()
{
    using namespace neo::ngx;
    for (std::uint64_t frame = 1; frame <= 8; ++frame)
        if (!sample_jitter(frame)) return 1;
    for (const unsigned phases : { 8u, 16u, 24u, 32u, 64u, 128u })
    {
        std::array<bool, 128> seen {};
        for (unsigned sample = 1; sample <= phases; ++sample)
        {
            const auto frame = sample * k_jitter_sample_period;
            if (!sample_jitter(frame) || sample_jitter(frame + 1) || seen[frame % phases]) return 1;
            seen[frame % phases] = true;
        }
    }
    std::puts("NGX diagnostic sampling covers every supported jitter phase");
    return 0;
}
