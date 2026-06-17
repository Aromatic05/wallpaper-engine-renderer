#pragma once

#include "runtime/backend/BackendFactory.hpp"

#include <memory>

namespace wallpaper
{
class BuiltinBackendFactory final : public BackendFactory {
public:
    Result<std::unique_ptr<ContentBackend>> create(BackendType          type,
                                                   const BackendContext& context) override;
};

std::shared_ptr<BackendFactory> CreateBuiltinBackendFactory();
} // namespace wallpaper
