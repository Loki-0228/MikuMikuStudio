#pragma once
#include "mikudancestudio/animation_curve.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include <array>
#include <memory>

namespace mikudancestudio {
class MMDApp;
namespace bone_curve {
// Explicit free-curve mode: translation, Y-X-Z Euler angles in degrees, scale.
// Native MMD interpolation remains available until a free curve is committed.
using Tracks = std::array<animation_curve::Track, 9>;
bool Read(const unsigned char* model, int bone, Tracks& out);
bool Has(const unsigned char* model, int bone);
bool Write(unsigned char* model, int bone, const Tracks&, std::string& error);
// Merge native edits into authored tracks without discarding unrelated curves.
void Synchronize(const unsigned char* model);
// Keep authoring intact while the native range tool deletes and recreates keys.
// Complete installs the explicitly retimed curves against the finished pool.
class RangeRetime {
public:
    RangeRetime(unsigned char* model, int first, int last, double scale);
    ~RangeRetime();
    bool Ready() const;
    bool Complete();
    RangeRetime(const RangeRetime&) = delete;
    RangeRetime& operator=(const RangeRetime&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// A native registration also creates missing lane keys at a shared mirror frame.
void ReconcileRegisteredFrame(const unsigned char* model, int bone, int frame);
void Evaluate(unsigned char* model, double frame);
void Forget(unsigned char* model);
void CopyState(const unsigned char* source, unsigned char* destination);
void ExchangeState(unsigned char* first, unsigned char* second);
void CaptureUndo(unsigned char* model, unsigned ring);
void Undo(unsigned char* model, unsigned ring);
void Redo(unsigned char* model, unsigned ring);
void Reset();
void CapturePmm(const MMDApp&, pmm_extension::Document&);
void LoadPmm(const MMDApp&, const pmm_extension::Document&);
bool CaptureVmd(const unsigned char* model, int offset, std::vector<std::uint8_t>& block, std::string& error);
std::size_t VmdBlockSize(const std::uint8_t*, std::size_t);
bool ApplyVmd(unsigned char* model, const std::uint8_t*, std::size_t, int offset, std::string& error);
// Draws the selected bone's current interval in the main interpolation panel.
// True means a free curve was drawn; native control editing must be disabled.
bool DrawPreview(MMDApp&);
}
}
