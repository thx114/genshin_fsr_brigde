#include "../runtime/DrawSequence.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace
{
enum class Stage { upscale, tone_map, spatial, original };

bool test(Stage handled, bool attempt_original, std::string_view expected)
{
    std::string observed;
    neo::graphics::execute_draw([&] { observed += 'D'; }, [&](auto &original)
    {
        observed += 'U';
        if (attempt_original)
            original();
        return handled == Stage::upscale;
    }, [&](auto &original)
    {
        observed += 'H';
        if (handled != Stage::tone_map)
            return false;
        original();
        return true;
    }, [&](auto &original)
    {
        observed += 'S';
        if (handled != Stage::spatial)
            return false;
        original();
        return true;
    }, [&] { observed += '['; }, [&] { observed += ']'; });
    if (observed == expected)
        return true;
    std::printf("expected %.*s, got %s\n", static_cast<int>(expected.size()), expected.data(), observed.c_str());
    return false;
}

bool test_observed_next(Stage handled, bool attempt_original, std::string_view expected)
{
    std::string observed;
    const auto draw = [&] { observed += "[D]"; };
    neo::graphics::execute_draw(draw, [&](auto &original)
    {
        observed += 'U';
        if (attempt_original) original();
        return handled == Stage::upscale;
    }, [&](auto &original)
    {
        observed += 'H';
        if (handled != Stage::tone_map) return false;
        original();
        return true;
    }, [&](auto &original)
    {
        observed += 'S';
        if (handled != Stage::spatial) return false;
        original();
        return true;
    }, [] {}, [] {});
    if (observed == expected) return true;
    std::printf("observed next expected %.*s, got %s\n", static_cast<int>(expected.size()), expected.data(),
        observed.c_str());
    return false;
}
}

int main()
{
    unsigned failures = 0;
    failures += !test(Stage::original, false, "UHS[D]");
    failures += !test(Stage::upscale, false, "U");
    failures += !test(Stage::upscale, true, "UD");
    failures += !test(Stage::tone_map, false, "UHD");
    failures += !test(Stage::spatial, false, "UHSD");
    failures += !test(Stage::original, true, "UD");
    // 2026-11-16：next 自身的观测包住业务分支内执行；最终 fallback 回调不能覆盖这些路径。
    failures += !test_observed_next(Stage::original, false, "UHS[D]");
    failures += !test_observed_next(Stage::upscale, false, "U");
    failures += !test_observed_next(Stage::upscale, true, "U[D]");
    failures += !test_observed_next(Stage::tone_map, false, "UH[D]");
    failures += !test_observed_next(Stage::spatial, false, "UHS[D]");
    failures += !test_observed_next(Stage::original, true, "U[D]");
    std::printf("DrawSequence: %u failures\n", failures);
    return failures != 0;
}
