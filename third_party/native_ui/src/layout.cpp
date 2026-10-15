#include <native_ui/layout.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace native_ui {
FlowResult Wrap(const std::vector<Item>& items, float width, float gap, float padding) {
    auto valid = [](float x) { return std::isfinite(x) && x >= 0 && x <= 1048576; };
    if (!valid(width) || !valid(gap) || !valid(padding)) throw std::invalid_argument("invalid flow bounds");
    FlowResult result; result.rects.resize(items.size());
    float x = padding, y = padding, row = 0; bool any = false;
    float available = std::max(0.0f, width - 2 * padding);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (!valid(item.minimum.width) || !valid(item.minimum.height) || !valid(item.preferred.width) || !valid(item.preferred.height)) throw std::invalid_argument("invalid flow size");
        if (!item.visible) continue;
        float w = std::max(item.minimum.width, std::min(available, item.preferred.width));
        float h = std::max(item.minimum.height, item.preferred.height);
        if (x > padding && x + w > width - padding) { y += row + gap; x = padding; row = 0; }
        result.rects[i] = {x, y, w, h}; x += w + gap; row = std::max(row, h); any = true;
    }
    result.height = any ? y + row + padding : 0; return result;
}
}
