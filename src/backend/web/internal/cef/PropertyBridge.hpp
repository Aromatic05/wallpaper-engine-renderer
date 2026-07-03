#pragma once

#include <string>

namespace wallpaper
{
std::string BuildPropertyListenerBootstrapSnippet();
std::string BuildPropertyListenerApplySnippet(const std::string& user_props_json);
std::string BuildApplyUserPropertySnippet(const std::string& key, const std::string& value_json);
} // namespace wallpaper
