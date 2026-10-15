#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace native_ui {
struct Size { float width, height; };
struct Rect { float x, y, width, height; };
struct Item { Size minimum, preferred; bool visible = true; };
struct FlowResult { std::vector<Rect> rects; float height = 0; };
// Deterministic row wrapping in 96-DPI logical units. Hidden items consume no
// space. A narrow host clips/scrolls a minimum-sized item, never scales text.
FlowResult Wrap(const std::vector<Item>&, float width, float gap = 4, float padding = 4);

enum class Flow { Auto, Row, Column, Wrap, Grid };
struct Hints {
    Flow flow = Flow::Auto;
    uint32_t columns = 0, align = 0;
    Size minimum{}, preferred{}, maximum{}, grow{}, shrink{};
    float gap[2]{4, 4}, padding[4]{4, 4, 4, 4};
    bool keepTogether = false, preserveHidden = false;
};
struct Node {
    std::string id;
    Hints hints;
    Size measuredMinimum{}, measuredPreferred{};
    bool visible = true;
    std::vector<Node> children;
};
struct Placement { std::string id; Rect rect{}; bool visible = false, overflowed = false; };
struct TreeResult { std::vector<Placement> items; Size content{}; };
// Coordinates are DIPs. The input tree and preferences are never modified.
// A zero available height means natural height; width always constrains flow.
TreeResult Arrange(const Node&, Size available);
}
