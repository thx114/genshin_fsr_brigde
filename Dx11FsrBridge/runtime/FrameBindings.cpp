#include "FrameBindings.h"

namespace neo::graphics
{
namespace
{
FrameBindings::Resource acquire_resource(ID3D11View *view) noexcept
{
    FrameBindings::Resource resource;
    if (view)
        view->GetResource(resource.GetAddressOf());
    return resource;
}
}

FrameBindings::FrameBindings(ID3D11DeviceContext &context, Extent extent) noexcept
    : context_(context), input_count_(extent == Extent::signature ? 7u : 8u),
      target_count_(extent == Extent::signature ? 2u : 4u)
{
    context_.OMGetRenderTargets(target_count_, targets_.data(), nullptr);
}

FrameBindings::~FrameBindings()
{
    for (auto *view : targets())
        if (view) view->Release();
    if (read_inputs_)
        for (auto *view : std::span(inputs_).first(input_count_))
            if (view) view->Release();
}

std::span<ID3D11RenderTargetView *const> FrameBindings::targets() const noexcept
{
    return std::span(targets_).first(target_count_);
}

std::span<ID3D11ShaderResourceView *const> FrameBindings::inputs() noexcept
{
    if (!read_inputs_)
    {
        context_.PSGetShaderResources(0, input_count_, inputs_.data());
        read_inputs_ = true;
    }
    return std::span(inputs_).first(input_count_);
}

FrameBindings::Resource FrameBindings::target_resource(UINT slot) const noexcept
{
    return slot < target_count_ ? acquire_resource(targets_[slot]) : Resource {};
}

FrameBindings::Resource FrameBindings::input_resource(UINT slot) noexcept
{
    return slot < input_count_ ? acquire_resource(inputs()[slot]) : Resource {};
}

FrameBindings::Texture FrameBindings::texture_2d(const Resource &resource) noexcept
{
    Texture texture;
    if (resource)
        resource.As(&texture);
    return texture;
}
}
