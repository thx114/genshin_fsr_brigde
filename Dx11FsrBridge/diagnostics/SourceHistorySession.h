#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace neo::diagnostics
{
// 2026-11-17：诊断许可不能等同于永久采集；仅在按键后的有限 SDK 窗口保留候选。
// collecting() 是 draw 侧廉价门控；其余操作由宿主同一 history mutex 串行化。
class SourceHistorySession
{
public:
    enum class Phase { idle, collecting, exporting, exported, failed };
    enum class Action { none, arm, capture, expire };
    static constexpr std::size_t call_limit() noexcept { return 65536; }
    static constexpr std::size_t write_limit() noexcept { return 4096; }
    static constexpr std::size_t resource_limit() noexcept { return 128; }
    static constexpr std::size_t byte_limit() noexcept { return 16 * 1024 * 1024; }
    static constexpr std::size_t sdk_limit() noexcept { return 64; }
    static constexpr std::uint64_t window_ms() noexcept { return 2000; }

    bool collecting() const noexcept { return phase_.load(std::memory_order_relaxed) == Phase::collecting; }
    Phase phase() const noexcept { return phase_.load(std::memory_order_relaxed); }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t context() const noexcept { return context_; }
    std::uint64_t instance() const noexcept { return instance_; }
    std::uint64_t requested_frame() const noexcept { return requested_frame_; }
    std::uint64_t captured_frame() const noexcept { return captured_frame_; }
    std::size_t calls() const noexcept { return calls_; }
    std::size_t writes() const noexcept { return writes_; }
    std::size_t resources() const noexcept { return resources_; }
    std::size_t bytes() const noexcept { return bytes_; }
    bool truncated() const noexcept { return truncated_; }
    const char *reason() const noexcept { return reason_; }

    Action sdk(bool key_down, std::uint64_t context, std::uint64_t instance,
        std::uint64_t frame, std::uint64_t now_ms, bool inputs_known) noexcept
    {
        const bool pressed = key_down && !key_down_;
        key_down_ = key_down;
        const auto current = phase();
        if (current == Phase::collecting)
        {
            ++sdk_calls_;
            if (context == context_ && instance == instance_ && frame < requested_frame_)
            {
                reason_ = "matching-sdk-frame-regressed";
                phase_.store(Phase::failed, std::memory_order_relaxed);
                return Action::expire;
            }
            if (expire(now_ms) || sdk_calls_ > sdk_limit())
            {
                reason_ = "matching-sdk-window-expired";
                phase_.store(Phase::failed, std::memory_order_relaxed);
                return Action::expire;
            }
            // 目标可以双缓冲；只在同一 context/instance 的严格后续 SDK 输入点冻结。
            if (context == context_ && instance == instance_ && frame > requested_frame_ && inputs_known)
            {
                captured_frame_ = frame;
                phase_.store(Phase::exporting, std::memory_order_relaxed);
                return Action::capture;
            }
            return Action::none;
        }
        if (current == Phase::exporting || !pressed || !context || !inputs_known) return Action::none;
        ++generation_;
        context_ = context;
        instance_ = instance;
        requested_frame_ = frame;
        captured_frame_ = 0;
        started_ms_ = now_ms;
        calls_ = writes_ = resources_ = bytes_ = sdk_calls_ = 0;
        truncated_ = false;
        reason_ = "collecting-requested-window";
        phase_.store(Phase::collecting, std::memory_order_relaxed);
        return Action::arm;
    }

    // SDK 停止后由既有 Present 入口维护；无逐 draw 时钟，也不新增后台线程。
    bool expire(std::uint64_t now_ms) noexcept
    {
        if (!collecting() || (now_ms >= started_ms_ && now_ms - started_ms_ < window_ms())) return false;
        reason_ = "matching-sdk-window-expired";
        phase_.store(Phase::failed, std::memory_order_relaxed);
        return true;
    }

    bool observe_call(std::uint64_t &generation) noexcept
    {
        if (!collecting()) return false;
        if (calls_ == call_limit()) return truncate("call-limit");
        ++calls_;
        generation = generation_;
        return true;
    }

    bool reserve_write(std::uint64_t generation, bool new_resource, std::size_t bytes) noexcept
    {
        if (!collecting() || generation != generation_) return false;
        if (writes_ == write_limit()) return truncate("write-limit");
        if (new_resource && resources_ == resource_limit()) return truncate("resource-limit");
        if (bytes > byte_limit() - bytes_) return truncate("byte-limit");
        ++writes_;
        resources_ += new_resource ? 1 : 0;
        bytes_ += bytes;
        return true;
    }

    void exported(bool success) noexcept
    {
        reason_ = success ? (truncated_ ? reason_ : "window-exported") : "file-export-failed";
        phase_.store(success ? Phase::exported : Phase::failed, std::memory_order_relaxed);
    }

private:
    bool truncate(const char *reason) noexcept
    {
        truncated_ = true;
        reason_ = reason;
        phase_.store(Phase::failed, std::memory_order_relaxed);
        return false;
    }
    std::atomic<Phase> phase_ { Phase::idle };
    bool key_down_ = false, truncated_ = false;
    std::uint64_t generation_ = 0, context_ = 0, instance_ = 0;
    std::uint64_t requested_frame_ = 0, captured_frame_ = 0, started_ms_ = 0;
    std::size_t calls_ = 0, writes_ = 0, resources_ = 0, bytes_ = 0, sdk_calls_ = 0;
    const char *reason_ = "no-request";
};
}
