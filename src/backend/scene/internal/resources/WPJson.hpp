#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>
#include <type_traits>
#include <vector>

#include "utils/Logging.h"
#include "WPUserProperties.hpp"

#define GET_JSON_VALUE(json, value) \
    wallpaper::GetJsonValue(        \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", true)
#define GET_JSON_NAME_VALUE(json, name, value) \
    wallpaper::GetJsonValue(                   \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), true)

#define GET_JSON_VALUE_NOWARN(json, value) \
    wallpaper::GetJsonValue(               \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", false)
#define GET_JSON_NAME_VALUE_NOWARN(json, name, value) \
    wallpaper::GetJsonValue(                          \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), false)

#define PARSE_JSON(source, result) \
    wallpaper::ParseJson(__SHORT_FILE__, __FUNCTION__, __LINE__, (source), (result))

namespace wallpaper
{

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! std::is_const_v<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename wallpaper::JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name, bool warn);

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result);

bool ReadJsonIntArray(const nlohmann::json& json,
                      std::string_view name,
                      std::vector<int32_t>& value);

class ScopedJsonUserProperties {
public:
    explicit ScopedJsonUserProperties(const UserPropertyMap* properties,
                                      const nlohmann::json* root = nullptr);
    ~ScopedJsonUserProperties();

    ScopedJsonUserProperties(const ScopedJsonUserProperties&)            = delete;
    ScopedJsonUserProperties& operator=(const ScopedJsonUserProperties&) = delete;

private:
    const UserPropertyMap* m_previous { nullptr };
    const nlohmann::json*  m_previous_root { nullptr };
};
} // namespace wallpaper
