#include "CreateFunctionRoutes.h"
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
using Function = int(WINAPI *)(int);
int WINAPI observe(int value);
using Routes = CreateFunctionRoutes<Function, &observe>;
std::atomic<unsigned> failures {};
Function native_entry = nullptr;
Function proxy_entry = nullptr;

int WINAPI native(int value)
{
    return value + 11;
}

int WINAPI proxy(int value)
{
    return native_entry(value) + 101;
}

int WINAPI observe(int value)
{
    const Function before = Routes::original();
    const int result = before(value);
    if (Routes::original() != before)
        ++failures;
    return result;
}

int WINAPI observe_small(int value);
using SmallRoutes = CreateFunctionRoutes<Function, &observe_small, 2>;
int WINAPI observe_small(int value)
{
    return SmallRoutes::original()(value);
}

int WINAPI third(int value)
{
    return value + 303;
}

void expect(bool condition, const char *description)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}
}

int main()
{
    native_entry = Routes::bind(&native);
    proxy_entry = Routes::bind(&proxy);
    expect(native_entry != nullptr && proxy_entry != nullptr && native_entry != proxy_entry,
        "distinct originals retain distinct callable entries");
    expect(native_entry(7) == 18 && proxy_entry(7) == 119, "nested proxy keeps its own result");
    expect(Routes::original() == nullptr, "outermost call restores empty context");
    expect(Routes::bind(native_entry) == native_entry, "IAT rescan does not wrap an existing entry");
    expect(Routes::bind(&native) == native_entry, "GetProcAddress and IAT share stable entry");

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([i]
        {
            for (int value = 0; value < 2000; ++value)
            {
                const Function entry = Routes::bind(i % 2 ? &proxy : &native);
                if (entry(value) != value + (i % 2 ? 112 : 11) || Routes::original() != nullptr)
                    ++failures;
            }
        });
    for (auto &thread : threads)
        thread.join();

    expect(SmallRoutes::bind(&native)(3) == 14, "first limited route works");
    expect(SmallRoutes::bind(&proxy)(3) == 115, "second limited route works");
    expect(SmallRoutes::bind(&third) == nullptr, "exhaustion never substitutes a different original");
    expect(SmallRoutes::bind(nullptr) == nullptr, "null original remains null");
    std::printf("CreateFunctionRoutes: 16000 concurrent calls, nested restoration, rescan and capacity: %u failures\n",
        failures.load());
    return failures.load() ? 1 : 0;
}
