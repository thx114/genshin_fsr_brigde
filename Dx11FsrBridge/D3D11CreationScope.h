#pragma once

// 2026-11-14：实际 DLL 宿主检出 proxy 返回前内层对象已被装钩。两个创建 API
// 共用一次观察事务，等包装器完成后再接入；不改变对象身份或每帧绘制路径。
class D3D11CreationScope
{
    inline static thread_local const D3D11CreationScope *active_ = nullptr;
    const D3D11CreationScope *previous_;

public:
    D3D11CreationScope() noexcept : previous_(active_)
    {
        active_ = this;
    }

    ~D3D11CreationScope()
    {
        active_ = previous_;
    }

    D3D11CreationScope(const D3D11CreationScope &) = delete;
    D3D11CreationScope &operator=(const D3D11CreationScope &) = delete;

    bool outermost() const noexcept { return previous_ == nullptr; }
};
