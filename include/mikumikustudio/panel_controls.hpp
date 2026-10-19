// ===========================================================================
// MikuMikuStudio - main-window panel & dialog control IDs
// ===========================================================================
// Single home for the control IDs that used to appear as scattered decimal /
// hexadecimal literals (the old code even ran both tracks in parallel:
// GetDlgItem(hwnd, 686) next to GetDlgItem(hDlg, 0x2AE)).
//
// Values are pinned to the original binary:
//   * main-window panel controls 400..567 - creation table
//     src/window/ui_controls.inc (decompiled 0x00466D20), JP captions in the
//     table, EN captions in src/window/localize_ui.cpp kEnTexts;
//   * dialog controls 600..817 - res/templates/dialog/dialogs.rc
//     (JP/EN template twins share control IDs);
//   * per-ID behavior cross-checked against the WM_COMMAND dispatchers
//     (command_control_400/450/500.cpp) and the loader/UI refresh code.
//
// IDs are dialog-local in places: the same number can mean different
// controls in different dialogs (e.g. 628 is the codec combo in the AVI
// dialog but a list box in the ordering dialogs; 637..639 are position
// edits in the numeric-input dialogs and direction sliders in the gravity
// dialog).  Where that happens both names are defined with the same value
// and each call site uses the name that matches its dialog.
//
// NOTE: src/window/physics_model_dialog.cpp keeps its own file-local
// constants (kSizeXEdit & friends); they were deliberately NOT migrated.
// ===========================================================================
#pragma once

namespace mikumikustudio {
namespace panel {

// ---- main window: frame / timeline row ------------------------------------
constexpr int kUndoButton = 400;             // 0x190 元に戻す / undo
constexpr int kRedoButton = 401;             // 0x191 やり直し / redo
constexpr int kPlayButton = 408;             // 0x198 再生 / play
constexpr int kPlayStartFrameEdit = 409;     // 0x199 frame-zone start edit (ﾌﾚｿﾞｰﾄ)
constexpr int kPlayStopFrameEdit = 410;      // 0x19A frame-zone stop edit (ﾌﾚｿﾞﾄｯﾌﾟ)
constexpr int kCurrentFrameEdit = 417;       // 0x1A1 current-frame edit
constexpr int kRangeStartEdit = 425;         // 0x1A9 range-select start edit (mirror of dialog 686)
constexpr int kRangeStopEdit = 426;          // 0x1AA range-select stop edit (mirror of dialog 687)
constexpr int kTimelineVScroll = 427;        // 0x1AB timeline vertical scrollbar
constexpr int kTimelineHScroll = 428;        // 0x1AC timeline horizontal scrollbar
constexpr int kPasteButton = 421;            // 0x1A5 ペースト / paste (camera/light/acc)
constexpr int kForcePasteButton = 250;       // Same command as F / Ctrl+F and the Edit menu.
constexpr int kReversePasteButton = 422;     // 0x1A6 反転P / reverse paste
constexpr int kExpandShrinkButton = 424;     // 0x1A8 拡大縮小 / frame-range scale dialog
constexpr int kCurveCopyButton = 430;        // 0x1AE 補間曲線コピー (curve-panel copy)
constexpr int kCurvePasteButton = 431;       // 0x1AF 補間曲線ペースト (curve-panel paste)
constexpr int kCurveLinearButton = 432;      // 0x1B0 線形補間 (curve reset to linear)

// ---- main window: camera / light / accessory register panel ----------------
constexpr int kCameraRefModelCheckbox = 412; // 0x19C camera reference = model root (vs 531)
constexpr int kInterpCurveCombo = 433;       // 0x1B1 interpolation channel (Ｘ移動..すべて)
constexpr int kRegisterScopeCombo = 434;     // 0x1B2 frame-op scope / accessory list (mode-dependent)
constexpr int kMainComboModel = 436;         // 0x1B4 camera･light･accessory / model selector
constexpr int kModelVisibleCheckbox = 439;   // 0x1B7 model-visible toggle (mirrors loadComplete)
constexpr int kShadowCheckbox = 440;         // 0x1B8 影 / shadow toggle
constexpr int kAddBlendCheckbox = 441;       // 0x1B9 加算 / additive blend toggle
constexpr int kIkChainCombo = 443;           // 0x1BB IK-chain selector
constexpr int kIkOnRadio = 444;              // 0x1BC IK on
constexpr int kIkOffRadio = 445;             // 0x1BD IK off
constexpr int kPerspectiveCheckbox = 446;    // 0x1BE パース / perspective toggle
constexpr int kFovSlider = 447;              // 0x1BF camera FOV trackbar (1..125)
constexpr int kFovEdit = 448;                // 0x1C0 camera FOV edit
constexpr int kMainComboNormal = 449;        // 0x1C1 "non" + model names (bone-register model)
constexpr int kBoneRegisterCombo = 450;      // 0x1C2 bone-register combo (RefillBoneRegisterCombo)
constexpr int kLightColorSliderR = 455;      // 0x1C7 light color R trackbar (0..255)
constexpr int kLightColorSliderG = 456;      // 0x1C8 light color G trackbar
constexpr int kLightColorSliderB = 457;      // 0x1C9 light color B trackbar
constexpr int kLightDirSliderX = 458;        // 0x1CA light direction X trackbar (-100..100)
constexpr int kLightDirSliderY = 459;        // 0x1CB light direction Y trackbar
constexpr int kLightDirSliderZ = 460;        // 0x1CC light direction Z trackbar
constexpr int kLightColorEditR = 461;        // 0x1CD light color R edit
constexpr int kLightColorEditG = 462;        // 0x1CE light color G edit
constexpr int kLightColorEditB = 463;        // 0x1CF light color B edit
constexpr int kLightDirEditX = 464;          // 0x1D0 light direction X edit
constexpr int kLightDirEditY = 465;          // 0x1D1 light direction Y edit
constexpr int kLightDirEditZ = 466;          // 0x1D2 light direction Z edit

// ---- main window: accessory panel ------------------------------------------
constexpr int kAccessoryCombo = 471;         // 0x1D7 accessory selector
constexpr int kMainComboGround = 474;        // 0x1DA "ground" + model names (accessory parent model)
constexpr int kAttachBoneCombo = 475;        // 0x1DB accessory / self-shadow attach (parent) bone
constexpr int kAccessoryVisibleCheckbox = 476;  // 0x1DC accessory display toggle (byte 0x210)
constexpr int kAccessoryAddBlendCheckbox = 477; // 0x1DD 加算 / accessory additive blend
constexpr int kAccPosXEdit = 478;            // 0x1DE accessory position X edit
constexpr int kAccPosYEdit = 479;            // 0x1DF accessory position Y edit
constexpr int kAccPosZEdit = 480;            // 0x1E0 accessory position Z edit
constexpr int kAccRotXEdit = 481;            // 0x1E1 accessory rotation X edit (rad->deg echo)
constexpr int kAccRotYEdit = 482;            // 0x1E2 accessory rotation Y edit
constexpr int kAccRotZEdit = 483;            // 0x1E3 accessory rotation Z edit
constexpr int kAccScaleXEdit = 484;          // 0x1E4 accessory scale X edit (%)
constexpr int kAccScaleYEdit = 485;          // 0x1E5 accessory scale Y edit (%)
constexpr int kAccessoryShadowCheckbox = 486;   // 0x1E6 accessory shadow toggle (byte 0x49C)

// ---- main window: bone-manipulation mode group ------------------------------
constexpr int kBoneSelectRadio = 490;        // 0x1EA 選択 / select mode
// 0x1EB was the BOX選択 radio.  MMS repurposes it into the bone scale mode of
// the model panel's second row; the merged select / box-select cycle button
// (kBoneSelectRadio) carries the box-select state instead.
constexpr int kBoxSelectRadio = 491;         // 0x1EB MMS: bone scale mode
constexpr int kBoneScaleRadio = 491;         // 0x1EB MMS: bone scale mode
constexpr int kBoneMoveRadio = 492;          // 0x1EC 移動 / move mode
constexpr int kBoneRotateRadio = 493;        // 0x1ED 回転 / rotate mode
constexpr int kBoneSelectAllButton = 494;    // 0x1EE 全て選択 / select all bones
constexpr int kBoneResetButton = 495;        // 0x1EF 初期化 / reset selected bones
constexpr int kBoneCopyButton = 496;         // 0x1F0 コピー / bone copy
constexpr int kBonePasteButton = 497;        // 0x1F1 bone-frame paste
constexpr int kBoneReversePasteButton = 498; // 0x1F2 bone-frame reverse paste
constexpr int kPhysicsCheckbox = 499;        // 0x1F3 物理演算 / physics on-off
constexpr int kRegisterFrameButton = 500;    // 0x1F4 登録 / keyframe register
constexpr int kSelectUnregisteredButton = 501;  // 0x1F5 未登録選 / unregistered

// MMS addition: geometry of the bone-operations grid in the model panel's
// option band 4 of HandleWindowSize.  The band is anchored to the window's
// bottom right corner and its cells never scale with the window, so the table
// holds absolute x positions and bottom offsets.  HandleWindowSize walks it and
// the panel regression test reads the same numbers, which is what pins the
// three-language captions inside their cells.
//
// Columns are 64 pixels apart (60 wide) and the mode rows are 35 pixels apart
// with the original's 28-pixel mode-row height; the plain rows keep 24.  The
// physics toggle keeps its original 48x28 cell at x=500.
struct BoneModeCell {
    int id;
    int x;
    int yOffset;   // distance from the client-area bottom to the cell's top
    int width;
    int height;
};
constexpr BoneModeCell kBoneModeGrid[] = {
    // Row 1: select, rotate, move - the original's mode row.
    {kBoneSelectRadio, 366, 140, 60, 28},
    {kBoneRotateRadio, 430, 140, 60, 28},
    {kBoneMoveRadio, 494, 140, 60, 28},
    // Row 2: bone scale (in the original's BOX-sel cell; box selection is
    // Shift + drag in select mode), select all, select unregistered.
    {kBoneScaleRadio, 366, 105, 60, 28},
    {kBoneSelectAllButton, 430, 105, 60, 28},
    {kSelectUnregisteredButton, 494, 105, 60, 28},
    // Row 3: copy, paste, reverse paste.
    {kBoneCopyButton, 366, 70, 60, 24},
    {kBonePasteButton, 430, 70, 60, 24},
    {kBoneReversePasteButton, 494, 70, 60, 24},
    // Row 4: register, reset, physics.
    {kRegisterFrameButton, 366, 36, 60, 28},
    {kBoneResetButton, 430, 32, 60, 24},
    {kPhysicsCheckbox, 500, 36, 48, 28},
};
constexpr int kBoneModeGridCount =
    sizeof(kBoneModeGrid) / sizeof(kBoneModeGrid[0]);

// ---- main window: morph lanes (4 slots: combo / slider / edit) --------------
constexpr int kMorphCombo0 = 504;            // 0x1F8 morph selector, lane 0
constexpr int kMorphSlider0 = 505;           // 0x1F9 morph slider, lane 0 (0..100)
constexpr int kMorphEdit0 = 506;             // 0x1FA morph value edit, lane 0
constexpr int kMorphCombo1 = 509;            // 0x1FD morph selector, lane 1
constexpr int kMorphSlider1 = 510;           // 0x1FE morph slider, lane 1
constexpr int kMorphEdit1 = 511;             // 0x1FF morph value edit, lane 1
constexpr int kMorphCombo2 = 514;            // 0x202 morph selector, lane 2
constexpr int kMorphSlider2 = 515;           // 0x203 morph slider, lane 2
constexpr int kMorphEdit2 = 516;             // 0x204 morph value edit, lane 2
constexpr int kMorphCombo3 = 519;            // 0x207 morph selector, lane 3
constexpr int kMorphSlider3 = 520;           // 0x208 morph slider, lane 3
constexpr int kMorphEdit3 = 521;             // 0x209 morph value edit, lane 3

// ---- main window: option / display toggles ---------------------------------
constexpr int kPhysicsFrameCheckbox = 530;   // 0x212 physics ON/OFF frame toggle
constexpr int kCameraRefBoneCheckbox = 531;  // 0x213 camera reference = selected bone (vs 412)
constexpr int kFrameVolumeSlider = 534;      // 0x216 frame-volume slider (100 - FrameNormalization)
constexpr int kFollowCameraCheckbox = 535;   // 0x217 追従 / camera follow model
constexpr int kModelEditToggle = 536;        // モデル編 / "To model" - panel mode toggle
constexpr int kCameraDistanceButton = 543;   // 距離 / camera distance reset
constexpr int kReadoutPosXEdit = 544;        // 0x220 camera/bone position X readout
constexpr int kReadoutPosYEdit = 545;        // 0x221 position Y readout
constexpr int kReadoutPosZEdit = 546;        // 0x222 position Z readout
constexpr int kReadoutRotXEdit = 547;        // 0x223 camera/bone angle X readout (deg)
constexpr int kReadoutRotYEdit = 548;        // 0x224 angle Y readout
constexpr int kReadoutRotZEdit = 549;        // 0x225 angle Z readout
constexpr int kReadoutDistEdit = 550;        // 0x226 camera distance readout (model-mode extra)
constexpr int kReadoutScaleLabel = 570;      // MMS bone-scale section label
constexpr int kReadoutScaleXEdit = 572;      // MMS selected bone scale X
constexpr int kReadoutScaleYEdit = 575;      // MMS selected bone scale Y
constexpr int kReadoutScaleZEdit = 577;      // MMS selected bone scale Z
constexpr int kInfoCheckbox = 551;           // 0x227 情報 / info window
constexpr int kFrameVolumeCheckbox = 552;    // 0x228 省エネ / frame-volume control enable
constexpr int kGotoFrameEdit = 554;          // 0x22A go-to-frame edit
constexpr int kSelfShadowCheckbox = 556;     // 0x22C 美影 / self-shadow enable
constexpr int kCoordAxisCheckbox = 557;      // 0x22D 座標軸 / coordinate axis display
constexpr int kSelfShadowRangeSlider = 560;  // 0x230 self-shadow range trackbar (0..9999)
constexpr int kSelfShadowRangeEdit = 561;    // 0x231 self-shadow range edit
constexpr int kAdaptiveShadowCheckbox = 578;
constexpr int kShadowFilterCheckbox = 579;
constexpr int kEditOffCheckbox = 562;        // 0x232 camera/light/acc edit OFF (EN "off")
constexpr int kEditMode1Checkbox = 563;      // 0x233 EN "mode1" (checked at init)
constexpr int kEditMode2Checkbox = 564;      // 0x234 EN "mode2"

// ---- dialog 600/651: center-position bias ----------------------------------
constexpr int kBiasXEdit = 601;              // 0x259 bias X
constexpr int kBiasYEdit = 602;              // 0x25A bias Y
constexpr int kBiasZEdit = 603;              // 0x25B bias Z

// ---- dialog 606/652: frame-range scale --------------------------------------
constexpr int kScaleRateEdit = 605;          // 倍率 magnification
constexpr int kScaleFromEdit = 686;          // 0x2AE frame FROM (mirror of 425)
constexpr int kScaleToEdit = 687;            // frame TO (mirror of 426)
constexpr int kScaleBoneCheckbox = 688;      // ボーン bone frames
constexpr int kScaleMorphCheckbox = 689;     // 表情 facial frames
constexpr int kScaleDispIkCheckbox = 690;    // 表示･IK･外親 disp/IK/OP frames

// ---- dialog 607/653: output screen size -------------------------------------
constexpr int kScreenWidthEdit = 621;        // 0x26D width
constexpr int kScreenHeightEdit = 622;       // height

// ---- dialog 608/654: AVI output ----------------------------------------------
constexpr int kAviStartFrameEdit = 609;      // 0x261 record frame FROM
constexpr int kAviEndFrameEdit = 610;        // 0x262 record frame TO
constexpr int kAviFpsEdit = 611;             // 0x263 frame rate
constexpr int kAviWidthEdit = 612;           // 0x264 record width
constexpr int kAviHeightEdit = 613;          // 0x265 record height
constexpr int kAviWaveCheckbox = 614;        // 0x266 WAVEも出力する
constexpr int kAviCloseAfterExportCheckbox = 615;
constexpr int kAviCodecCombo = 628;          // 0x274 video compressor (list box in other dialogs)
constexpr int kAviSettingsButton = 724;      // 0x2D4 詳細設定 / codec settings (physics dialog: copy body)
constexpr int kAvi3dVisionCheckbox = 806;    // NVIDIA 3D Vision output
constexpr int kAviLeftHalfCheckbox = 807;    // left half only (Discover)

// ---- dialog 615/655: frame shift ---------------------------------------------
constexpr int kShiftFramesEdit = 616;        // 0x268 frames to shift

// ---- dialog 617/656: register blinking ----------------------------------------
constexpr int kBlinkStartEdit = 618;         // 開始フレーム
constexpr int kBlinkEndEdit = 619;           // 終了フレーム

// ---- dialog 624/658: ground shadow color (modeless) ---------------------------
constexpr int kGroundShadowSlider = 625;     // 0x271 brightness trackbar
constexpr int kGroundShadowEdit = 626;       // 0x272 brightness edit

// ---- dialogs 627/659, 804/805, 810/811: ordering / accessory settings ----------
constexpr int kOrderListBox = 628;           // 0x274 accessory / model order list
constexpr int kAccessoryNameEdit = 629;      // 0x275 accessory name (rename)
constexpr int kOrderUpButton = 630;          // 上 / move up
constexpr int kOrderDownButton = 631;        // 下 / move down
constexpr int kDialogOkButton = 632;         // OK (決定)
constexpr int kAccessoryOrderEdit = 635;     // 0x27B order-number edit

// ---- dialogs 648/663, 649/664: numeric input (bone / camera) -------------------
constexpr int kNumInputPosXEdit = 637;       // 0x27D 位置X (gravity dialog: dir-X slider)
constexpr int kNumInputPosYEdit = 638;       // 0x27E 位置Y (gravity dialog: dir-Y slider)
constexpr int kNumInputPosZEdit = 639;       // 0x27F 位置Z (gravity dialog: dir-Z slider)
constexpr int kNumInputRotXEdit = 640;       // X軸 angle X
constexpr int kNumInputRotYEdit = 641;       // Y軸
constexpr int kNumInputRotZEdit = 642;       // Z軸
constexpr int kNumInputDistEdit = 644;       // 距離 distance (camera variant only)

// ---- dialog 645/662: edge thickness (modeless) --------------------------------
constexpr int kEdgeThicknessEdit = 646;      // 太さ (0..2)
constexpr int kEdgeThicknessSlider = 647;    // paired trackbar

// ---- dialog 665/666: English-name editor ----------------------------------------
constexpr int kModelNameEnEdit = 667;        // 0x29B model name
constexpr int kModelCommentEnEdit = 668;     // model info (multiline)
constexpr int kBoneNameCombo = 669;          // bone list
constexpr int kBonePrevButton = 671;         // <
constexpr int kBoneNextButton = 670;         // >
constexpr int kBoneNameEnEdit = 672;         // 0x2A0 bone English name
constexpr int kMorphNameCombo = 673;         // facial/morph list
constexpr int kMorphNameEnEdit = 676;        // 0x2A4 morph English name
constexpr int kGroupNameCombo = 677;         // division (bone group) list
constexpr int kGroupNameEnEdit = 680;        // 0x2A8 group English name

// ---- physics dialogs (683/684 model, 802/803 gravity) ----------------------------
// 704..767 body/joint editor IDs live in physics_model_dialog.cpp constants;
// only the gravity dialog's reused IDs and cross-file references live here.
constexpr int kGravityAccelEdit = 709;       // 0x2C5 gravity acceleration (physics dialog: size X)
constexpr int kGravityDirXEdit = 710;        // 0x2C6 gravity dir X (physics dialog: size Y)
constexpr int kGravityDirYEdit = 711;        // 0x2C7 gravity dir Y (physics dialog: size Z)
constexpr int kGravityDirZEdit = 712;        // gravity dir Z (physics dialog: pos X)
constexpr int kGravityNoiseEdit = 713;       // noise amount
constexpr int kBodyACombo = 741;             // joint rigid-body A (physics model dialog)
constexpr int kBodyBCombo = 742;             // joint rigid-body B
constexpr int kGravityNoiseCheckbox = 731;   // 0x2DB ﾉｲｽﾞ付加 / noise enable
constexpr int kGravityDirXSlider = 637;      // gravity dir-X slider (numeric dialogs: pos-X edit)
constexpr int kGravityDirYSlider = 638;      // gravity dir-Y slider
constexpr int kGravityDirZSlider = 639;      // gravity dir-Z slider

// ---- dialog 681/682: toon texture paths ------------------------------------------
constexpr int kToon01Edit = 709;             // ﾄｩｰﾝ01 (gravity dialog: acceleration)
constexpr int kToon02Edit = 710;             // ﾄｩｰﾝ02
constexpr int kToon03Edit = 711;             // ﾄｩｰﾝ03
constexpr int kToon04Edit = 712;             // ﾄｩｰﾝ04
constexpr int kToon05Edit = 713;             // ﾄｩｰﾝ05

// ---- dialog 623/657: camera-frame multiply (even = scale, odd = offset) -----------
constexpr int kCamMulPosXScaleEdit = 686;    // camera pos X * (frame-scale dialog: FROM edit)
constexpr int kCamMulPosXOffsetEdit = 687;   // camera pos X + (frame-scale dialog: TO edit)
constexpr int kBoneMulPosXScaleEdit = 686;   // bone-frame multiply pos X * (dialog 636)
constexpr int kBoneMulPosXOffsetEdit = 687;  // bone-frame multiply pos X +
constexpr int kBoneMulScaleXFactorEdit = 698; // MMS: scale X/Y/Z pairs, 698..703
constexpr int kBoneMulScaleXOffsetEdit = 699;
constexpr int kMorphMulScaleEdit = 686;      // facial 大きさ * (dialog 643)
constexpr int kMorphMulOffsetEdit = 687;     // facial 大きさ +

}  // namespace panel
}  // namespace mikumikustudio
