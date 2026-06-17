#pragma once

#include "runtime/input/InputEvent.hpp"

#include <deque>

namespace wallpaper
{
using InputQueue = std::deque<InputEvent>;
} // namespace wallpaper
