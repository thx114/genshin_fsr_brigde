#include "ProducerTransfer.h"
#include "../ComMethodRoutes.h"
#include "../InterfaceVtableLayout.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <d3d11_3.h>

namespace neo::producer
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr GUID k_program {0xa73eb8da, 0x945f, 0x4e81, {0x9b,0x20,0x28,0x6d,0x6d,0x4d,0xb6,0x10}};
constexpr GUID k_constants {0x5729c3d8, 0xf874, 0x4bb2, {0x8f,0x61,0x2c,0x48,0x65,0xa8,0x06,0x32}};
constexpr GUID k_contents {0x44263cb6, 0x42de, 0x4fab, {0x90,0x68,0x66,0x6c,0x21,0x01,0xa2,0x94}};
constexpr GUID k_execution {0xf1b6746e, 0x413b, 0x43ed, {0xbd,0x14,0xad,0x82,0x99,0x78,0xa3,0x43}};
constexpr GUID k_vertex {0x8a680ef0, 0x4fe8, 0x4442, {0xbe,0x66,0x1e,0x6b,0x18,0x29,0xdb,0xa4}};
constexpr GUID k_layout {0xa5295390, 0xbaf6, 0x4d8a, {0x9a,0x20,0xab,0x7c,0x5f,0x46,0xc4,0x14}};
constexpr GUID k_geometry {0x64ba12da, 0x2c45, 0x4d84, {0xb9,0x34,0x14,0xf9,0x22,0xaf,0x85,0xcc}};
constexpr UINT k_constant_bytes = 105 * 16;
constexpr std::array<UINT, 3> k_control_offsets {20 * 16, 20 * 16 + 4, 25 * 16};
std::atomic<std::uint64_t> g_opaque_epoch {1}, g_next_owner {0}, g_buffer_marks {0};
std::atomic_bool g_enabled {false};
void (*g_repair)(ID3D11DeviceContext *) = nullptr;

// private-data对象不持有宿主resource/context，避免环；其寿命由真实COM对象持有。
class PrivateObject : public IUnknown
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) noexcept override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const auto left = --references_;
        if (!left) delete this;
        return left;
    }
protected:
    virtual ~PrivateObject() = default;
private:
    std::atomic<ULONG> references_ {1};
};

template <class T, class Object>
ComPtr<T> private_object(Object *object, const GUID &key) noexcept
{
    ComPtr<T> value;
    if (!object) return value;
    UINT size = sizeof(IUnknown *);
    IUnknown *raw = nullptr;
    if (SUCCEEDED(object->GetPrivateData(key, &size, &raw)) && raw)
        value.Attach(static_cast<T *>(raw));
    return value;
}

template <class T, class Object>
ComPtr<T> add_private(Object &object, const GUID &key) noexcept
{
    auto value = private_object<T>(&object, key);
    if (value) return value;
    value.Attach(new (std::nothrow) T);
    if (value && FAILED(object.SetPrivateDataInterface(key, value.Get()))) value.Reset();
    return value;
}

bool read_memory(void *destination, const void *source, std::size_t bytes) noexcept
{
    __try
    {
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Constants final : PrivateObject
{
    std::mutex mutex;
    std::array<float, 3> values {};
    unsigned known = 0;
    std::uint64_t version = 0, epoch = 0;
    void *mapped = nullptr;
    std::uint64_t map_owner = 0;

    void write(const void *data, UINT begin, UINT end) noexcept
    {
        const auto current = g_opaque_epoch.load(std::memory_order_relaxed);
        if (epoch != current) known = 0;
        epoch = current;
        ++version;
        for (unsigned i = 0; i < k_control_offsets.size(); ++i)
        {
            const UINT offset = k_control_offsets[i];
            if (begin >= offset + sizeof(float) || end <= offset) continue;
            known &= ~(1u << i);
            if (begin <= offset && end >= offset + sizeof(float) && data &&
                read_memory(&values[i], static_cast<const std::uint8_t *>(data) + offset - begin, sizeof(float)))
                known |= 1u << i;
        }
    }
};

struct Contents final : PrivateObject
{
    std::mutex mutex;
    std::uint64_t serial = 0, owner = 0, epoch = 0, constants_version = 0;
    bool srgb = false;
    void overwritten() noexcept
    {
        std::lock_guard lock(mutex);
        ++serial;
        srgb = false;
    }
};

struct Target
{
    ComPtr<Contents> contents;
    D3D11_RENDER_TARGET_VIEW_DESC view {};
    D3D11_TEXTURE2D_DESC texture {};
};

struct Geometry final : PrivateObject
{
    std::mutex mutex;
    std::array<std::uint8_t, 4096> bytes {};
    std::array<bool, 4096> known {};
    UINT size = 0;
    std::uint64_t epoch = 0, map_owner = 0, version = 0;
    bool immutable = false;
    void *mapped = nullptr;

    void write(const void *data, UINT begin, UINT end) noexcept
    {
        const auto current = g_opaque_epoch.load(std::memory_order_relaxed);
        if (epoch != current && !immutable) known.fill(false);
        epoch = current;
        ++version;
        if (begin >= end || end > size) { known.fill(false); return; }
        const bool valid = data && read_memory(bytes.data() + begin, data, end - begin);
        std::fill(known.begin() + begin, known.begin() + end, valid);
    }
};

struct Position { UINT slot = 0, offset = 0, bytes = 0; };

struct Execution final : PrivateObject
{
    std::mutex mutex;
    const std::uint64_t owner = ++g_next_owner;
    std::array<Target, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
    std::array<ComPtr<Contents>, D3D11_1_UAV_SLOT_COUNT> graphics_uavs, compute_uavs;
};

std::uint64_t buffer_mark(ID3D11Resource *resource) noexcept
{
    return 1ull << ((reinterpret_cast<std::uintptr_t>(resource) >> 4u) & 63u);
}

ComPtr<Constants> constants(ID3D11Resource *resource) noexcept
{
    // 只作负向快筛，永远不能据地址认领；真正数据在当前COM对象的private-data里。
    if (!resource || !(g_buffer_marks.load(std::memory_order_relaxed) & buffer_mark(resource))) return {};
    return private_object<Constants>(resource, k_constants);
}

bool rgba8(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

ComPtr<Contents> view_contents(ID3D11View *view, Target *target = nullptr) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (view) view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return {};
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (!rgba8(desc.Format) || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        desc.CPUAccessFlags != 0 || desc.Usage != D3D11_USAGE_DEFAULT) return {};
    if (target) target->texture = desc;
    return add_private<Contents>(*resource.Get(), k_contents);
}

bool program_matches(const void *data, std::size_t size, bool vertex = false) noexcept
{
    // 2026-11-16：39776/44888实际PS的SHEX程序已逐条审计。只忽略容器调试/反射块，
    // 不以文件名、shader对象地址、大小或含几个gamma常数就宣称语义相同。
    // 程序中max(1.055*pow(c,1/2.4)-.055,0)与Unity PostProcessing Colors.hlsl
    // LinearToSRGB/USE_FAST_SRGB同式；cb0[20].y额外gamma和[25].x叠色须每次判定。
    if (!data || size < 36 || size > 1024 * 1024) return false;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    const auto word = [&](std::size_t offset) { std::uint32_t v; std::memcpy(&v, bytes + offset, 4); return v; };
    if (word(0) != 0x43425844 || word(24) != size) return false;
    const auto chunks = word(28);
    if (chunks > (size - 32) / 4) return false;
    bool found = false;
    for (UINT i = 0; i < chunks; ++i)
    {
        const auto offset = word(32 + i * 4);
        if (offset > size - 8) return false;
        const auto length = word(offset + 4);
        if (length > size - offset - 8) return false;
        if (word(offset) != 0x58454853 && word(offset) != 0x52444853) continue;
        if (found || length != (vertex ? 240u : 3384u)) return false;
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t j = offset + 8; j < offset + 8 + length; ++j)
            hash = (hash ^ bytes[j]) * 1099511628211ull;
        if (hash != (vertex ? 0xBD2E7C93E9449541ull : 0xABAF2B478C198672ull)) return false;
        found = true;
    }
    return found;
}

ComPtr<Constants> srgb_controls(ID3D11DeviceContext &context, std::uint64_t &version, std::uint64_t epoch) noexcept
{
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11DeviceContext1> extended;
    UINT first = 0, count = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
    if (SUCCEEDED(context.QueryInterface(IID_PPV_ARGS(extended.GetAddressOf()))))
        extended->PSGetConstantBuffers1(0, 1, buffer.GetAddressOf(), &first, &count);
    else context.PSGetConstantBuffers(0, 1, buffer.GetAddressOf());
    // 此程序固定105个register；非零first需要另一组CPU绝对偏移，不能借base0的镜像。
    if (!buffer || first != 0 || count < 105) return {};
    auto shadow = constants(buffer.Get());
    if (!shadow) return {};
    std::lock_guard lock(shadow->mutex);
    if (shadow->mapped || shadow->known != 7 || shadow->epoch != epoch)
        return {};
    const auto &[mode, gamma, overlay] = shadow->values;
    if (!std::isfinite(mode) || !std::isfinite(gamma) || !std::isfinite(overlay) ||
        mode > .5f || gamma != 1.f || overlay > .5f) return {};
    version = shadow->version;
    return shadow;
}

bool geometry_bytes(ID3D11Buffer &buffer, UINT offset, void *out, UINT count, std::uint64_t epoch,
    ComPtr<IUnknown> &witness, std::uint64_t &version) noexcept
{
    auto shadow = private_object<Geometry>(&buffer, k_geometry);
    if (!shadow) return false;
    std::lock_guard lock(shadow->mutex);
    if (shadow->mapped || (!shadow->immutable && shadow->epoch != epoch) ||
        offset > shadow->size || count > shadow->size - offset ||
        std::find(shadow->known.begin() + offset, shadow->known.begin() + offset + count, false) !=
            shadow->known.begin() + offset + count) return false;
    if (witness && (witness.Get() != shadow.Get() || version != shadow->version)) return false;
    witness = shadow;
    version = shadow->version;
    std::memcpy(out, shadow->bytes.data() + offset, count);
    return true;
}

bool fullscreen_geometry(ID3D11DeviceContext &context, bool indexed, UINT start, INT base,
    std::uint64_t epoch, ComPtr<IUnknown> (&witness)[2], std::uint64_t (&versions)[2]) noexcept
{
    ComPtr<ID3D11VertexShader> vertex;
    context.VSGetShader(vertex.GetAddressOf(), nullptr, nullptr);
    UINT known = 0, size = sizeof(known);
    if (!vertex || FAILED(vertex->GetPrivateData(k_vertex, &size, &known)) || known != 1) return false;
    ComPtr<ID3D11InputLayout> layout;
    context.IAGetInputLayout(layout.GetAddressOf());
    Position position;
    size = sizeof(position);
    if (!layout || FAILED(layout->GetPrivateData(k_layout, &size, &position))) return false;
    D3D11_PRIMITIVE_TOPOLOGY topology {};
    context.IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return false;
    ComPtr<ID3D11Buffer> vertices, indices;
    UINT stride = 0, offset = 0, index_offset = 0;
    context.IAGetVertexBuffers(position.slot, 1, vertices.GetAddressOf(), &stride, &offset);
    if (!vertices || stride < position.offset + position.bytes) return false;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    if (indexed) context.IAGetIndexBuffer(indices.GetAddressOf(), &index_format, &index_offset);
    const UINT index_size = index_format == DXGI_FORMAT_R16_UINT ? 2u : index_format == DXGI_FORMAT_R32_UINT ? 4u : 0;
    if (indexed && (!indices || !index_size)) return false;
    std::array<std::array<float, 2>, 3> points;
    for (UINT i = 0; i < 3; ++i)
    {
        std::int64_t index = static_cast<std::int64_t>(start) + i;
        if (indexed)
        {
            const auto index_address = static_cast<std::uint64_t>(index_offset) + index * index_size;
            UINT value = 0;
            if (index_address > UINT_MAX || !geometry_bytes(*indices.Get(), static_cast<UINT>(index_address),
                &value, index_size, epoch, witness[1], versions[1])) return false;
            index = static_cast<std::int64_t>(value) + base;
        }
        const auto address = static_cast<std::int64_t>(offset) + index * stride + position.offset;
        if (index < 0 || address < 0 || address > UINT_MAX ||
            !geometry_bytes(*vertices.Get(), static_cast<UINT>(address), points[i].data(), sizeof(points[i]),
                epoch, witness[0], versions[0]))
            return false;
        if (!std::isfinite(points[i][0]) || !std::isfinite(points[i][1])) return false;
    }
    const auto cross = [](const auto &a, const auto &b, const auto &p)
    { return (double(b[0]) - a[0]) * (double(p[1]) - a[1]) - (double(b[1]) - a[1]) * (double(p[0]) - a[0]); };
    const double area = cross(points[0], points[1], points[2]);
    if (!std::isfinite(area) || area == 0) return false;
    constexpr std::array<std::array<float, 2>, 4> corners {{{-1,-1}, {-1,1}, {1,-1}, {1,1}}};
    for (const auto &corner : corners)
        for (unsigned i = 0; i < 3; ++i)
            if (cross(points[i], points[(i + 1) % 3], corner) * area < 0) return false;
    return true;
}

bool full_output(ID3D11DeviceContext &context, const Target &target) noexcept
{
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports;
    context.RSGetViewports(&count, viewports.data());
    if (count != 1 || viewports[0].TopLeftX != 0 || viewports[0].TopLeftY != 0 ||
        viewports[0].Width != target.texture.Width || viewports[0].Height != target.texture.Height ||
        viewports[0].MinDepth != 0 || viewports[0].MaxDepth != 1) return false;
    ComPtr<ID3D11RasterizerState> raster;
    context.RSGetState(raster.GetAddressOf());
    if (!raster) return false; // 默认背面剔除需要另证绕序，当前不猜。
    D3D11_RASTERIZER_DESC rd {};
    raster->GetDesc(&rd);
    if (rd.FillMode != D3D11_FILL_SOLID || rd.CullMode != D3D11_CULL_NONE) return false;
    ComPtr<ID3D11RasterizerState2> raster2;
    if (SUCCEEDED(raster.As(&raster2)))
    {
        D3D11_RASTERIZER_DESC2 extended {};
        raster2->GetDesc2(&extended);
        if (extended.ConservativeRaster != D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF ||
            extended.ForcedSampleCount != 0) return false;
    }
    if (rd.ScissorEnable)
    {
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> rectangles;
        count = static_cast<UINT>(rectangles.size());
        context.RSGetScissorRects(&count, rectangles.data());
        if (count != 1 || rectangles[0].left > 0 || rectangles[0].top > 0 ||
            rectangles[0].right < static_cast<LONG>(target.texture.Width) ||
            rectangles[0].bottom < static_cast<LONG>(target.texture.Height)) return false;
    }
    ComPtr<ID3D11DepthStencilState> depth;
    context.OMGetDepthStencilState(depth.GetAddressOf(), nullptr);
    if (!depth) return false;
    D3D11_DEPTH_STENCIL_DESC dd {};
    depth->GetDesc(&dd);
    if (dd.StencilEnable || (dd.DepthEnable && dd.DepthFunc != D3D11_COMPARISON_ALWAYS)) return false;
    ComPtr<ID3D11Predicate> predicate;
    context.GetPredication(predicate.GetAddressOf(), nullptr);
    if (predicate) return false;
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    context.GSGetShader(geometry.GetAddressOf(), nullptr, nullptr);
    context.HSGetShader(hull.GetAddressOf(), nullptr, nullptr);
    context.DSGetShader(domain.GetAddressOf(), nullptr, nullptr);
    return !geometry && !hull && !domain;
}

void opaque_commands() noexcept { ++g_opaque_epoch; }
void repair_dispatch(ID3D11DeviceContext *context) { if (g_repair) g_repair(context); }

struct Opaque
{
    template <class... Args> static void before(ID3D11DeviceContext *, Args...) noexcept { opaque_commands(); }
    static void after(ID3D11DeviceContext *context) noexcept
    {
        refresh_targets(*context);
        refresh_targets(*context, true);
    }
};
struct GraphicsWrite
{
    template <class... Args> static void before(ID3D11DeviceContext *context, Args...) noexcept
    { (void)begin_draw(*context, 0); }
    static void after(ID3D11DeviceContext *) noexcept {}
};
struct ComputeWrite
{
    template <class... Args> static void before(ID3D11DeviceContext *context, Args...) noexcept
    { unordered_write(*context); }
    static void after(ID3D11DeviceContext *) noexcept {}
};
struct ResourceWrite
{
    template <class... Args> static void before(ID3D11DeviceContext *, ID3D11Resource *destination, Args...) noexcept
    { invalidate(destination); }
    static void after(ID3D11DeviceContext *) noexcept {}
};
struct ViewWrite
{
    template <class View, class... Args> static void before(ID3D11DeviceContext *, View *view, Args...) noexcept
    {
        ComPtr<ID3D11Resource> resource;
        if (view) view->GetResource(resource.GetAddressOf());
        invalidate(resource.Get());
    }
    static void after(ID3D11DeviceContext *) noexcept {}
};
struct Update1
{
    static void before(ID3D11DeviceContext *context, ID3D11Resource *destination, UINT subresource,
        const D3D11_BOX *box, const void *data, UINT, UINT, UINT flags) noexcept
    {
        if (flags & D3D11_COPY_DISCARD) invalidate(destination);
        if (flags & ~(D3D11_COPY_DISCARD | D3D11_COPY_NO_OVERWRITE)) invalidate(destination);
        else update_buffer(*context, destination, subresource, box, data, true);
    }
    static void after(ID3D11DeviceContext *) noexcept {}
};

template <std::size_t Index, class Policy, class Result, class... Args>
struct Method
{
    static Result STDMETHODCALLTYPE observe(ID3D11DeviceContext *context, Args... args)
    {
        Policy::before(context, args...);
        if constexpr (std::is_void_v<Result>)
        {
            Route {}(context, args...);
            Policy::after(context);
        }
        else
        {
            const auto result = Route {}(context, args...);
            Policy::after(context);
            return result;
        }
    }
    using Function = Result(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, Args...);
    using Route = ComMethodRoutes<Function, &observe, Index, true, &repair_dispatch>;
    static constexpr com_vtable::MethodPatch slot() { return Route::slot(); }
};

using DrawInstanced = Method<21, GraphicsWrite, void, UINT, UINT, UINT, UINT>;
using DrawIndexedInstanced = Method<20, GraphicsWrite, void, UINT, UINT, UINT, INT, UINT>;
using DrawAuto = Method<38, GraphicsWrite, void>;
using DrawIndirect = Method<40, GraphicsWrite, void, ID3D11Buffer *, UINT>;
using DrawIndexedIndirect = Method<39, GraphicsWrite, void, ID3D11Buffer *, UINT>;
using DispatchIndirect = Method<42, ComputeWrite, void, ID3D11Buffer *, UINT>;
using ClearUavUint = Method<51, ViewWrite, void, ID3D11UnorderedAccessView *, const UINT *>;
using ClearUavFloat = Method<52, ViewWrite, void, ID3D11UnorderedAccessView *, const FLOAT *>;
using GenerateMips = Method<54, ViewWrite, void, ID3D11ShaderResourceView *>;
using Resolve = Method<57, ResourceWrite, void, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT>;
using Execute = Method<58, Opaque, void, ID3D11CommandList *, BOOL>;
using ClearState = Method<110, Opaque, void>;
using Finish = Method<114, Opaque, HRESULT, BOOL, ID3D11CommandList **>;
using Copy1 = Method<115, ResourceWrite, void, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT,
    const D3D11_BOX *, UINT>;
using Update = Method<116, Update1, void, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT, UINT>;
using Discard = Method<117, ResourceWrite, void, ID3D11Resource *>;
using DiscardView = Method<118, ViewWrite, void, ID3D11View *>;
using SwapState = Method<131, Opaque, void, ID3DDeviceContextState *, ID3DDeviceContextState **>;
using ClearView = Method<132, ViewWrite, void, ID3D11View *, const FLOAT *, const D3D11_RECT *, UINT>;
using DiscardView1 = Method<133, ViewWrite, void, ID3D11View *, const D3D11_RECT *, UINT>;
const std::array k_extended {Copy1::slot(), Update::slot(), Discard::slot(), DiscardView::slot(),
    SwapState::slot(), ClearView::slot(), DiscardView1::slot()};

HRESULT STDMETHODCALLTYPE create_layout(ID3D11Device *device, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
    const void *bytecode, SIZE_T bytes, ID3D11InputLayout **out);
using LayoutRoute = ComMethodRoutes<decltype(&create_layout), &create_layout, 11>;

HRESULT STDMETHODCALLTYPE create_layout(ID3D11Device *device, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
    const void *bytecode, SIZE_T bytes, ID3D11InputLayout **out)
{
    const auto result = LayoutRoute {}(device, elements, count, bytecode, bytes, out);
    if (FAILED(result) || !out || !*out || !program_matches(bytecode, bytes, true)) return result;
    std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> next {};
    Position position;
    bool found = false;
    for (UINT i = 0; i < count; ++i)
    {
        const auto &element = elements[i];
        const UINT width = element.Format == DXGI_FORMAT_R32G32_FLOAT ? 8u :
            element.Format == DXGI_FORMAT_R32G32B32_FLOAT ? 12u :
            element.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16u : 0u;
        if (element.InputSlot >= next.size() || !width) return result;
        const UINT offset = element.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ?
            next[element.InputSlot] : element.AlignedByteOffset;
        if (offset > 4096 - width) return result;
        next[element.InputSlot] = offset + width;
        if (element.SemanticName && _stricmp(element.SemanticName, "POSITION") == 0 && element.SemanticIndex == 0)
        {
            if (found || element.InputSlotClass != D3D11_INPUT_PER_VERTEX_DATA || element.InstanceDataStepRate)
                return result;
            position = {element.InputSlot, offset, width};
            found = true;
        }
    }
    if (found) (*out)->SetPrivateData(k_layout, sizeof(position), &position);
    return result;
}
}

void set_enabled(bool value) noexcept { g_enabled.store(value, std::memory_order_release); }
bool enabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

void remember_shader(ID3D11PixelShader &shader, const void *bytes, std::size_t size) noexcept
{
    if (!enabled()) return;
    if (!program_matches(bytes, size))
    {
        shader.SetPrivateData(k_program, 0, nullptr);
        return;
    }
    constexpr UINT recognized = 1;
    shader.SetPrivateData(k_program, sizeof(recognized), &recognized);
}

void remember_buffer(ID3D11Buffer &buffer, const D3D11_BUFFER_DESC &desc,
    const D3D11_SUBRESOURCE_DATA *initial) noexcept
{
    if (!enabled()) return;
    // 2026-11-16：Dispatch/SO能改写小型VB/IB而不经过CPU更新入口；实际GPU清零反例
    // 仍会留下旧全屏点。创建契约允许这两类写入时不镜像几何，避免把旧覆盖当成当前事实。
    if (desc.ByteWidth <= 4096 && (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) &&
        !(desc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT)))
    {
        auto geometry = add_private<Geometry>(buffer, k_geometry);
        if (geometry)
        {
            std::lock_guard lock(geometry->mutex);
            geometry->size = desc.ByteWidth;
            // IMMUTABLE由D3D创建契约禁止后续CPU/GPU写；不因无关命令列表丢弃其初值。
            geometry->immutable = desc.Usage == D3D11_USAGE_IMMUTABLE;
            geometry->known.fill(false);
            if (initial && initial->pSysMem) geometry->write(initial->pSysMem, 0, desc.ByteWidth);
        }
    }
    if (desc.ByteWidth != k_constant_bytes || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    auto value = add_private<Constants>(buffer, k_constants);
    if (!value) return;
    g_buffer_marks.fetch_or(buffer_mark(&buffer), std::memory_order_relaxed);
    std::lock_guard lock(value->mutex);
    value->known = 0;
    value->mapped = nullptr;
    if (initial && initial->pSysMem) value->write(initial->pSysMem, 0, desc.ByteWidth);
}

void remember_vertex(ID3D11VertexShader &shader, const void *bytes, std::size_t size) noexcept
{
    if (!enabled() || !program_matches(bytes, size, true)) return;
    constexpr UINT recognized = 1;
    shader.SetPrivateData(k_vertex, sizeof(recognized), &recognized);
}

void attach_context(ID3D11DeviceContext &context) noexcept
{
    if (!enabled()) return;
    // private-data的IUnknown在宿主COM释放时才销毁；其vtable所在DLL必须活到该时刻。
    // 桥本身的进程内钩子同样属于本局运行时，不允许卸载代码后保留这些对象。
    static const bool lifetime = []
    {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&attach_context), &module) != FALSE;
    }();
    if (lifetime) (void)add_private<Execution>(context, k_execution);
}

void refresh_targets(ID3D11DeviceContext &context, bool compute) noexcept
{
    if (!enabled()) return;
    auto execution = private_object<Execution>(&context, k_execution);
    if (!execution) return;
    ComPtr<ID3D11Device> device;
    context.GetDevice(device.GetAddressOf());
    const UINT slots = device && device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> uavs {};
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    if (compute) context.CSGetUnorderedAccessViews(0, slots, uavs.data());
    else
    {
        context.OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), nullptr);
        context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, slots, uavs.data());
    }
    std::lock_guard lock(execution->mutex);
    if (!compute)
        for (unsigned i = 0; i < rtvs.size(); ++i)
        {
            auto &target = execution->targets[i];
            target = {};
            if (rtvs[i])
            {
                rtvs[i]->GetDesc(&target.view);
                target.contents = view_contents(rtvs[i], &target);
                rtvs[i]->Release();
            }
        }
    auto &bound = compute ? execution->compute_uavs : execution->graphics_uavs;
    for (unsigned i = 0; i < bound.size(); ++i)
    {
        bound[i] = view_contents(uavs[i]);
        if (uavs[i]) uavs[i]->Release();
    }
}

void invalidate(ID3D11Resource *resource) noexcept
{
    if (!enabled() || !resource) return;
    if (auto contents = private_object<Contents>(resource, k_contents)) contents->overwritten();
    if (auto shadow = constants(resource))
    {
        std::lock_guard lock(shadow->mutex);
        shadow->known = 0;
        shadow->mapped = nullptr;
        ++shadow->version;
    }
    if (auto geometry = private_object<Geometry>(resource, k_geometry))
    {
        std::lock_guard lock(geometry->mutex);
        geometry->known.fill(false);
        geometry->mapped = nullptr;
        ++geometry->version;
    }
}

void update_buffer(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource,
    const D3D11_BOX *box, const void *data, bool context1) noexcept
{
    if (!enabled() || !resource) return;
    if (auto contents = private_object<Contents>(resource, k_contents)) contents->overwritten();
    if (auto geometry = private_object<Geometry>(resource, k_geometry))
    {
        std::lock_guard lock(geometry->mutex);
        geometry->mapped = nullptr;
        const UINT begin = box ? box->left : 0, end = box ? box->right : geometry->size;
        if (subresource || !data || begin >= end || end > geometry->size ||
            context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            (box && (box->top || box->front || box->bottom != 1 || box->back != 1)))
            geometry->known.fill(false);
        else geometry->write(data, begin, end);
    }
    auto shadow = constants(resource);
    if (!shadow) return;
    std::lock_guard lock(shadow->mutex);
    shadow->mapped = nullptr;
    if (subresource || !data || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || (box && !context1))
    {
        shadow->known = 0;
        ++shadow->version;
        return;
    }
    const UINT begin = box ? box->left : 0, end = box ? box->right : k_constant_bytes;
    if (box)
    {
        ComPtr<ID3D11Device> device;
        context.GetDevice(device.GetAddressOf());
        D3D11_FEATURE_DATA_D3D11_OPTIONS options {};
        if ((begin % 16) || (end % 16) || !device || FAILED(device->CheckFeatureSupport(
            D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) || !options.ConstantBufferPartialUpdate)
        { shadow->known = 0; ++shadow->version; return; }
    }
    if (begin >= end || end > k_constant_bytes || (box && (box->top || box->front ||
        box->bottom != 1 || box->back != 1)))
    {
        shadow->known = 0;
        ++shadow->version;
        return;
    }
    shadow->write(data, begin, end);
}

void mapped(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource,
    D3D11_MAP type, HRESULT result, const D3D11_MAPPED_SUBRESOURCE *mapping) noexcept
{
    if (!enabled() || FAILED(result) || !resource || type == D3D11_MAP_READ) return;
    invalidate(resource);
    auto shadow = constants(resource);
    auto execution = private_object<Execution>(&context, k_execution);
    if (auto geometry = private_object<Geometry>(resource, k_geometry))
    {
        if (execution && !subresource && mapping && mapping->pData &&
            context.GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            std::lock_guard lock(geometry->mutex);
            geometry->mapped = mapping->pData;
            geometry->map_owner = execution->owner;
        }
    }
    if (!shadow || !execution || subresource || !mapping || !mapping->pData ||
        context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    std::lock_guard lock(shadow->mutex);
    shadow->mapped = mapping->pData;
    shadow->map_owner = execution->owner;
}

void unmapping(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource) noexcept
{
    if (!enabled()) return;
    auto execution = private_object<Execution>(&context, k_execution);
    if (auto geometry = private_object<Geometry>(resource, k_geometry))
    {
        std::lock_guard lock(geometry->mutex);
        if (!subresource && execution && geometry->mapped && geometry->map_owner == execution->owner)
            geometry->write(geometry->mapped, 0, geometry->size);
        else if (geometry->mapped) geometry->known.fill(false);
        geometry->mapped = nullptr;
    }
    auto shadow = constants(resource);
    if (!shadow) return;
    std::lock_guard lock(shadow->mutex);
    if (!subresource && execution && shadow->mapped && shadow->map_owner == execution->owner)
        shadow->write(shadow->mapped, 0, k_constant_bytes);
    else if (shadow->mapped) shadow->known = 0;
    shadow->mapped = nullptr;
}

void unordered_write(ID3D11DeviceContext &context) noexcept
{
    if (!enabled()) return;
    auto execution = private_object<Execution>(&context, k_execution);
    if (!execution) return;
    std::lock_guard lock(execution->mutex);
    for (auto &value : execution->compute_uavs) if (value) value->overwritten();
}

DrawWrite begin_draw(ID3D11DeviceContext &context, UINT elements) noexcept
{ return begin_draw(context, elements, false, 0, 0); }

DrawWrite begin_draw(ID3D11DeviceContext &context, UINT elements, bool indexed, UINT start, INT base) noexcept
{
    DrawWrite result;
    if (!enabled()) return result;
    const auto epoch = g_opaque_epoch.load(std::memory_order_acquire);
    auto execution = private_object<Execution>(&context, k_execution);
    if (!execution) return result;
    std::lock_guard lock(execution->mutex);
    for (auto &target : execution->targets) if (target.contents) target.contents->overwritten();
    for (auto &value : execution->graphics_uavs) if (value) value->overwritten();
    auto &target = execution->targets[0];
    if (elements != 3 || !target.contents || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (target.view.Format != DXGI_FORMAT_R8G8B8A8_UNORM && target.view.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        target.view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || target.view.Texture2D.MipSlice != 0)
        return result;
    ComPtr<ID3D11PixelShader> shader;
    context.PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    UINT recognized = 0, size = sizeof(recognized);
    if (!shader || FAILED(shader->GetPrivateData(k_program, &size, &recognized)) || recognized != 1) return result;
    auto shadow = srgb_controls(context, result.constants_version_, epoch);
    if (!shadow || !full_output(context, target) || !fullscreen_geometry(context, indexed, start, base, epoch,
        result.geometry_, result.geometry_versions_))
        return result;
    result.constants_ = shadow;
    ComPtr<ID3D11BlendState> blend;
    UINT sample_mask = 0;
    context.OMGetBlendState(blend.GetAddressOf(), nullptr, &sample_mask);
    if (sample_mask != ~0u) return result;
    if (blend)
    {
        D3D11_BLEND_DESC desc {};
        blend->GetDesc(&desc);
        if (desc.AlphaToCoverageEnable || desc.RenderTarget[0].BlendEnable ||
            (desc.RenderTarget[0].RenderTargetWriteMask & 7) != 7) return result;
        ComPtr<ID3D11BlendState1> extended;
        if (SUCCEEDED(blend.As(&extended)))
        {
            D3D11_BLEND_DESC1 desc1 {};
            extended->GetDesc1(&desc1);
            if (desc1.RenderTarget[0].LogicOpEnable) return result;
        }
    }
    result.record_ = target.contents;
    result.owner_ = execution->owner;
    result.epoch_ = epoch;
    {
        std::lock_guard contents_lock(target.contents->mutex);
        result.serial_ = target.contents->serial;
    }
    return result;
}

void DrawWrite::returned() noexcept
{
    if (!record_) return;
    auto *contents = static_cast<Contents *>(record_.Get());
    auto *shadow = static_cast<Constants *>(constants_.Get());
    for (unsigned i = 0; i < 2; ++i)
    {
        auto *geometry = static_cast<Geometry *>(geometry_[i].Get());
        if (!geometry) continue;
        std::lock_guard geometry_lock(geometry->mutex);
        if (geometry->mapped || geometry->version != geometry_versions_[i]) { record_.Reset(); return; }
    }
    std::lock_guard constants_lock(shadow->mutex);
    std::lock_guard lock(contents->mutex);
    if (contents->serial == serial_ && epoch_ == g_opaque_epoch.load(std::memory_order_relaxed) &&
        shadow->version == constants_version_ && !shadow->mapped && shadow->known == 7)
    {
        contents->srgb = true;
        contents->owner = owner_;
        contents->epoch = epoch_;
        contents->constants_version = constants_version_;
    }
    record_.Reset();
}

interop::ColorTransfer take_transfer(ID3D11DeviceContext &context, ID3D11Texture2D &source) noexcept
{
    if (!enabled()) return interop::ColorTransfer::unknown;
    auto contents = private_object<Contents>(&source, k_contents);
    auto execution = private_object<Execution>(&context, k_execution);
    if (!contents || !execution) return interop::ColorTransfer::unknown;
    std::lock_guard lock(contents->mutex);
    const bool current = contents->srgb && contents->owner == execution->owner &&
        contents->epoch == g_opaque_epoch.load(std::memory_order_relaxed);
    // 同一writer只能授权一次本桥消费，不让未重写的旧generation自动成为下一帧证书。
    if (contents->owner == execution->owner) contents->srgb = false;
    return current ? interop::ColorTransfer::srgb : interop::ColorTransfer::unknown;
}

std::span<const com_vtable::MethodPatch> extra_methods(void (*repair)(ID3D11DeviceContext *)) noexcept
{
    g_repair = repair;
    static const std::array methods {DrawInstanced::slot(), DrawIndexedInstanced::slot(), DrawAuto::slot(),
        DrawIndirect::slot(), DrawIndexedIndirect::slot(), DispatchIndirect::slot(), ClearUavUint::slot(),
        ClearUavFloat::slot(), GenerateMips::slot(), Resolve::slot(), Execute::slot(), ClearState::slot(), Finish::slot()};
    return methods;
}

bool bind_extended(ID3D11DeviceContext &context) noexcept
{
    const auto count = interface_layout::context(&context);
    // 无Context1接口不创建编码证书，防止未观察到的扩展入口沿用旧声明。
    if (count < 134) return false;
    return com_vtable::patch(&context, count, k_extended);
}

bool bind_device(ID3D11Device &device) noexcept
{
    return !enabled() || com_vtable::patch(&device, interface_layout::device(&device), {LayoutRoute::slot()});
}
}
