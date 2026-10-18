#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
bool ModelHudOperation(MMDApp&,int operation) noexcept;
int ModelHudOperationForMode(int mode) noexcept;
MmsResult BeginModelHudGesture(MMDApp&,SharedHistory&,HWND owner,int operation) noexcept;
bool ModelHudGestureActive(const MMDApp&) noexcept;
std::shared_ptr<const HistoryChange> ModelHudPreviewSource(MMDApp&) noexcept;
bool AllowModelHudPreview(MMDApp&,int operation) noexcept;
MmsResult ApplyModelHudSample(MMDApp&,int operation) noexcept;
void RecordModelHudRefreshHint(MMDApp&) noexcept;
MmsResult EndModelHudGesture(MMDApp&,bool& changed) noexcept;
bool CancelModelHudGesture(MMDApp&) noexcept;
bool HandleModelHudMessage(MMDApp&,HWND,UINT,WPARAM,LPARAM) noexcept;
}
#endif
