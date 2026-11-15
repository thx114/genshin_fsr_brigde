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
    std::printf("DrawSequence: %u failures\n", failures);
    return failures != 0;
}
