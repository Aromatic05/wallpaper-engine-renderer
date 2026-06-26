#pragma once

#include "output/RenderPlan.hpp"
#include "output/RenderPlanSource.hpp"

#include <functional>
#include <memory>

namespace wallpaper
{
// RenderPlan for the CEF-based web backend. Required binding kind
// is WebRenderTarget, the value commit 6 added to the public enum.
//
// The actual bindOutput work is captured as a std::function so this
// header stays independent of the BrowserHost CEF implementation.
// WebBackend (commit 8) wires the bind lambda at construction time;
// before that the lambda stays null and bindOutput returns a clear
// error so the public surface still compiles in -DBUILD_WEWEB=OFF
// configurations where BrowserHost is not built.
class WebRenderPlan final : public RenderPlan {
public:
    using BindFn = std::function<Result<void>(const OutputTarget&)>;

    explicit WebRenderPlan(BindFn bind = {});

    OutputTargetBindingKind requiredBindingKind() const override;
    std::uint64_t           revision() const override;
    Result<void>            bindOutput(const OutputTarget& target) override;

private:
    BindFn m_bind;
    std::uint64_t m_revision { 0 };
};

// RenderPlanSource the WebBackend exposes to the session's
// OutputController. Returns the single WebRenderPlan instance.
class WebOutputSource final : public RenderPlanSource {
public:
    explicit WebOutputSource(std::shared_ptr<WebRenderPlan> plan);

protected:
    Result<RenderPlanPtr> currentRenderPlan() const override;

private:
    std::shared_ptr<WebRenderPlan> m_plan;
};
} // namespace wallpaper
