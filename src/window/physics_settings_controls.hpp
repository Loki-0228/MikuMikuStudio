#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include "mikumikustudio/physics_bake_settings.hpp"
#include <array>
#include <charconv>
#include <clocale>
#include <cwchar>

namespace mikumikustudio::physics_settings_controls {

inline constexpr int Rate = 3701, Erp = 3702, Cfm = 3703, Iterations = 3704;
inline constexpr int Hint = 3705, SliderOffset = 100;

class Editor {
    HWND window_ = nullptr;
    int language_ = 0;
    bool updating_ = false;
    bake::Settings settings_;

    const wchar_t* Text(const wchar_t* zh, const wchar_t* en, const wchar_t* ja) const {
        return language_ == 2 ? zh : language_ == 1 ? en : ja;
    }
    HWND Add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
             int x, int y, int width, int height, DWORD extended = 0) {
        RECT rect{x,y,x+width,y+height}; MapDialogRect(window_, &rect);
        HWND control = CreateWindowExW(extended, type, text, WS_CHILD|WS_VISIBLE|style,
            rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, window_,
            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, SendMessageW(window_, WM_GETFONT, 0, 0), FALSE);
        return control;
    }
    static std::wstring Number(float value) {
        char text[48]; const auto end = std::to_chars(text, text+sizeof text, value);
        return {text, end.ptr};
    }
    void Write(int id, const std::wstring& text) { SetDlgItemTextW(window_, id, text.c_str()); }
    bool Real(int id, double& value) const {
        wchar_t text[80]{}; GetDlgItemTextW(window_, id, text, 80);
        const auto locale = _create_locale(LC_NUMERIC, "C");
        if (!locale) return false;
        wchar_t* end = nullptr; value = _wcstod_l(text, &end, locale); _free_locale(locale);
        return end != text && *end == 0 && std::isfinite(value) && value >= 0;
    }
    bool Integer(int id, int& value) const {
        double parsed = 0;
        if (!Real(id, parsed) || parsed < 1 || parsed > (std::numeric_limits<int>::max)() || std::floor(parsed) != parsed) return false;
        value = static_cast<int>(parsed); return true;
    }
    static double PositionValue(int id, int position) {
        if (id == Rate) return std::round(15 + 465.0*position/1000);
        if (id == Iterations) return std::round(1 + 99.0*position/1000);
        if (id == Erp) return .8*position/1000;
        return position ? std::pow(10.0, -6 + 5.0*(position-1)/999) : 0;
    }
    void SyncSlider(int id) {
        double value = 0; if (!Real(id, value)) return;
        double position = 0;
        if (id == Rate) position = (value-15)*1000/465;
        else if (id == Iterations) position = (value-1)*1000/99;
        else if (id == Erp) position = value*1000/.8;
        else if (value > 0) position = 1 + (std::log10(value)+6)*999/5;
        // A manual value outside the slider range is kept verbatim.
        SendDlgItemMessageW(window_, id+SliderOffset, TBM_SETPOS, TRUE,
            static_cast<int>((std::clamp)(position, 0.0, 1000.0)));
    }
    bool Manual(bake::Settings& settings) const {
        double erp = 0, cfm = 0;
        if (!Real(Erp,erp) || erp > 1 || !Real(Cfm,cfm) ||
            !Integer(Iterations,settings.solverIterations)) return false;
        wchar_t text[80]{}; GetDlgItemTextW(window_,Cfm,text,80);
        const auto locale = _create_locale(LC_NUMERIC,"C");
        if (!locale) return false;
        const float parsed = _wcstof_l(text,nullptr,locale); _free_locale(locale);
        if (!std::isfinite(parsed)) return false;
        settings.erp = static_cast<float>(erp); settings.cfm = parsed;
        return true;
    }
    void Refresh() {
        auto values = settings_;
        std::wstring error;
        std::wstring hint=Text(
            L"自动补偿时间步，无需模式开关。ERP/CFM 按标注的基准频率输入；迭代不增加步数。输入框可超出滑块范围。",
            L"Time-step compensation is automatic. ERP/CFM use the labeled reference rate. Iterations do not advance time. Exact input may exceed the sliders.",
            L"時間ステップを自動補正。ERP/CFM は表示した基準周波数の値です。反復は時間を進めません。数値欄は範囲外も入力可能です。");
        if (Read(values,error)) {
            const auto tuned=bake::SolverSettings(values,values.baseRateHz);
            hint+=Text(L"\n实际 ERP：",L"\nApplied ERP: ",L"\n実効 ERP: ");
            hint+=Number(tuned.erp)+L"   CFM: "+Number(tuned.cfm);
        }
        SetDlgItemTextW(window_,Hint,hint.c_str());
    }
public:
    void Create(HWND window,int language,int x,int y,int width,const bake::Settings& settings) {
        window_=window; language_=language;
        INITCOMMONCONTROLSEX controls{sizeof controls,ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        const std::array<const wchar_t*,4> labels{{
            Text(L"物理帧率（Hz）",L"Physics rate (Hz)",L"物理周波数（Hz）"),
            Text(L"误差修正 ERP",L"Error reduction ERP",L"誤差補正 ERP"),
            Text(L"关节柔化 CFM",L"Joint softness CFM",L"ジョイント柔軟性 CFM"),
            Text(L"每步求解迭代",L"Iterations per step",L"ステップ毎の反復")}};
        for (int i=0;i<4;++i) {
            const int id=Rate+i, row=y+7+i*31;
            Add(L"STATIC",labels[i],SS_LEFT,3900+i,x,row+2,133,14);
            Add(TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,id+SliderOffset,x+136,row-1,width-220,20);
            SendDlgItemMessageW(window_,id+SliderOffset,TBM_SETRANGE,TRUE,MAKELPARAM(0,1000));
            SendDlgItemMessageW(window_,id+SliderOffset,TBM_SETPAGESIZE,0,50);
            Add(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,id,x+width-77,row,77,17,WS_EX_CLIENTEDGE);
            SendDlgItemMessageW(window_,id,EM_SETLIMITTEXT,64,0);
        }
        Add(L"STATIC",L"",SS_LEFT,Hint,x,y+134,width,58);
        Set(settings);
    }
    void Set(const bake::Settings& settings) {
        updating_=true; settings_=settings;
        Write(Rate,std::to_wstring(bake::SimulationRate(settings)));
        Write(Erp,Number(settings.erp)); Write(Cfm,Number(settings.cfm));
        Write(Iterations,std::to_wstring(settings.solverIterations));
        const auto reference=L" ("+std::to_wstring(settings.parameterReferenceRateHz)+L" Hz)";
        Write(3901,std::wstring(Text(L"误差修正 ERP",L"Error reduction ERP",L"誤差補正 ERP"))+reference);
        Write(3902,std::wstring(Text(L"关节柔化 CFM",L"Joint softness CFM",L"柔軟性 CFM"))+reference);
        for(int id:{Rate,Erp,Cfm,Iterations}) SyncSlider(id);
        updating_=false;
        Refresh();
    }
    bool Handle(UINT message,WPARAM wp,LPARAM lp) {
        if (!window_ || updating_) return false;
        if (message==WM_HSCROLL && lp) {
            const int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp))-SliderOffset;
            if (id<Rate || id>Iterations) return false;
            const int position=static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0));
            const double value=PositionValue(id,position);
            Write(id,id==Rate||id==Iterations?std::to_wstring(static_cast<int>(value)):Number(static_cast<float>(value)));
            return true;
        }
        if (message!=WM_COMMAND) return false;
        const int id=LOWORD(wp),code=HIWORD(wp);
        if (id>=Rate && id<=Iterations && code==EN_CHANGE) {
            SyncSlider(id);
            Refresh();
            return true;
        }
        return false;
    }
    bool Read(bake::Settings& output,std::wstring& error) const {
        auto proposed=settings_;
        if (!Integer(Rate,proposed.baseRateHz)) {
            error=Text(L"物理帧率须为可表示的正整数。",L"Physics rate must be a representable positive integer.",L"物理周波数は表現可能な正の整数です。"); return false;
        }
        proposed.substeps=proposed.rateMultiplier=1;
        if (!Manual(proposed)) {
            error=Text(L"ERP 须为 0–1，CFM 须为非负有限数，迭代次数须为正整数。",L"ERP must be 0–1, CFM finite and nonnegative, and iterations a positive integer.",L"ERP は 0–1、CFM は非負の有限数、反復回数は正の整数です。"); return false;
        }
        if (!bake::ValidParameters(proposed)) return false;
        output=std::move(proposed); error.clear(); return true;
    }
};
}
