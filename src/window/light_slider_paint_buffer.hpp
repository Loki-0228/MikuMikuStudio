#pragma once
// Private, optional native-trackbar paint route. This is not a plugin ABI.
#include "mikumikustudio/build_profile.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <cstdint>

#if !MMD_ORIGINAL_PROFILE
namespace mikumikustudio::ui::light_slider_paint_buffer {
// Synchronous UI-thread reader. It must only read the host's current divider
// flags, must not dispatch messages or edit state, and must outlive Forward.
// A captured main window alone does not identify an internal layout drag.
// Only pass a context for trackbars already attached to the panel template.
struct LayoutContext {
    HWND main=nullptr;
    void* user=nullptr;
    bool (*dragging)(void*) noexcept=nullptr;
};
inline bool CompactLightClient(int id, const RECT& client) noexcept {
    return client.right == (id < 458 ? 136 : 132) &&
        client.bottom == (id < 458 ? 18 : 20);
}
// The default table consists solely of Win7-era Win32/GDI calls. Tests may
// substitute individual calls while the real HWND and native procedure remain.
struct GdiFunctions {
    decltype(&::CreateRectRgn) createRectRgn=&::CreateRectRgn;
    decltype(&::GetUpdateRgn) getUpdateRgn=&::GetUpdateRgn;
    decltype(&::GetDC) getDC=&::GetDC;
    decltype(&::ReleaseDC) releaseDC=&::ReleaseDC;
    decltype(&::CreateCompatibleDC) createCompatibleDC=&::CreateCompatibleDC;
    decltype(&::CreateCompatibleBitmap) createCompatibleBitmap=&::CreateCompatibleBitmap;
    decltype(&::SelectObject) selectObject=&::SelectObject;
    decltype(&::DeleteObject) deleteObject=&::DeleteObject;
    decltype(&::DeleteDC) deleteDC=&::DeleteDC;
    decltype(&::BeginPaint) beginPaint=&::BeginPaint;
    decltype(&::EndPaint) endPaint=&::EndPaint;
    decltype(&::BitBlt) bitBlt=&::BitBlt;
    decltype(&::InvalidateRgn) invalidateRgn=&::InvalidateRgn;
    decltype(&::InvalidateRect) invalidateRect=&::InvalidateRect;
    decltype(&::GetPropW) getProp=&::GetPropW;
    decltype(&::SetPropW) setProp=&::SetPropW;
    decltype(&::RemovePropW) removeProp=&::RemovePropW;
};
namespace detail {
inline constexpr wchar_t kDirectOnce[]=L"MMS.Private.LightSliderPaint.DirectOnce.v1";
inline HANDLE DirectMarker() noexcept {return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));}
struct ActivePaint {
    HWND window=nullptr;HDC memory=nullptr;bool beginning=false,redirectingErase=false;
    bool destroyed=false;
};
inline thread_local ActivePaint* active=nullptr;
inline thread_local bool permanentlyDisabled=false;
inline thread_local const GdiFunctions* testFunctions=nullptr;
inline const GdiFunctions& Functions() noexcept {
    static const GdiFunctions system{};return testFunctions?*testFunctions:system;
}
struct Resources {
    const GdiFunctions& api;HWND window;HRGN original=nullptr,full=nullptr;
    HDC reference=nullptr,memory=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
    Resources(const GdiFunctions& functions,HWND target)noexcept:api(functions),window(target){}
    ~Resources(){
        if(memory&&previous&&previous!=HGDI_ERROR)api.selectObject(memory,previous);
        // Releasing the owned DC also deselects the bitmap if restoration of
        // the previous selection failed. Delete the bitmap only afterwards.
        if(memory)api.deleteDC(memory);if(bitmap)api.deleteObject(bitmap);
        if(reference)api.releaseDC(window,reference);
        if(original)api.deleteObject(original);if(full)api.deleteObject(full);
    }
    Resources(const Resources&)=delete;Resources& operator=(const Resources&)=delete;
};
struct ActiveScope {
    ActivePaint* previous;explicit ActiveScope(ActivePaint& value)noexcept:previous(active){active=&value;}
    ~ActiveScope(){active=previous;}
};
// A null damaged region requests the complete current client, including a
// client resized by a synchronous native/customdraw callback.
inline void RetryDirect(HWND window,HRGN damaged,const GdiFunctions& api)noexcept{
    if(!IsWindow(window))return;
    // A failed property allocation must not create a buffer/failure loop.
    if(!api.setProp(window,kDirectOnce,DirectMarker()))permanentlyDisabled=true;
    const bool invalidated=damaged?api.invalidateRgn(window,damaged,TRUE)!=FALSE:
        api.invalidateRect(window,nullptr,TRUE)!=FALSE;
    if(!invalidated){
        permanentlyDisabled=true;
        api.invalidateRect(window,nullptr,TRUE);
    }
}
inline bool NonemptyUpdate(HWND window,HRGN region,const GdiFunctions& api)noexcept{
    const int kind=api.getUpdateRgn(window,region,FALSE);
    return kind==SIMPLEREGION||kind==COMPLEXREGION;
}
inline bool LayoutDragging(HWND window,int id,const LayoutContext& layout)noexcept{
    return layout.main&&layout.dragging&&GetCapture()==layout.main&&
        GetParent(window)==layout.main&&GetDlgItem(layout.main,id)==window&&layout.dragging(layout.user);
}
} // namespace detail

// Test-only thread-local override. It neither modifies the process imports nor
// changes the original class procedure. The table must outlive this scope.
class ScopedFunctionsForTesting {
    const GdiFunctions* previous_;
public:
    explicit ScopedFunctionsForTesting(const GdiFunctions& value)noexcept:previous_(detail::testFunctions){detail::testFunctions=&value;}
    ~ScopedFunctionsForTesting(){detail::testFunctions=previous_;}
    ScopedFunctionsForTesting(const ScopedFunctionsForTesting&)=delete;
    ScopedFunctionsForTesting& operator=(const ScopedFunctionsForTesting&)=delete;
};

// Other messages, visible keyboard cues and foreign supplied HDCs keep
// their exact arguments and original return value. The buffer commits one
// admitted native paint together; it does not make the whole layout or DWM
// composition atomic, nor suppress a separately dispatched native paint.
template<class Next>
inline LRESULT ForwardTo(Next next,HWND window,UINT message,WPARAM wp,LPARAM lp,const LayoutContext& layout={})noexcept{
    const auto& api=detail::Functions();
    const auto direct=[&]()noexcept{
        if(message==WM_PAINT&&detail::active&&detail::active->window==window&&detail::active->destroyed)return LRESULT(0);
        return next(window,message,wp,lp);
    };
    if(message==WM_NCDESTROY){
        if(detail::active&&detail::active->window==window)detail::active->destroyed=true;
        if(api.getProp(window,detail::kDirectOnce)==detail::DirectMarker()&&
            api.removeProp(window,detail::kDirectOnce)!=detail::DirectMarker())detail::permanentlyDisabled=true;
        return direct();
    }
    if(auto* paint=detail::active){
        // BeginPaint can synchronously request erasure. Redirect only this
        // target's actual display DC during that call; never swallow external
        // erases or redirect erases from the later native paint itself.
        const auto dc=reinterpret_cast<HDC>(wp);
        if(message==WM_ERASEBKGND&&paint->beginning&&!paint->redirectingErase&&!paint->destroyed&&window==paint->window&&
            dc&&GetObjectType(dc)==OBJ_DC&&WindowFromDC(dc)==window){
            paint->redirectingErase=true;
            const auto result=next(window,message,reinterpret_cast<WPARAM>(paint->memory),lp);
            paint->redirectingErase=false;return result;
        }
        return direct(); // Reentry never starts another buffer transaction.
    }
    if(message!=WM_PAINT||wp)return direct();
    const auto retry=api.getProp(window,detail::kDirectOnce);
    if(retry){
        if(retry!=detail::DirectMarker()||api.removeProp(window,detail::kDirectOnce)!=detail::DirectMarker())
            detail::permanentlyDisabled=true;
        return direct();
    }
    if(detail::permanentlyDisabled)return direct();
    const int id=GetDlgCtrlID(window);
    // Lock this transaction to its original admission mode. A later capture
    // transfer cannot turn a retired layout paint into a held-light paint.
    const bool layoutMode=detail::LayoutDragging(window,id,layout);
    if((!layoutMode&&(id<455||id>460||GetCapture()!=window))||
        !(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))return direct();
    RECT client{};
    if(!GetClientRect(window,&client)||client.left||client.top)return direct();
    if(layoutMode){
        // Divider widths are dynamic; the vertical slider uses its actual
        // client width/height without rotating or replacing native drawing.
        // Bound allocation before creating any GDI resource (at most 4 MiB).
        if(client.right<=0||client.bottom<=0||client.right>4096||client.bottom>256||
            std::uint64_t(client.right)*std::uint64_t(client.bottom)>(std::uint64_t(1)<<20))return direct();
    }else{
        // Preserve the separately checked held-light route's exact sizes.
        if(!CompactLightClient(id,client))return direct();
    }
    HWND parent=nullptr;RECT position{};
    if(layoutMode){parent=GetParent(window);if(!GetWindowRect(window,&position))return direct();}
    const auto sameAdmission=[&]()noexcept{
        if(!layoutMode)return GetCapture()==window;
        RECT now{};
        return detail::LayoutDragging(window,id,layout)&&GetParent(window)==parent&&
            GetWindowRect(window,&now)&&EqualRect(&now,&position);
    };
    const auto windowProcedure=GetWindowLongPtrA(window,GWLP_WNDPROC);
    if(!windowProcedure)return direct();
    // Guard preparation as well as painting. GDI/test callbacks and resource
    // cleanup may synchronously reenter this window before a buffer is ready.
    // Declare the scope first so it also remains active during GDI cleanup.
    detail::ActivePaint painting{window,nullptr,false,false};detail::ActiveScope scope(painting);
    detail::Resources resources(api,window);
    resources.original=api.createRectRgn(0,0,0,0);
    if(!resources.original||!detail::NonemptyUpdate(window,resources.original,api))return direct();
    resources.full=api.createRectRgn(0,0,client.right,client.bottom);
    if(!resources.full)return direct();
    resources.reference=api.getDC(window);if(!resources.reference)return direct();
    resources.memory=api.createCompatibleDC(resources.reference);if(!resources.memory)return direct();
    resources.bitmap=api.createCompatibleBitmap(resources.reference,client.right,client.bottom);if(!resources.bitmap)return direct();
    resources.previous=api.selectObject(resources.memory,resources.bitmap);
    if(!resources.previous||resources.previous==HGDI_ERROR)return direct();
    api.releaseDC(window,resources.reference);resources.reference=nullptr;
    // Preparation has not validated/erased or enlarged the original damage.
    // QUERYUISTATE can itself enter an outer window procedure. Perform it
    // before the final geometry/capture proof, leaving no synchronous query
    // after that proof which could move the target or retire its gesture.
    const bool preparedHidden=!painting.destroyed&&IsWindow(window)&&
        (SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)!=0;
    RECT current{};
    if(!preparedHidden||painting.destroyed||!IsWindow(window)||GetWindowLongPtrA(window,GWLP_WNDPROC)!=windowProcedure||
        !GetClientRect(window,&current)||!EqualRect(&current,&client)||!sameAdmission()||
        !detail::NonemptyUpdate(window,resources.original,api))return direct();
    // A complete tiny control is painted and committed together. Preparing
    // resources may fail without changing the original partial update region.
    if(!api.invalidateRgn(window,resources.full,FALSE))return direct();
    painting.memory=resources.memory;painting.beginning=true;
    PAINTSTRUCT paint{};const HDC destination=api.beginPaint(window,&paint);painting.beginning=false;
    if(painting.destroyed||!IsWindow(window))return 0;
    if(!destination){
        if(!painting.destroyed&&IsWindow(window)){
            api.endPaint(window,&paint);
            if(!painting.destroyed)detail::RetryDirect(window,resources.full,api);
        }
        if(painting.destroyed)return 0; // Never address a recycled HWND.
        return direct(); // Native WM_PAINT has not run yet; call it once.
    }
    const auto result=next(window,WM_PAINT,reinterpret_cast<WPARAM>(resources.memory),lp);
    // Native customdraw/parent callbacks can resize, move, reparent, replace or
    // destroy the window synchronously, or retire the divider gesture while
    // main still has capture. Do not commit an old surface afterwards.
    const bool paintedHidden=!painting.destroyed&&IsWindow(window)&&
        (SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)!=0;
    RECT after{};
    const bool sameWindow=paintedHidden&&!painting.destroyed&&IsWindow(window)&&GetWindowLongPtrA(window,GWLP_WNDPROC)==windowProcedure&&
        GetClientRect(window,&after)&&EqualRect(&after,&client)&&sameAdmission()&&
        !painting.destroyed;
    const bool copied=sameWindow&&api.bitBlt(destination,0,0,client.right,client.bottom,resources.memory,0,0,SRCCOPY)!=FALSE;
    // Destruction releases the window's paint DC. An HWND value reused during
    // the callback must never receive the old EndPaint or retry invalidation.
    const bool ended=!painting.destroyed&&IsWindow(window)&&api.endPaint(window,&paint)!=FALSE;
    // Never replay a native paint already run against the memory DC. Retry on
    // the next ordinary paint, preserving the full region enlarged above.
    if((!copied||!ended)&&!painting.destroyed)detail::RetryDirect(window,sameWindow?resources.full:nullptr,api);
    return result;
}

inline LRESULT Forward(WNDPROC native,HWND window,UINT message,WPARAM wp,LPARAM lp,const LayoutContext& layout={})noexcept{
    return ForwardTo([native](HWND child, UINT msg, WPARAM w, LPARAM l) {
        return CallWindowProcA(native,child,msg,w,l);
    },window,message,wp,lp,layout);
}
} // namespace mikumikustudio::ui::light_slider_paint_buffer
#endif
