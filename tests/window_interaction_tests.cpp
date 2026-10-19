// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// User-acceptance regressions for window interaction.
//
// Field report (defect 8): clicking a bone in the bone list did not select it.
//   Root cause chain (see ui_editor_click.cpp / wndproc.cpp):
//     * the label column keeps one record per drawn row; a bone row stores the
//       bone index, morph rows store -1-morphIndex and unpainted rows keep the
//       -999 sentinel.  The click handler treated every record that was not
//       exactly -999 as "no row" for morphs and, before the fix, could index
//       boneSelection with a record outside boneCount;
//     * the press was judged on the *stored* mouse position, which
//       HandleMouseMove only refreshes after its auto-hide gates, so a press
//       without a preceding stored move was routed to a row that was never
//       painted (record -999, type 0) - the selection was cleared and nothing
//       was selected.
//
// Field report (defect 9): the F key "paste frame" dialog was mojibake and its
// OK button did nothing.
//   * mojibake: the JP literals are byte-exact Shift-JIS; MessageBoxA decodes
//     them with the system code page (CP936 here), so every Japanese line came
//     out garbled.  The path must decode CP932 explicitly.
//   * "OK does nothing": the command silently returned when no frame data was
//     copied, i.e. the confirmation appeared, OK was pressed and no paste ran.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"

namespace mikumikustudio {
// Non-exported helpers under test (definitions in the files noted).
int BoneListRowBoneIndex(const mikumikustudio::mdl::ModelRecord* record,
                         std::int32_t row);   // ui_editor_click.cpp
std::int32_t BoneListRowMorphIndex(
    const mikumikustudio::mdl::ModelRecord* record,
    std::int32_t row);                        // ui_editor_click.cpp
enum class PasteFrameGate {
    Ready,
    NoClipboardData,
    NoTarget,
};
PasteFrameGate PasteFrameGateOf(bool accessoryMode, std::uint32_t copiedFrames,
                                bool targetSelected);  // command_file_menu.cpp
}  // namespace mikumikustudio

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

namespace {

using mikumikustudio::BoneListRowBoneIndex;
using mikumikustudio::BoneListRowMorphIndex;
using mikumikustudio::PasteFrameGate;
using mikumikustudio::PasteFrameGateOf;
namespace mdl = mikumikustudio::mdl;
namespace enc = mikumikustudio::text_encoding;

// ---------------------------------------------------------------------------
// Defect 8: "row -> bone index" mapping of the bone list.
// ---------------------------------------------------------------------------
void BoneListRowMapping() {
    mdl::ModelRecord record{};
    record.boneCount = 8;
    for (int i = 0; i < 200; ++i)
        record.boneListRowRecord[i] = -999;

    // Painted bone rows carry the bone index; the first row of the list is
    // the model's display root bone (PostLanguageSweep writes record[0]).
    record.boneListRowRecord[0] = 3;
    record.boneListRowRecord[4] = 7;
    // Painted morph rows carry -1-morphIndex; frame rows keep -999.
    record.boneListRowRecord[5] = -1;    // morph 0
    record.boneListRowRecord[6] = -42;   // morph 41
    record.boneListRowRecord[7] = -999;  // a frame row: no bone, no morph
    // A stale record (painter wrote it for another model) must not be used.
    record.boneListRowRecord[8] = 99;

    Check(BoneListRowBoneIndex(&record, 0) == 3,
          "row 0 maps to the display root bone");
    Check(BoneListRowBoneIndex(&record, 4) == 7,
          "a painted bone row maps to its bone index");
    Check(BoneListRowBoneIndex(&record, 5) == -1 &&
              BoneListRowBoneIndex(&record, 6) == -1 &&
              BoneListRowBoneIndex(&record, 7) == -1,
          "morph rows and unpainted rows carry no bone index");
    // Before the fix the record was used as-is, so row 8 addressed bone 99 of
    // an 8-bone model - the read/write of boneSelection[99] was out of bounds
    // and the "bone" could never be selected.
    Check(BoneListRowBoneIndex(&record, 8) == -1,
          "a record outside boneCount is rejected");
    Check(BoneListRowBoneIndex(&record, -1) == -1 &&
              BoneListRowBoneIndex(&record, 200) == -1,
          "rows outside the 200-entry table are rejected");
    Check(BoneListRowBoneIndex(nullptr, 0) == -1, "no model, no bone row");
    std::puts("PASS bone list row -> bone index mapping");

    // Morph rows: every negative record is -1-morphIndex, so the morph branch
    // must run for morph 0 (-1) too.  Before the fix only the -999 sentinel
    // was recognised, so clicking a morph row did nothing at all.
    Check(BoneListRowMorphIndex(&record, 5) == 0,
          "record -1 is morph row 0");
    Check(BoneListRowMorphIndex(&record, 6) == 41,
          "record -42 is morph row 41");
    Check(BoneListRowMorphIndex(&record, 0) == -1 &&
              BoneListRowMorphIndex(&record, 7) == -1 &&
              BoneListRowMorphIndex(&record, 8) == -1,
          "bone rows, -999 rows and stale rows are not morph rows");
    Check(BoneListRowMorphIndex(&record, 200) == -1,
          "out-of-range rows are not morph rows");
    std::puts("PASS bone list row -> morph index mapping");
}

// ---------------------------------------------------------------------------
// Defect 9: dialog text conversion (CP932 -> UTF-16, never the ACP) and the
// F-key paste preconditions that decide whether OK applies anything.
// ---------------------------------------------------------------------------
void PasteDialogText() {
    // 0x530A88 "別ﾌﾚｰﾑへﾍﾟｰｽﾄ" (case 250 caption, byte-exact Shift-JIS).
    const char kCaptionJp[] =
        "\x95\xCA\xCC\xDA\xB0\xD1\x82\xD6\xCD\xDF\xB0\xBD\xC4";
    // 0x530878 "操作対象ボーン(画面左上に表示)を選択して下さい".
    const char kSelectBoneJp[] =
        "\x91\x80\x8D\xEC\x91\xCE\x8F\xDB\x83\x7B\x81\x5B\x83\x93\x28\x89\xE6"
        "\x96\xCA\x8D\xB6\x8F\xE3\x82\xC9\x95\x5C\x8E\xA6\x29\x82\xF0\x91\x49"
        "\x91\xF0\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2";
    // The system code page of the porting box is 936; the point of the test is
    // that the JP box never depends on it.
    std::wstring jp;
    Check(enc::Decode(kCaptionJp, 932, jp),
          "the case 250 caption decodes as CP932");
    Check(jp == std::wstring(L"\u5225\uFF8C\uFF9A\uFF70\uFF91\u3078\uFF8D\uFF9F"
                             L"\uFF70\uFF7D\uFF84"),
          "the caption text is the original Japanese, not mojibake");
    std::wstring acp;
    // The system page either rejects the JP bytes outright or maps them to
    // different characters - either way the MessageBoxA path cannot render the
    // Japanese caption, which is exactly the reported mojibake.
    const bool acpDecoded = enc::Decode(kCaptionJp, 936, acp);
    Check(!acpDecoded || acp != jp,
          "the system code page (CP936) never yields the Japanese caption: the "
          "decode fails or the characters differ");
    std::wstring select;
    Check(enc::Decode(kSelectBoneJp, 932, select) &&
              select == std::wstring(
                  L"\u64CD\u4F5C\u5BFE\u8C61\u30DC\u30FC\u30F3(\u753B\u9762"
                  L"\u5DE6\u4E0A\u306B\u8868\u793A)\u3092\u9078\u629E\u3057"
                  L"\u3066\u4E0B\u3055\u3044"),
          "the 'please select bone' box text decodes as CP932");
    std::puts("PASS paste dialog JP text decodes as CP932 (no ACP mojibake)");
}

void PasteFramePreconditions() {
    // Model mode: the bone clipboard decides, the selected bone is the target.
    Check(PasteFrameGateOf(false, 0, true) == PasteFrameGate::NoClipboardData,
          "model mode without copied bone frames is gated before the paste");
    Check(PasteFrameGateOf(false, 12, false) == PasteFrameGate::NoTarget,
          "model mode without a selected bone is gated");
    Check(PasteFrameGateOf(false, 12, true) == PasteFrameGate::Ready,
          "model mode with copied frames and a bone pastes");
    // Display mode: accessories.
    Check(PasteFrameGateOf(true, 0, true) == PasteFrameGate::NoClipboardData,
          "display mode without copied accessory frames is gated");
    Check(PasteFrameGateOf(true, 5, false) == PasteFrameGate::NoTarget,
          "display mode without a selected accessory is gated");
    Check(PasteFrameGateOf(true, 5, true) == PasteFrameGate::Ready,
          "display mode with copied frames and an accessory pastes");
    // The gate must be reachable in the order the command uses it: clipboard
    // first (before the confirmation), target second (after it).
    Check(PasteFrameGateOf(false, 0, false) ==
              PasteFrameGate::NoClipboardData,
          "the clipboard gate fires before the target gate");
    std::puts("PASS F-key paste preconditions (no silent OK)");
}

}  // namespace

int main() {
    BoneListRowMapping();
    PasteDialogText();
    PasteFramePreconditions();
    std::puts("Window interaction regressions passed");
    return 0;
}
