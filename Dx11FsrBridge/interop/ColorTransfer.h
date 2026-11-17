#pragma once
#include <cstdint>

namespace neo::interop
{
// 视图描述硬件读法；本枚举描述产生存储值的颜色编码，二者不互相推导。
enum class ColorTransfer : std::uint32_t { unknown = 0, srgb = 1, linear = 2, pq = 3 };
}
