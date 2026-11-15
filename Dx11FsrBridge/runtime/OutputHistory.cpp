#include "OutputHistory.h"

namespace neo::graphics
{
namespace
{
bool can_copy(ID3D11DeviceContext &context, ID3D11Texture2D &source, ID3D11Texture2D &target) noexcept
{
    D3D11_TEXTURE2D_DESC src {}, dst {};
    source.GetDesc(&src);
    target.GetDesc(&dst);
    if (src.Width != dst.Width || src.Height != dst.Height || src.Format != dst.Format ||
        src.MipLevels != dst.MipLevels || src.ArraySize != dst.ArraySize ||
        src.SampleDesc.Count != dst.SampleDesc.Count || src.SampleDesc.Quality != dst.SampleDesc.Quality ||
        dst.Usage == D3D11_USAGE_IMMUTABLE)
        return false;

    // 双缓冲跨设备重建时尺寸可能不变；不能因为描述相同就向旧设备的纹理发 CopyResource。
    Microsoft::WRL::ComPtr<ID3D11Device> execution_device, src_device, dst_device;
    context.GetDevice(execution_device.GetAddressOf());
    source.GetDevice(src_device.GetAddressOf());
    target.GetDevice(dst_device.GetAddressOf());
    return execution_device && execution_device.Get() == src_device.Get() && src_device.Get() == dst_device.Get();
}
}

void OutputHistory::note_output(std::uint64_t output) noexcept
{
    if (!output || owns(output)) return;
    if (!a_) a_ = output;
    else if (!b_) b_ = output;
    else
    {
        b_ = a_;
        a_ = output;
    }
}

void OutputHistory::publish(ID3D11Texture2D &canonical, std::uint64_t generation, std::uint64_t output) noexcept
{
    canonical_ = &canonical;
    generation_ = generation;
    note_output(output);
}

void OutputHistory::invalidate_frame() noexcept
{
    canonical_.Reset();
    generation_ = 0;
    repair_generation_ = 0;
}

void OutputHistory::forget_instance() noexcept
{
    invalidate_frame();
    a_ = b_ = 0;
}

bool OutputHistory::owns(std::uint64_t output) const noexcept
{
    return output != 0 && (output == a_ || output == b_);
}

bool OutputHistory::can_repair(std::uint64_t generation) const noexcept
{
    return generation != 0 && canonical_ && generation_ == generation && repair_generation_ != generation;
}

std::optional<OutputHistory::Delivery> OutputHistory::deliver(ID3D11DeviceContext &context,
    ID3D11Texture2D &target, std::uint64_t generation, std::uint64_t output, Mode mode) noexcept
{
    const bool repair = mode == Mode::Repair;
    if (!generation || !canonical_ || generation_ != generation ||
        (repair && (!owns(output) || !can_repair(generation))))
        return std::nullopt;
    auto *const source = canonical_.Get();
    const bool same = source == &target;
    if (!same)
    {
        if (!can_copy(context, *source, target)) return std::nullopt;
        context.CopyResource(&target, source);
    }
    if (repair) repair_generation_ = generation;
    note_output(output);
    return Delivery { source, same };
}
}
