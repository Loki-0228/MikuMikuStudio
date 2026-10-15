#include <native_ui/layout.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <map>

namespace native_ui {
namespace {
float Axis(Size s, int axis) { return axis ? s.height : s.width; }
void Axis(Size& s, int axis, float value) { (axis ? s.height : s.width) = value; }
bool Number(float x) { return std::isfinite(x) && x >= 0 && x <= 1048576; }
struct Measured {
    const Node* node;
    Size minimum{}, preferred{}, maximum{};
    std::vector<Measured> children;
};
Measured Measure(const Node& node, unsigned depth) {
    if (depth > 64) throw std::invalid_argument("layout depth exceeds 64");
    const auto& h = node.hints;
    if (unsigned(h.flow) > 4 || h.columns > 4096 || h.align > 3) throw std::invalid_argument("invalid layout mode");
    for (Size s : {node.measuredMinimum, node.measuredPreferred, h.minimum, h.preferred, h.maximum, h.grow, h.shrink})
        if (!Number(s.width) || !Number(s.height)) throw std::invalid_argument("invalid layout size");
    for (float f : h.gap) if (!Number(f)) throw std::invalid_argument("invalid gap");
    for (float f : h.padding) if (!Number(f)) throw std::invalid_argument("invalid padding");
    Measured m{&node}; m.minimum = node.measuredMinimum; m.preferred = node.measuredPreferred;
    for (const auto& child : node.children) m.children.push_back(Measure(child, depth + 1));
    const bool column = h.flow == Flow::Column;
    float minMain = 0, prefMain = 0, minCross = 0, prefCross = 0; unsigned count = 0;
    for (const auto& child : m.children) if (child.node->visible || child.node->hints.preserveHidden) {
        if (count++) { minMain += h.gap[column]; prefMain += h.gap[column]; }
        minMain += Axis(child.minimum, column); prefMain += Axis(child.preferred, column);
        minCross = std::max(minCross, Axis(child.minimum, !column)); prefCross = std::max(prefCross, Axis(child.preferred, !column));
    }
    if (count) {
        const bool wrap = h.flow == Flow::Auto || h.flow == Flow::Wrap || h.flow == Flow::Grid;
        if (wrap && !h.keepTogether) { minMain = 0; for (const auto& child : m.children) if (child.node->visible || child.node->hints.preserveHidden) minMain = std::max(minMain, child.minimum.width); }
        Axis(m.minimum, column, std::max(Axis(m.minimum, column), minMain + h.padding[column] + h.padding[column + 2]));
        Axis(m.minimum, !column, std::max(Axis(m.minimum, !column), minCross + h.padding[!column] + h.padding[!column + 2]));
        Axis(m.preferred, column, std::max(Axis(m.preferred, column), prefMain + h.padding[column] + h.padding[column + 2]));
        Axis(m.preferred, !column, std::max(Axis(m.preferred, !column), prefCross + h.padding[!column] + h.padding[!column + 2]));
    }
    for (int axis = 0; axis != 2; ++axis) {
        float minimum = std::max(Axis(m.minimum, axis), Axis(h.minimum, axis));
        if (Axis(h.maximum, axis) && Axis(h.maximum, axis) < Axis(h.minimum, axis)) throw std::invalid_argument("maximum below minimum");
        float maximum = Axis(h.maximum, axis) ? std::max(minimum, Axis(h.maximum, axis)) : 1048576;
        float preferred = std::clamp(Axis(h.preferred, axis) ? Axis(h.preferred, axis) : Axis(m.preferred, axis), minimum, maximum);
        Axis(m.minimum, axis, minimum); Axis(m.maximum, axis, maximum); Axis(m.preferred, axis, preferred);
    }
    return m;
}
std::vector<float> Flex(const std::vector<const Measured*>& nodes, int axis, float available, float gap) {
    std::vector<float> sizes; float used = gap * float(nodes.empty() ? 0 : nodes.size() - 1);
    for (auto* n : nodes) { sizes.push_back(Axis(n->preferred, axis)); used += sizes.back(); }
    float free = available - used;
    for (size_t pass = 0; pass <= nodes.size() && std::abs(free) > 0.001f; ++pass) {
        float weights = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto* n = nodes[i]; if (free > 0 ? sizes[i] < Axis(n->maximum, axis) : sizes[i] > Axis(n->minimum, axis))
                weights += Axis(free > 0 ? n->node->hints.grow : n->node->hints.shrink, axis);
        }
        if (weights == 0) break;
        float consumed = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto* n = nodes[i]; bool can = free > 0 ? sizes[i] < Axis(n->maximum, axis) : sizes[i] > Axis(n->minimum, axis);
            float weight = can ? Axis(free > 0 ? n->node->hints.grow : n->node->hints.shrink, axis) : 0;
            float next = std::clamp(sizes[i] + free * weight / weights, Axis(n->minimum, axis), Axis(n->maximum, axis));
            consumed += next - sizes[i]; sizes[i] = next;
        }
        if (std::abs(consumed) < 0.001f) break; free -= consumed;
    }
    return sizes;
}
struct Pass { std::map<std::pair<const Node*, float>, float> heights; size_t visits = 0; };
Size Place(const Measured& m, Rect rect, bool parentVisible, TreeResult& out, Pass& pass);
float CrossSize(const Measured& child, int axis, float available, unsigned align) {
    float size = Axis(child.preferred, axis);
    if (align == 3) size = available;
    else if (Axis(child.node->hints.shrink, axis) > 0) size = std::min(size, available);
    return std::clamp(size, Axis(child.minimum, axis), Axis(child.maximum, axis));
}
float NaturalHeight(const Measured& child, float width, Pass& pass) {
    if (child.children.empty()) return child.preferred.height;
    auto key = std::make_pair(child.node, width); auto cached = pass.heights.find(key); if (cached != pass.heights.end()) return cached->second;
    TreeResult scratch;
    float height = std::max(child.minimum.height, Place(child, {0, 0, width, 0}, true, scratch, pass).height);
    pass.heights.emplace(key, height); return height;
}
Size Place(const Measured& m, Rect rect, bool parentVisible, TreeResult& out, Pass& pass) {
    if (++pass.visits > 262144) throw std::invalid_argument("layout work limit exceeded");
    const auto& node = *m.node; const auto& h = node.hints; bool visible = parentVisible && node.visible;
    const size_t index = out.items.size(); out.items.push_back({node.id, rect, visible, false});
    if (!node.visible && !h.preserveHidden) {
        out.items[index].rect = {};
        for (const auto& child : m.children) Place(child, {}, false, out, pass);
        return {};
    }
    if (m.children.empty()) return {rect.width, rect.height};
    const bool column = h.flow == Flow::Column;
    float width = std::max(0.0f, rect.width - h.padding[0] - h.padding[2]);
    float height = std::max(0.0f, rect.height - h.padding[1] - h.padding[3]);
    std::vector<const Measured*> active;
    for (const auto& child : m.children) if (child.node->visible || child.node->hints.preserveHidden) active.push_back(&child); else Place(child, {}, false, out, pass);
    std::vector<std::vector<const Measured*>> lines(1);
    bool wrap = (h.flow == Flow::Auto || h.flow == Flow::Wrap || h.flow == Flow::Grid) && !h.keepTogether;
    unsigned columns = 4096;
    if (h.flow == Flow::Grid) {
        float cell = 1; for (auto* child : active) cell = std::max(cell, child->minimum.width);
        columns = std::max(1u, unsigned((width + h.gap[0]) / (cell + h.gap[0])));
        if (h.columns) columns = std::min(columns, h.columns);
    }
    float minimumLine = 0;
    for (auto* child : active) {
        float minimum = h.flow == Flow::Grid || child->node->hints.shrink.width > 0 ? child->minimum.width : child->preferred.width;
        if (wrap && !lines.back().empty() && (lines.back().size() >= columns || minimumLine + h.gap[0] + minimum > width)) { lines.emplace_back(); minimumLine = 0; }
        if (!lines.back().empty()) minimumLine += h.gap[0]; lines.back().push_back(child); minimumLine += minimum;
    }
    float crossOffset = 0, extentX = 0, extentY = 0;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        // A column must measure nested wrap/grid children at their allocated
        // width before assigning vertical positions to subsequent siblings.
        std::vector<Measured> adjusted; adjusted.reserve(line.size());
        for (auto* child : line) {
            adjusted.push_back(*child);
            if (column && !child->children.empty()) {
                float natural = NaturalHeight(*child, CrossSize(*child, 0, width, h.align), pass);
                adjusted.back().preferred.height = std::max(child->preferred.height, natural);
                adjusted.back().minimum.height = std::max(child->minimum.height, natural);
                adjusted.back().maximum.height = std::max(child->maximum.height, natural);
            }
        }
        std::vector<const Measured*> measured; for (const auto& child : adjusted) measured.push_back(&child);
        float naturalMain = h.gap[column] * float(line.size() - 1); for (auto* child : measured) naturalMain += Axis(child->preferred, column);
        float mainAvailable = column ? (rect.height ? height : naturalMain) : width;
        auto sizes = Flex(measured, column, mainAvailable, h.gap[column]);
        if (h.flow == Flow::Grid) for (size_t i = 0; i < line.size(); ++i) sizes[i] = std::clamp((width - (columns - 1) * h.gap[0]) / columns, line[i]->minimum.width, line[i]->maximum.width);
        float cross = 0;
        for (size_t i = 0; i < line.size(); ++i) cross = std::max(cross, column ? line[i]->preferred.width : NaturalHeight(*line[i], sizes[i], pass));
        if (column) cross = width; else if (!wrap && rect.height) cross = height;
        float mainOffset = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            auto* child = line[i]; auto align = h.align;
            float size = CrossSize(*child, !column, cross, align);
            if (!column && !child->children.empty()) size = std::max(size, NaturalHeight(*child, sizes[i], pass));
            float offset = align == 1 ? (cross - size) * 0.5f : align == 2 ? cross - size : 0;
            Rect r{rect.x + h.padding[0] + (column ? crossOffset + offset : mainOffset),
                   rect.y + h.padding[1] + (column ? mainOffset : crossOffset + offset),
                   column ? size : sizes[i], column ? sizes[i] : size};
            const size_t at = out.items.size(); auto content = Place(*child, r, visible, out, pass);
            // Nested wrap containers use their measured natural height.
            if (!child->node->children.empty()) { r.height = std::max(r.height, content.height); out.items[at].rect.height = r.height; }
            out.items[at].overflowed = r.x + r.width > rect.x + rect.width || (rect.height > 0 && r.y + r.height > rect.y + rect.height);
            extentX = std::max(extentX, r.x - rect.x + r.width); extentY = std::max(extentY, r.y - rect.y + r.height);
            mainOffset += (column ? r.height : r.width) + h.gap[column];
        }
        crossOffset = column ? extentX - h.padding[0] + h.gap[0] : extentY - h.padding[1] + h.gap[1];
    }
    Size content{active.empty() ? 0 : extentX + h.padding[2], active.empty() ? 0 : extentY + h.padding[3]};
    if (!rect.height) out.items[index].rect.height = content.height;
    return content;
}
}
TreeResult Arrange(const Node& root, Size available) {
    if (!Number(available.width) || !Number(available.height)) throw std::invalid_argument("invalid layout bounds");
    auto measured = Measure(root, 0); TreeResult out; Pass pass;
    out.content = Place(measured, {0, 0, available.width, available.height}, true, out, pass); return out;
}
}
