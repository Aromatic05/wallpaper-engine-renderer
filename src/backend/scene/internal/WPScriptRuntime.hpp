#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wallpaper
{

enum class WPScriptValueShape
{
    Number,
    Boolean,
    String,
    NumberArray,
    VectorString,
};

struct WPScriptValue {
    WPScriptValueShape shape { WPScriptValueShape::Number };
    std::vector<double> numeric_values;
    std::string string_value;
    bool boolean_value { false };

    static WPScriptValue Number(double value);
    static WPScriptValue Boolean(bool value);
    static WPScriptValue String(std::string value);
    static WPScriptValue NumberArray(std::vector<double> values);
    static WPScriptValue VectorString(std::vector<double> values);

    bool isNumericVector() const noexcept;
};

using WPScriptPropertyMap = std::unordered_map<std::string, WPScriptValue>;

struct WPScriptEvaluationContext {
    WPScriptPropertyMap script_properties;
    std::array<double, 2> canvas_size { 0.0, 0.0 };
    std::string property_name;
};

class WPScriptRuntime {
public:
    WPScriptRuntime();
    ~WPScriptRuntime();

    WPScriptRuntime(const WPScriptRuntime&) = delete;
    WPScriptRuntime& operator=(const WPScriptRuntime&) = delete;

    bool isReady() const noexcept;

    std::optional<WPScriptValue> evaluate(std::string_view script_source,
                                          const WPScriptValue& current_value,
                                          const WPScriptEvaluationContext& context);

private:
    struct Opaque;
    Opaque* m_impl { nullptr };
};

} // namespace wallpaper
