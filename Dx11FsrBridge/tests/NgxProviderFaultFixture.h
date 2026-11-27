#pragma once

namespace neo::tests
{
enum class NgxProviderFault : unsigned
{
    none,
    init,
    create,
    evaluate,
    release,
    main_caps,
    query_caps,
    destroy_query,
    hudless_evaluate,
};

struct NgxParameterCalls
{
    unsigned get_caps = 0;
    unsigned destroy = 0;
    unsigned wrong_destroy = 0;
    unsigned shutdown = 0;
};
}
