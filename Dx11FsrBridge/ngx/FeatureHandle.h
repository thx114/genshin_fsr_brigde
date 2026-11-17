#pragma once
#include "../third_party/ngx/nvsdk_ngx_defs.h"
#include "GuardedCall.h"

namespace neo::ngx
{
// 只有 Create 成功转移所有权；不能释放失败时 provider 可能返回的非自有 handle。
class FeatureHandle
{
public:
    using Release = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);
    FeatureHandle() = default;
    ~FeatureHandle() { (void)retire(); }
    FeatureHandle(const FeatureHandle &) = delete;
    FeatureHandle &operator=(const FeatureHandle &) = delete;

    bool adopt(NVSDK_NGX_Result result, NVSDK_NGX_Handle *handle, Release release) noexcept
    {
        if (handle_ || result != NVSDK_NGX_Result_Success || !handle || !release) return false;
        handle_ = handle;
        release_ = release;
        return true;
    }
    [[nodiscard]] NVSDK_NGX_Result retire() noexcept
    {
        if (!handle_ || retire_result_ != NVSDK_NGX_Result_Success) return retire_result_;
        retire_result_ = call_provider(retire_exception_, release_, handle_);
        // 拒绝退休不授权复用池，也不推定再次 Release 能安全重试。
        if (retire_result_ == NVSDK_NGX_Result_Success) handle_ = nullptr;
        return retire_result_;
    }
    NVSDK_NGX_Handle *get() const noexcept { return handle_; }
    unsigned long retire_exception() const noexcept { return retire_exception_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    NVSDK_NGX_Handle *handle_ = nullptr;
    Release release_ = nullptr;
    NVSDK_NGX_Result retire_result_ = NVSDK_NGX_Result_Success;
    unsigned long retire_exception_ = 0;
};
}
