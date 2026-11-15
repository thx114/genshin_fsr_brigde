#include "ffx_api.h"
#include "ffx_upscale.h"

// 不模拟 SDK 成功：让桥经过上下文创建和实际资源准备，再拒绝本帧录制。
ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *)
{
    static int identity = 0;
    *context = &identity;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *)
{
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxConfigure(ffxContext *, const ffxConfigureDescHeader *)
{
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxQuery(ffxContext *, ffxQueryDescHeader *header)
{
    if (header->type != FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    auto &query = *reinterpret_cast<ffxQueryDescGetVersions *>(header);
    if (query.versionIds) query.versionIds[0] = 1;
    if (query.versionNames) query.versionNames[0] = "2.3.4 fixture-reject";
    *query.outputCount = 1;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxDispatchDescHeader *header)
{
    if (!context || !*context || header->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        return FFX_API_RETURN_ERROR_PARAMETER;
    return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}
