#include "../runtime/FrameAttribution.h"

#include <algorithm>
#include <array>
#include <cstdio>

using neo::graphics::AttributionPath;
using neo::graphics::FrameEvidence;
using neo::graphics::attribute_frame;
using Entry = il2cpp_callsite::RenderSnapshot::Entry;

int main()
{
    constexpr std::uint64_t now = 430, window = 50, color = 9;
    Entry a { 1 }, b { 2 };
    a.dimensions_match = b.dimensions_match = true;
    a.pending.generation = 7;
    b.pending.generation = 8;
    std::array<FrameEvidence, 2> evidence {{ { &a, 0, 0, true, false }, { &b, 0, 0, false, false } }};
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool ok, const char *what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    };
    const auto select = [&] { return attribute_frame(evidence, color, now, window); };
    auto match = select();
    check(!match.instance && !match.token.generation && match.path == AttributionPath::Ambiguous &&
        match.live_candidates == 1 && match.dormant_candidates == 1, "cold same-size candidates cannot claim by recency");
    std::swap(a.pending.generation, b.pending.generation);
    check(select().path == AttributionPath::Ambiguous, "reversing Render order does not authorize cold attribution");
    std::reverse(evidence.begin(), evidence.end());
    check(select().path == AttributionPath::Ambiguous, "snapshot enumeration order does not authorize attribution");
    std::reverse(evidence.begin(), evidence.end());
    std::swap(a.pending.generation, b.pending.generation);
    check(a.pending.generation == 7 && b.pending.generation == 8, "rejection preserves both original tokens");
    check(attribute_frame(evidence, 0, now, window).path == AttributionPath::Ambiguous,
        "missing source cannot choose among two tokens");

    // 2026-11-17：A 本帧换源而 B 没有记忆，旧颜色不同不能作为排除 A、把其 Draw 交给 B 的证据。
    evidence[0].previous_color = 10;
    evidence[0].last_write_tick = now;
    check(attribute_frame(evidence, 30, now, window).path == AttributionPath::Ambiguous,
        "half-warm changed source cannot be attributed to the unknown view");
    std::swap(a.pending.generation, b.pending.generation);
    std::reverse(evidence.begin(), evidence.end());
    check(attribute_frame(evidence, 30, now, window).path == AttributionPath::Ambiguous,
        "half-warm rejection is independent of Render and enumeration order");
    std::reverse(evidence.begin(), evidence.end());
    std::swap(a.pending.generation, b.pending.generation);

    evidence[0].previous_color = color;
    evidence[0].last_write_tick = now;
    evidence[1].previous_color = 10;
    evidence[1].last_write_tick = now;
    match = select();
    check(match.instance == a.instance && match.path == AttributionPath::Bootstrap && match.token.generation == 7 &&
        match.live_candidates == 1 && match.dormant_candidates == 1, "unique known source beats unrelated tokens");
    evidence[1].owns_target = true;
    match = select();
    check(!match.instance && match.path == AttributionPath::Ambiguous,
        "conflicting source and output owners cannot steal either token");
    evidence[0].owns_target = true;
    match = select();
    check(match.instance == a.instance && match.path == AttributionPath::OutputWithToken && !match.unique_output_owner,
        "unique source and output intersection resolves a shared output");
    evidence[1].previous_color = color;
    check(select().path == AttributionPath::Ambiguous, "shared source and output remain ambiguous despite newer token");
    evidence[0].owns_target = false;
    match = select();
    check(match.instance == b.instance && match.path == AttributionPath::OutputWithToken,
        "unique output resolves a shared source");

    evidence[0].previous_color = 11;
    evidence[1].previous_color = 10;
    b.pending = {};
    match = select();
    check(match.instance == a.instance && match.path == AttributionPath::Bootstrap && match.token.generation == 7,
        "tokenless previous output owner does not veto a real new Render reusing that resource");
    check(a.pending.generation == 7, "attribution remains read-only until the actual Draw consumer commits");
    evidence[0].previous_color = color;
    check(select().instance == a.instance, "current source and Render outrank tokenless previous output history");
    evidence[0].previous_color = 11;
    evidence[0].owns_target = true;
    a.pending = {};
    match = select();
    check(!match.instance && !match.unique_output_owner && match.path == AttributionPath::Ambiguous,
        "ambiguous output cannot repair without a token");
    evidence[0].owns_target = false;
    check(select().instance == b.instance && select().path == AttributionPath::RepairOnly,
        "without any current token the unique output owner may still repair");
    evidence[0].owns_target = true;
    a.pending.generation = 7;
    b.pending.generation = 8;
    check(select().path == AttributionPath::Ambiguous, "shared output and two tokens still require positive identity");

    evidence[0].owns_target = evidence[1].owns_target = false;
    check(select().path == AttributionPath::Ambiguous, "all fresh source memories disagree, so no recency fallback");
    b.pending = {};
    check(select().instance == a.instance, "one current token can recover a changed source despite old memory");
    b.pending.generation = 8;
    evidence[0].last_write_tick = now - window - 1;
    match = select();
    check(match.path == AttributionPath::Ambiguous,
        "stale or different source memory cannot exclude either pending view");
    evidence[1].last_write_tick = now - window - 1;
    check(select().path == AttributionPath::Ambiguous, "two stale memories cannot authenticate a view");
    evidence[0].previous_color = color;
    check(select().path == AttributionPath::Ambiguous, "stale matching source is not positive evidence");
    b.dimensions_match = false;
    evidence[1].owns_target = true;
    match = select();
    check(match.instance == a.instance && !match.unique_output_owner,
        "mismatched display cannot claim a token or repair an owned output");
    check(b.pending.generation == 8, "attribution rejection leaves the other token intact");
    check(attribute_frame(evidence, 0, now, window).instance == a.instance,
        "one compatible token can bootstrap without source memory");
    a.dimensions_match = false;
    check(!select().instance, "all incompatible Render dimensions pass through before takeover");
    check(!neo::graphics::recently_observed(now + 1, now, window) &&
        neo::graphics::recently_observed(now - window, now, window), "recency uses one snapshot clock");
    std::printf("FrameAttribution: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
