#pragma once
#include <native_ui/layout.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace mikudancestudio::plugins::toolbar_overflow {
struct Policy { bool menu = false; bool command = false; bool separator = false; };
struct Result {
    native_ui::TreeResult layout;
    std::set<std::string> hidden;
    std::map<std::string, native_ui::Rect> retained;
    bool menu = false;
};
namespace detail {
struct Unit { std::vector<std::string> leaves; bool command = false; };
inline void Leaves(const native_ui::Node& node, const std::map<std::string, Policy>& policies,
    std::vector<std::string>& leaves, bool& command, bool inherited, bool& requested) {
    if (!node.visible) return;
    const auto found = policies.find(node.id);
    const Policy policy = found == policies.end() ? Policy{} : found->second;
    inherited = inherited || policy.menu; requested = requested || inherited;
    if (node.children.empty()) {
        if (policy.command || policy.separator) leaves.push_back(node.id);
        command = command || policy.command;
    } else for (const auto& child : node.children) Leaves(child, policies, leaves, command, inherited, requested);
}
inline void Units(const native_ui::Node& node, const std::map<std::string, Policy>& policies,
    std::vector<Unit>& units, bool inherited = false, bool root = true) {
    if (!node.visible) return;
    const auto found = policies.find(node.id);
    const Policy policy = found == policies.end() ? Policy{} : found->second;
    inherited = inherited || policy.menu;
    if (!root && !node.children.empty() && node.hints.keepTogether) {
        Unit unit; bool requested = false;
        Leaves(node, policies, unit.leaves, unit.command, inherited, requested);
        // A keep-together group is one presentation unit, including siblings
        // without their own opt-in. Its commands keep their original identity.
        if (requested && !unit.leaves.empty()) units.push_back(std::move(unit));
    } else if (node.children.empty()) {
        if (inherited && (policy.command || policy.separator)) units.push_back({{node.id}, policy.command});
    } else for (const auto& child : node.children) Units(child, policies, units, inherited, false);
}
inline void Hide(native_ui::Node& node, const std::set<std::string>& hidden, bool root = true) {
    if (hidden.count(node.id)) { node.visible = false; node.hints.preserveHidden = false; }
    for (auto& child : node.children) Hide(child, hidden, false);
    if (!root && !node.children.empty() && std::none_of(node.children.begin(), node.children.end(),
        [](const auto& child) { return child.visible || child.hints.preserveHidden; })) {
        node.visible = false; node.hints.preserveHidden = false;
    }
}
}
// Plan in DIPs against the visible toolbar, without editing retained layout or
// allocating new UI objects. Earlier effective order wins. Non-menu elements
// keep the existing clip/scroll behavior. The right-hand menu strip is reserved
// only after a command actually overflows.
inline Result Arrange(const native_ui::Node& original, const std::map<std::string, Policy>& policies,
    float width, float maximumHeight, float menuWidth = 32) {
    Result out; std::vector<detail::Unit> units; detail::Units(original, policies, units);
    if (units.empty()) { out.layout = native_ui::Arrange(original, {width, 0}); return out; }
    bool reserved = false;
    // A bounded number of reflows prevents pathological nested layouts from
    // turning a resize into thousands of full tree measurements. The fallback
    // still exposes every eligible command in the same overflow menu.
    for (unsigned pass = 0;; ++pass) {
        auto tree = original; detail::Hide(tree, out.hidden);
        const float available = std::max(0.0f, width - (reserved ? std::min(width, menuWidth) : 0));
        out.layout = native_ui::Arrange(tree, {available, 0});
        if (!pass) for (const auto& item : out.layout.items) out.retained.emplace(item.id, item.rect);
        std::map<std::string, const native_ui::Placement*> placed;
        for (const auto& item : out.layout.items) placed.emplace(item.id, &item);
        size_t first = units.size();
        for (size_t i = 0; i < units.size(); ++i) for (const auto& id : units[i].leaves) {
            if (out.hidden.count(id)) continue;
            const auto item = placed.find(id);
            if (item == placed.end() || !item->second->visible) continue;
            const auto& r = item->second->rect;
            if (r.x < 0 || r.y < 0 || r.x + r.width > available + 0.01f ||
                r.y + r.height > maximumHeight + 0.01f) first = std::min(first, i);
        }
        if (first == units.size()) break;
        if (pass >= 63) first = 0;
        for (size_t i = first; i < units.size(); ++i) for (const auto& id : units[i].leaves) out.hidden.insert(id);
        reserved = false;
        for (const auto& unit : units) if (unit.command)
            for (const auto& id : unit.leaves) if (out.hidden.count(id)) reserved = true;
    }
    out.menu = reserved;
    return out;
}
}
