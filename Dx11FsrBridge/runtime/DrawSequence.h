#pragma once

#include <utility>

namespace neo::graphics
{
// 同一调用可以先由后端试用原 Draw 补齐历史，再回退；回退不能再次执行同一调用。
// 栈上守卫不分配、不同步，也不影响独立的内部 GPU 绘制。
template <class Draw>
class OriginalDraw
{
public:
    explicit OriginalDraw(Draw &next) : next_(next) {}

    bool executed() const { return executed_; }

    void operator()()
    {
        if (!std::exchange(executed_, true))
            next_();
    }

private:
    Draw &next_;
    bool executed_ = false;
};

template <class Draw, class Upscale, class ToneMap, class Spatial, class Before, class After>
void execute_draw(Draw &&next, Upscale &&upscale, ToneMap &&tone_map, Spatial &&spatial, Before &&before, After &&after)
{
    OriginalDraw original(next);
    if (upscale(original) || original.executed() || tone_map(original) || original.executed() ||
        spatial(original) || original.executed())
        return;
    before();
    original();
    after();
}
}
