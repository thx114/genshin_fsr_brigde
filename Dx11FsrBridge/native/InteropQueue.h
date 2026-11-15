#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>

namespace neo::native
{
// 2026-11-15：输入/输出两条单写者时间线。GPU 完成 ticket 同时负责 allocator 和实例退役，
// 不另发一个等价的 ring Signal。所有调用在 native 后端锁内。
class InteropQueue
{
public:
    struct Submission
    {
        UINT64 ticket = 0;
        bool signal_ordered = false;
    };

    bool initialize(ID3D11Device5 &d11, ID3D12Device &d12) noexcept;
    bool available() const noexcept { return queue_ && !faulted_; }
    ID3D12GraphicsCommandList *begin() noexcept;
    void discard() noexcept;
    Submission submit(ID3D11DeviceContext4 &producer) noexcept;
    bool order_before_use(ID3D11DeviceContext4 &consumer, UINT64 ticket) const noexcept;
    bool retire(UINT64 ticket) const noexcept;
    bool drain() noexcept;

private:
    struct SharedFence
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> d12;
        Microsoft::WRL::ComPtr<ID3D11Fence> d11;

        bool create(ID3D11Device5 &device11, ID3D12Device &device12) noexcept;
    };
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 ticket = 0;
    };
    struct CloseEvent
    {
        void operator()(void *handle) const noexcept { CloseHandle(handle); }
    };

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    SharedFence input_, output_;
    std::unique_ptr<void, CloseEvent> event_;
    std::array<Slot, 3> slots_;
    std::size_t cursor_ = 0;
    Slot *recording_ = nullptr;
    UINT64 input_serial_ = 0, output_serial_ = 0;
    bool faulted_ = false;
};
}
