#include "../runtime/FrameAttribution.h"

#include <cstdio>

using neo::graphics::AttributionPath;
using neo::graphics::FrameEvidence;
using neo::graphics::attribute_frame;
using Entry = il2cpp_callsite::RenderSnapshot::Entry;

int main()
{
    constexpr std::uint64_t now = 430, window = 50, color = 9;
    Entry a { 1 }, b { 2 }, c { 3 };
    a.dimensions_match = b.dimensions_match = c.dimensions_match = true;
    a.pending.generation = 7;
    b.pending.generation = 8;
    c.pending.generation = 9;
    FrameEvidence evidence[] {
        { &a, color, now, true, false },
        { &b, 10, now, true, false },
        { &c, 0, 0, false, false },
    };
    unsigned failures = 0;
    const auto check = [&](bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    };
    const auto select = [&] { return attribute_frame(evidence, color, now, window); };
    auto match = select();
    check(match.instance == a.instance && match.path == AttributionPath::Bootstrap && match.token.generation == 7 &&
        match.live_candidates == 2 && match.dormant_candidates == 1, "known source beats later unrelated tokens");
    evidence[1].owns_target = true;
    match = select();
    check(match.instance == b.instance && match.unique_output_owner == b.instance &&
        match.path == AttributionPath::OutputWithToken && !match.live_candidates, "output and token outrank source");
    b.pending = {};
    match = select();
    check(match.instance == a.instance && match.unique_output_owner == b.instance && match.live_candidates == 1,
        "an association without a token does not win a new dispatch or count as a token candidate");
    a.pending = c.pending = {};
    match = select();
    check(match.instance == b.instance && match.path == AttributionPath::RepairOnly && !match.token.generation,
        "unique associated output without token can only repair");
    evidence[0].owns_target = true;
    match = select();
    check(!match.instance && !match.unique_output_owner, "ambiguous output cannot repair without a token");
    a.pending.generation = 7;
    b.pending.generation = 8;
    match = select();
    check(match.instance == b.instance && match.path == AttributionPath::OutputWithToken && !match.unique_output_owner,
        "shared output does not erase real token evidence");

    evidence[0].owns_target = evidence[1].owns_target = false;
    evidence[0].previous_color = 11;
    evidence[0].last_write_tick = now - window - 1;
    match = select();
    check(match.instance == a.instance, "older unrelated color memory cannot exclude an otherwise eligible view");
    evidence[0].last_write_tick = now;
    match = select();
    check(match.instance == b.instance && match.path == AttributionPath::Bootstrap,
        "when all source memories disagree, preserve original newest-generation bootstrap");
    check(attribute_frame(evidence, 0, now, window).instance == b.instance,
        "missing source provides no preference");
    b.dimensions_match = false;
    evidence[1].owns_target = true;
    match = select();
    check(match.instance == a.instance && !match.unique_output_owner,
        "mismatched display cannot claim a token or repair an owned output");
    check(b.pending.generation == 8, "attribution rejection leaves the other token intact");
    a.dimensions_match = c.dimensions_match = false;
    check(!select().instance, "all incompatible Render dimensions pass through before takeover");
    check(!neo::graphics::recently_observed(now + 1, now, window) &&
        neo::graphics::recently_observed(now - window, now, window), "recency uses one snapshot clock");
    std::printf("FrameAttribution: %u failures\n", failures);
    return failures != 0;
}
