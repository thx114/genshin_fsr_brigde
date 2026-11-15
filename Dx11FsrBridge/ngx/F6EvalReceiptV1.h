#pragma once

#include "../third_party/ngx/nvsdk_ngx_defs.h"
#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace neo::ngx
{
inline constexpr char k_f6_evaluate_v1[] = "F6_NGX_D3D11_EvaluateFeatureV1";

enum class F6OutputStatus : std::uint32_t
{
    unproven = 0,
    no_output = 1,
    current_output_ordered = 2,
};

// x64 ABI。调用方在调用前初始化 size/version；不匹配时提供方在求值前拒绝，不修改回执。
// 有效回执在可能早退前先初始化；不得保存指针。身份使用入口原始 ABI 参数，不换成内部对象。
// current_output_ordered 仅指本次提交、回程同步及目标写入已排入该 context 的顺序，
// 不是 CPU 观察到完成，也不授权释放资源。no_output 同样不表示没有 GPU 工作。
struct F6EvalReceiptV1
{
    std::uint32_t size = sizeof(F6EvalReceiptV1);
    std::uint32_t version = 1;
    std::uint64_t request_id = 0;
    std::uint64_t handle = 0;
    std::uint64_t context = 0;
    std::uint64_t output = 0;
    std::uint64_t provider_frame = 0; // 仅诊断，跨 feature 不保证单调。
    F6OutputStatus status = F6OutputStatus::unproven;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(F6EvalReceiptV1) == 56 && offsetof(F6EvalReceiptV1, status) == 48);

using F6EvaluateV1 = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
    NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback, std::uint64_t request_id, F6EvalReceiptV1 *);

inline F6OutputStatus f6_output_status(const F6EvalReceiptV1 &r, std::uint64_t request,
    const NVSDK_NGX_Handle *handle, const ID3D11DeviceContext *context, const ID3D11Resource *output) noexcept
{
    if (r.size != sizeof(r) || r.version != 1 || r.request_id != request ||
        r.handle != reinterpret_cast<std::uintptr_t>(handle) ||
        r.context != reinterpret_cast<std::uintptr_t>(context) ||
        r.output != reinterpret_cast<std::uintptr_t>(output) || r.reserved != 0)
        return F6OutputStatus::unproven;
    switch (r.status)
    {
    case F6OutputStatus::no_output:
    case F6OutputStatus::current_output_ordered:
        return r.status;
    default:
        return F6OutputStatus::unproven;
    }
}
}
