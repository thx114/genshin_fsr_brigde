#include "InteropQueue.h"
#include "../PerfProbe.h"

#include <limits>
#include <utility>

namespace neo::native
{
namespace
{
constexpr DWORD k_retire_timeout_ms = 1000;
constexpr auto k_device_removed = (std::numeric_limits<UINT64>::max)();
}

bool InteropQueue::SharedFence::create(ID3D11Device5 &device11, ID3D12Device &device12) noexcept
{
    if (FAILED(device12.CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d12.GetAddressOf()))))
        return false;
    HANDLE handle = nullptr;
    if (FAILED(device12.CreateSharedHandle(d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        return false;
    const HRESULT opened = device11.OpenSharedFence(handle, IID_PPV_ARGS(d11.GetAddressOf()));
    CloseHandle(handle);
    return SUCCEEDED(opened);
}

bool InteropQueue::initialize(ID3D11Device5 &d11, ID3D12Device &d12) noexcept
{
    InteropQueue pending;
    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d12.CreateCommandQueue(&desc, IID_PPV_ARGS(pending.queue_.GetAddressOf()))) ||
        !pending.input_.create(d11, d12) || !pending.output_.create(d11, d12))
        return false;
    pending.event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!pending.event_)
        return false;
    for (Slot &slot : pending.slots_)
    {
        if (FAILED(d12.CreateCommandAllocator(desc.Type, IID_PPV_ARGS(slot.allocator.GetAddressOf()))) ||
            FAILED(d12.CreateCommandList(0, desc.Type, slot.allocator.Get(), nullptr,
                IID_PPV_ARGS(slot.list.GetAddressOf()))) || FAILED(slot.list->Close()))
            return false;
    }
    *this = std::move(pending);
    return true;
}

bool InteropQueue::retire(UINT64 ticket) const noexcept
{
    if (!ticket) return true;
    const UINT64 completed = output_.d12->GetCompletedValue();
    if (completed == k_device_removed) return false;
    if (completed >= ticket) return true;
    // 旧的超时事件仍可能在这次等待期间发信号；唤醒不能代替本 ticket 已完成的证明。
    ResetEvent(event_.get());
    if (FAILED(output_.d12->SetEventOnCompletion(ticket, event_.get())) ||
        perf_probe::wait_single_object_counted(event_.get(), k_retire_timeout_ms) != WAIT_OBJECT_0)
        return false;
    const UINT64 after_wait = output_.d12->GetCompletedValue();
    return after_wait != k_device_removed && after_wait >= ticket;
}

ID3D12GraphicsCommandList *InteropQueue::begin() noexcept
{
    if (!available() || recording_) return nullptr;
    Slot &slot = slots_[cursor_];
    if (!retire(slot.ticket) || FAILED(slot.allocator->Reset()) ||
        FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        return nullptr;
    cursor_ = (cursor_ + 1) % slots_.size();
    recording_ = &slot;
    return slot.list.Get();
}

void InteropQueue::discard() noexcept
{
    if (Slot *slot = std::exchange(recording_, nullptr))
        slot->list->Close();
}

InteropQueue::Submission InteropQueue::submit(ID3D11DeviceContext4 &producer) noexcept
{
    Slot *const slot = std::exchange(recording_, nullptr);
    if (!slot || FAILED(slot->list->Close())) return {};
    const UINT64 input = ++input_serial_;
    if (FAILED(producer.Signal(input_.d11.Get(), input)) || FAILED(queue_->Wait(input_.d12.Get(), input)))
        return {};

    ID3D12CommandList *lists[] = {slot->list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    slot->ticket = ++output_serial_;
    const bool signaled = SUCCEEDED(queue_->Signal(output_.d12.Get(), slot->ticket));
    // Execute 已发生；若 Signal 失败不能再复用 allocator 或把待用纹理交给调用方。
    // 仅在 queue 退役或设备移除之后才能释放这批资源。
    faulted_ = !signaled;
    return {slot->ticket, signaled};
}

bool InteropQueue::order_before_use(ID3D11DeviceContext4 &consumer, UINT64 ticket) const noexcept
{
    return ticket && available() && SUCCEEDED(consumer.Wait(output_.d11.Get(), ticket));
}

bool InteropQueue::drain() noexcept
{
    if (!queue_) return true;
    if (output_.d12->GetCompletedValue() == k_device_removed) return true;
    if (!faulted_) return retire(output_serial_);
    // 正常卸载时为上次 Execute 后 Signal 失败的 work 再试建立一个退役点。
    const UINT64 ticket = ++output_serial_;
    return SUCCEEDED(queue_->Signal(output_.d12.Get(), ticket)) && retire(ticket);
}
}
