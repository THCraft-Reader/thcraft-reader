#pragma once
namespace BoardConfig {
struct ViewableInsets { int top = 0, right = 0, bottom = 0, left = 0; };
struct Board { ViewableInsets viewableInsets; };
inline constexpr Board ACTIVE{};
}
