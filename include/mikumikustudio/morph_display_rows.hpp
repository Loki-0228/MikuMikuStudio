#pragma once

#include "mikumikustudio/display_capacity.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mikumikustudio::morph_display {
// A morph owns one key track, but every visible display-frame occurrence has
// its own row. Build this view from the painted rows without changing the ABI.
class Rows {
public:
    explicit Rows(const mdl::ModelRecord* model) : model_(model) {
        if (!model || !model->morphCount || !model->groupNames) return;
        const auto groupCount = mdl::DisplayGroupCount(*model);
        const auto* groups = static_cast<const mdl::DisplayGroup*>(model->groupNames);
        std::unordered_map<std::uint32_t, int> collapsed;
        for (int row = 1; row < 200; ++row) {
            const auto type = mdl::DisplayRowType(*model, row);
            const auto record = model->boneListRowRecord[row];
            metadata_ = metadata_ || type != 0 || (record < 0 && record != -999);
            if (type != 0) {
                if (type < groupCount && groups[type].flags == 0)
                    collapsed.emplace(type, -row);
            } else if (record < 0 && record != -999) {
                const auto morph = std::uint32_t(-1 - std::int64_t(record));
                if (morph < model->morphCount) Add(morph, row);
            }
        }
        for (std::uint32_t i = 0; model->displayFrames &&
             i < mdl::FacialFrameCount(*model); ++i) {
            const auto& face = model->displayFrames[i];
            const auto found = collapsed.find(mdl::FrameGroupIndex(face) + 1);
            const auto morph = mdl::FrameTargetIndex(face);
            if (found != collapsed.end() && morph < model->morphCount)
                Add(morph, found->second);
        }
    }

    bool HasMetadata() const { return metadata_; }

    template<class Function>
    void ForEach(std::uint32_t morph, Function function) const {
        if (!model_ || morph >= model_->morphCount) return;
        if (!metadata_) {
            if (!model_->morphKeyIndices) return;
            const int link = static_cast<std::int32_t>(model_->morphKeyIndices[morph]);
            if (link != 0 && link > -200 && link < 200) function(link);
            return;
        }
        const auto found = links_.find(morph);
        if (found != links_.end())
            for (const int link : found->second) function(link);
    }

    template<class Predicate>
    bool Any(std::uint32_t morph, Predicate predicate) const {
        bool found = false;
        ForEach(morph, [&](int link) { found = found || predicate(link); });
        return found;
    }

private:
    void Add(std::uint32_t morph, int link) {
        auto& links = links_[morph];
        if (std::find(links.begin(), links.end(), link) == links.end())
            links.push_back(link);
    }
    const mdl::ModelRecord* model_;
    bool metadata_ = false;
    std::unordered_map<std::uint32_t, std::vector<int>> links_;
};
} // namespace mikumikustudio::morph_display
