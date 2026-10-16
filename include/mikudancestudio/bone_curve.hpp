#pragma once
#include "mikudancestudio/animation_curve.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include "mikudancestudio/build_profile.hpp"
#include <array>
#include <memory>

namespace mikudancestudio {
class MMDApp;
namespace bone_scale { struct Matrix; }
namespace bone_curve {
// Explicit free-curve mode: translation, Y-X-Z Euler angles in degrees, scale.
// Native MMD interpolation remains available until a free curve is committed.
using Tracks = std::array<animation_curve::Track, 9>;
bool Read(const unsigned char* model, int bone, Tracks& out);
bool Has(const unsigned char* model, int bone);
bool Write(unsigned char* model, int bone, const Tracks&, std::string& error);
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
#if !MMD_ORIGINAL_PROFILE
class AuthoringRestore;
// Owned free-curve authoring only: tracks, stored native hashes and identity.
// Capture does not call Read/Has, prune stale curves, or copy the 30-slot rings.
// Empty curves and missing storage are equivalent; restoring empty curves
// leaves any existing ring storage intact. The caller retains the model and
// validates its external scene/pool identities before using this pointer API.
class AuthoringState {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AuthoringState();
    friend class AuthoringRestore;
public:
    ~AuthoringState();
    AuthoringState(AuthoringState&&) noexcept;
    AuthoringState& operator=(AuthoringState&&) noexcept;
    AuthoringState(const AuthoringState&) = delete;
    AuthoringState& operator=(const AuthoringState&) = delete;
    // Allocation/invalid-owner failures throw before changing live state.
    static std::unique_ptr<AuthoringState> Capture(const unsigned char* model);
    std::unique_ptr<AuthoringState> Clone() const;
    bool Equals(const AuthoringState&) const noexcept;
    // Remove one bone from this private snapshot when planning native edits.
    bool Invalidate(int bone) noexcept;
    // Samples this private snapshot without reading or pruning live state.
    // Only a matching target native-track hash enables the free curve. The
    // incoming scale supplies the native track's matrix basis; matching
    // curves replace position/rotation and its three scale magnitudes.
    // False leaves outputs untouched. No allocation or native-key lookup.
    bool Sample(int bone, double frame, std::uint64_t targetNativeHash,
        float position[3], float rotation[4], bone_scale::Matrix& scale) const noexcept;
    // Checked charge using actual key-vector capacities and map node counts;
    // includes a conservative node/link allowance, not CRT heap bookkeeping.
    std::uint64_t RetainedBytes() const noexcept;
};
class AuthoringRestore {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AuthoringRestore();
public:
    ~AuthoringRestore();
    AuthoringRestore(AuthoringRestore&&) noexcept;
    AuthoringRestore& operator=(AuthoringRestore&&) noexcept;
    AuthoringRestore(const AuthoringRestore&) = delete;
    AuthoringRestore& operator=(const AuthoringRestore&) = delete;
    // Prepares copies and any missing std::map node entirely in private
    // storage. Cancellation has no effect on live curves or ring storage.
    static std::unique_ptr<AuthoringRestore> Prepare(unsigned char* model,const AuthoringState&);
    // Compares only side-table values/stored hashes and model bone metadata.
    // Does not recompute hashes from native keys, which the host may publish
    // immediately before publishing this independently prepared participant.
    bool IsCurrent() const noexcept;
    bool Publish() noexcept; // revalidates; consumes once; no allocation
};
#endif
}
}
