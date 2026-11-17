#pragma once

#include <windows.h>
#include "../third_party/ngx/nvsdk_ngx_defs.h"

namespace neo::ngx
{
// 2026-11-17（上游 27a977a/c6bcc1b）：第三方异常不能越过桥；独立栈帧仅传标量/指针，
// 让外层 COM/参数作用域正常析构。异常码由调用方消费，不以公共 Fail 冒充供应方返回。
template<class Function, class... Args>
__declspec(noinline) NVSDK_NGX_Result call_provider(unsigned long &exception, Function function, Args... args) noexcept
{
    exception = 0;
    __try
    {
        return function(args...);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        exception = GetExceptionCode();
        return NVSDK_NGX_Result_Fail;
    }
}
}
