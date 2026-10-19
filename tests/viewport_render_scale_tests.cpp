// Hidden Win32 controls and real D3D9 render targets; never activates a window.
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <CommCtrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>

using namespace mikumikustudio;
namespace scale = mikumikustudio::viewport_render_scale;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool good, const char* why) {
    ++checks;
    if (!good) { std::printf("FAIL %s\n", why); std::exit(1); }
}
std::wstring Text(HWND dialog, int id) {
    wchar_t text[512]{}; GetDlgItemTextW(dialog, id, text, 512); return text;
}
void CaptureDialog(HWND dialog, int language, const char* state) {
    RECT client{}; GetClientRect(dialog,&client);
    RECT window{}; GetWindowRect(dialog,&window);
    const int width=window.right-window.left,height=window.bottom-window.top;
    HDC screen=GetDC(dialog), dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    HGDIOBJ previous=SelectObject(dc,bitmap);
    SendMessageW(dialog,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_NONCLIENT|PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    for (HWND child=GetWindow(dialog,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT box{}; GetWindowRect(child,&box); MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&box),2);
        Check(box.left>=0 && box.top>=0 && box.right<=client.right && box.bottom<=client.bottom,"controls fit dialog");
        const int id=GetDlgCtrlID(child);
        if (id==scale::Warning || id==scale::Dimensions || id==scale::Automatic) {
            const auto text=Text(dialog,id);
            HGDIOBJ font=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
            RECT measure{0,0,box.right-box.left-(id==scale::Automatic?22:0),0};
            DrawTextW(dc,text.c_str(),int(text.size()),&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
            Check(measure.bottom<=box.bottom-box.top,"localized text fits without clipping");
            if(font)SelectObject(dc,font);
        }
    }
    SelectObject(dc,previous);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(std::size_t(width)*height*4);
    Check(GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0,"dialog paint readback");
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+DWORD(pixels.size());
    char path[200]{};sprintf_s(path,"build/tests/viewport-scale-ui-%d-%s.bmp",language,state);
    FILE* out=nullptr;fopen_s(&out,path,"wb");Check(out!=nullptr,"save dialog capture");
    fwrite(&file,sizeof(file),1,out);fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,out);fwrite(pixels.data(),pixels.size(),1,out);fclose(out);
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(dialog,screen);
}
void UiTests(MMDApp* app) {
    scale::Size size;
    double number = 0;
    for (const auto* text : {L"0", L"-1", L"nan", L"inf", L"1e999", L"50junk", L"", L"%"})
        Check(!scale::ParsePercent(text, number), "invalid number rejected");
    Check(scale::ParsePercent(L" 12.5% ", number) && number == 12.5, "decimal below slider range accepted");
    Check(scale::ParsePercent(L"250", number) && number == 250, "manual supersampling accepted");
    Check(scale::CalculateSize(1920, 1080, 50, size) && size.width == 960 && size.height == 540, "half dimensions");
    Check(scale::CalculateSize(1920, 1080, 200, size) && !scale::Above4K(size), "exact 4K is not a warning");
    Check(scale::CalculateSize(1920, 1080, 201, size) && scale::Above4K(size), "above 4K warning boundary");
    Check(!scale::CalculateSize(1920, 1080, 1e300, size), "overflow rejected before conversion");
    Check(app->viewportRender.automatic && app->viewportRender.percent == 100, "new instance defaults");
    for (double percent : {100., 150., 75., 50., 25., 12.5, 1.}) {
        app->viewportRender = {false, percent};
        const unsigned expected = percent >= 75 ? 4096 : percent == 50 ? 2048 : percent == 25 ? 1024 : 512;
        Check(scale::ShadowSize(app,4096)==expected,"preview shadow budget retains power-of-two tiers and minimum");
        Check(scale::ShadowSize(app,512)==512,"small shadow map never shrinks below supported minimum");
    }
    app->viewportRender = {true,25};
    Check(scale::ShadowSize(app,4096)==4096,"automatic preview keeps shadow quality");
    app->viewportRender = {false,25};app->RecordingWindow()=app->MainWindow();
    Check(scale::ShadowSize(app,4096)==4096,"recording ignores preview shadow budget");
    app->RecordingWindow()=nullptr;app->viewportRender={};
    for (int lang = 0; lang != 3; ++lang) {
        app->EnglishUI() = static_cast<unsigned char>(lang);
        scale::RefreshMenu(app); scale::RefreshMenu(app);
        HMENU bar = GetMenu(app->MainWindow());
        bool adjacent = false;
        unsigned count = 0;
        for (int i = 0; i < GetMenuItemCount(bar); ++i) {
            HMENU view = GetSubMenu(bar, i);
            for (int j = 0; j < GetMenuItemCount(view); ++j) {
                if (GetMenuItemID(view, j) == scale::kCommand) {
                    ++count; adjacent = j > 0 && GetMenuItemID(view, j - 1) == 212;
                }
            }
        }
        Check(adjacent && count == 1, "menu immediately below screen size without duplicates");
        app->viewportRender = {};
        HWND dialog = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(scale::kDialog),
            app->MainWindow(), scale::DialogProc, reinterpret_cast<LPARAM>(app));
        Check(dialog != nullptr && !IsWindowVisible(dialog), "shipped dialog created hidden");
        Check(IsDlgButtonChecked(dialog, scale::Automatic) == BST_CHECKED &&
            !IsWindowEnabled(GetDlgItem(dialog, scale::Slider)) && Text(dialog, scale::Percent) == L"100", "auto disables slider at startup");
        Check(!(GetWindowLongPtrW(GetDlgItem(dialog, scale::Warning), GWL_STYLE) & WS_VISIBLE), "4K warning hidden at default size");
        CaptureDialog(dialog,lang,"auto");
        Check(SendDlgItemMessageW(dialog, scale::Slider, TBM_GETRANGEMIN, 0, 0) == 25 &&
            SendDlgItemMessageW(dialog, scale::Slider, TBM_GETRANGEMAX, 0, 0) == 100, "slider bounds");
        CheckDlgButton(dialog, scale::Automatic, BST_UNCHECKED);
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(scale::Automatic, BN_CLICKED), 0);
        Check(!app->viewportRender.automatic && IsWindowEnabled(GetDlgItem(dialog, scale::Slider)), "uncheck enables manual mode");
        SendDlgItemMessageW(dialog, scale::Slider, TBM_SETPOS, TRUE, 50);
        SendMessageW(dialog, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(GetDlgItem(dialog, scale::Slider)));
        Check(app->viewportRender.percent == 100 && Text(dialog, scale::Percent) == L"50", "drag updates readout without committing");
        SendMessageW(dialog, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(GetDlgItem(dialog, scale::Slider)));
        SendMessageW(dialog, WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(GetDlgItem(dialog, scale::Slider)));
        Check(app->viewportRender.percent == 50, "release commits");
        SetDlgItemTextW(dialog, scale::Percent, L"12.5");
        SendMessageW(dialog, WM_COMMAND, scale::Apply, 0);
        Check(app->viewportRender.percent == 12.5, "manual value below slider minimum applied");
        SetDlgItemTextW(dialog, scale::Percent, L"200");
        Check(Text(dialog, scale::Warning).empty(), "exact 4K does not show warning");
        SetDlgItemTextW(dialog, scale::Percent, L"250");
        Check(Text(dialog, scale::Warning).find(L"4K") != std::wstring::npos, "4K warning visible before apply");
        Check(GetWindowLongPtrW(GetDlgItem(dialog, scale::Warning), GWL_STYLE) & WS_VISIBLE, "above 4K reveals warning control");
        RECT warning{}, apply{};
        GetWindowRect(GetDlgItem(dialog, scale::Warning), &warning);
        GetWindowRect(GetDlgItem(dialog, scale::Apply), &apply);
        Check(warning.top == apply.top && warning.right < apply.left, "4K warning shares button row without overlap");
        HDC warningDc = GetDC(GetDlgItem(dialog, scale::Warning));
        SendMessageW(dialog, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(warningDc), reinterpret_cast<LPARAM>(GetDlgItem(dialog, scale::Warning)));
        Check(GetTextColor(warningDc) == RGB(192, 0, 0), "4K warning is red");
        ReleaseDC(GetDlgItem(dialog, scale::Warning), warningDc);
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        Check(app->viewportRender.percent == 250 && !Text(dialog, scale::Warning).empty(), "Enter accepts above 4K with persistent warning");
        CaptureDialog(dialog,lang,"warning");
        SetDlgItemTextW(dialog, scale::Percent, L"-7");
        SendMessageW(dialog, WM_COMMAND, scale::Apply, 0);
        Check(app->viewportRender.percent == 250 && !Text(dialog, scale::Dimensions).empty(), "invalid input preserves applied setting and shows validation");
        CheckDlgButton(dialog, scale::Automatic, BST_CHECKED);
        SendMessageW(dialog, WM_COMMAND, scale::Automatic, 0);
        Check(app->viewportRender.automatic && app->viewportRender.percent == 250 &&
            !IsWindowEnabled(GetDlgItem(dialog, scale::Slider)), "auto restores original path and retains manual value");
        Check(!(GetWindowLongPtrW(GetDlgItem(dialog, scale::Warning), GWL_STYLE) & WS_VISIBLE), "returning to normal size hides warning");
        Check(app->RenderWidth() == 1920 && app->RenderHeight() == 1080, "preview never changes export dimensions");
        DestroyWindow(dialog);
    }
}
void GpuTests(MMDApp* app, D3DMULTISAMPLE_TYPE msaa) {
    auto r = std::make_unique<D3DRenderer>();
    ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    Check(d3d != nullptr, "D3D9 available");
    const D3DFORMAT format = D3DFMT_A8R8G8B8;
    if (msaa != D3DMULTISAMPLE_NONE && FAILED(d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, format, TRUE, msaa, nullptr))) {
        std::puts("SKIP unsupported MSAA mode"); return;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 360; pp.BackBufferFormat = format;
    pp.hDeviceWindow = app->MainWindow(); pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8; pp.MultiSampleType = msaa;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &r->device)), "hidden production-style device");
    auto* device = r->device;
    app->Renderer() = r.get(); r->viewScale = 1.25f;
    D3DCAPS9 caps{}; device->GetDeviceCaps(&caps);
    r->maxTextureWidth = int(caps.MaxTextureWidth); r->maxTextureHeight = int(caps.MaxTextureHeight);
    const RECT display{32, 24, 544, 312}; app->ViewportRect() = display;
    const D3DVIEWPORT9 vp{32, 24, 512, 288, 0, 1}; device->SetViewport(&vp);
    ComPtr<IDirect3DSurface9> original;
    device->GetRenderTarget(0, &original);
    struct Vertex { float x,y,z,rhw; DWORD color; float u,v; };
    const Vertex vertex{132, 74, 0, 1, 0xffffffff, 0, 0};
    Check(SUCCEEDED(device->CreateVertexBuffer(sizeof(Vertex)*3, 0,
        D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1, D3DPOOL_MANAGED, &app->OverlayVertices(), nullptr)), "real overlay buffer");
    app->TextOverlayPrimitiveCount() = 1;
    void* data = nullptr; app->OverlayVertices()->Lock(0,0,&data,0);
    for (int i=0;i<3;++i) static_cast<Vertex*>(data)[i] = vertex;
    app->OverlayVertices()->Unlock();
    Check(SUCCEEDED(device->CreateVertexBuffer(sizeof(Vertex)*6,0,
        D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1,D3DPOOL_MANAGED,
        &app->PictureOverlayVertices(),nullptr)),"background geometry for scale regression");
    app->PictureOverlayVertices()->Lock(0,0,&data,0);
    for(int i=0;i<6;++i)static_cast<Vertex*>(data)[i]=vertex;
    app->PictureOverlayVertices()->Unlock();
    for (double percent : {25., 50., 100., 12.5, 150.}) {
        std::wstring error;
        Check(scale::Configure(app, false, percent, error), "valid scale can allocate");
        const auto expected = scale::PreviewSize(app, false, percent);
        {
            scale::FrameScope scope(app);
            Check(scope.Ready(), "scaled frame ready");
            D3DVIEWPORT9 live{}; device->GetViewport(&live);
            Check(live.Width == expected.width && live.Height == expected.height, "GPU viewport uses scaled dimensions");
            ComPtr<IDirect3DSurface9> target; device->GetRenderTarget(0, &target);
            D3DSURFACE_DESC desc{}; target->GetDesc(&desc);
            if (percent != 100) {
                Check(target.Get() != original.Get() && desc.Width == expected.width && desc.Height == expected.height,
                    "actual offscreen render target resized");
                Check(desc.MultiSampleType == msaa, "anti-aliasing preserved");
                Check(r->backbufferSurface == target.Get(), "native shadow return target follows preview");
            } else Check(target.Get() == original.Get(), "100 percent matches original path");
            app->OverlayVertices()->Lock(0,0,&data,D3DLOCK_READONLY);
            Check(std::memcmp(data, &vertex, sizeof(vertex)) == 0,
                "HUD vertices stay in display pixels during scene scaling");
            app->OverlayVertices()->Unlock();
            app->PictureOverlayVertices()->Lock(0,0,&data,D3DLOCK_READONLY);
            Check(std::memcmp(data,&vertex,sizeof(vertex))==0,"disabled background is not rewritten during scaling");
            app->PictureOverlayVertices()->Unlock();
            Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff2468ac,1,0)), "render scaled color and depth");
            Check(scope.Complete(), "composite scene before drawing display HUD");
            Check(!scale::Rendering(app) && EqualRect(&app->ViewportRect(), &display),
                "explicit completion restores display coordinates before scope destruction");
            Check(scope.Complete(), "completion is idempotent");
        }
        Check(EqualRect(&app->ViewportRect(), &display) && r->viewScale == 1.25f, "restore mouse and gizmo coordinate space");
        Check(!r->backbufferSurface && !r->depthStencilSurface, "restore renderer resource slots");
        D3DVIEWPORT9 restored{}; device->GetViewport(&restored);
        Check(std::memcmp(&restored, &vp, sizeof(vp)) == 0, "restore device viewport");
        app->OverlayVertices()->Lock(0,0,&data,D3DLOCK_READONLY);
        Check(std::memcmp(data, &vertex, sizeof(vertex)) == 0, "overlay positions restored exactly");
        app->OverlayVertices()->Unlock();
        ComPtr<IDirect3DSurface9> resolved, cpu;
        Check(SUCCEEDED(device->CreateRenderTarget(640,360,format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)), "readback resolve surface");
        Check(SUCCEEDED(device->StretchRect(original.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE)), "resolve final preview");
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(640,360,format,D3DPOOL_SYSTEMMEM,&cpu,nullptr)), "readback surface");
        Check(SUCCEEDED(device->GetRenderTargetData(resolved.Get(),cpu.Get())), "read displayed render target");
        D3DLOCKED_RECT locked{}; cpu->LockRect(&locked,nullptr,D3DLOCK_READONLY);
        const DWORD center = *reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits)+180*locked.Pitch+280*4);
        cpu->UnlockRect();
        Check((center&0xffffff) == 0x2468ac, "scaled pixels reach the original display area");
    }
    std::wstring error;
    Check(SUCCEEDED(device->CreateTexture(1,1,1,0,format,D3DPOOL_MANAGED,
        &app->PictureBackgroundTexture(),nullptr)),"enabled background texture");
    app->PictureBackgroundEnabled()=1;
    Check(scale::Configure(app,false,25,error),"scale enabled background");
    {
        scale::FrameScope scope(app);Check(scope.Ready(),"background scaling is ready");
        app->PictureOverlayVertices()->Lock(0,0,&data,D3DLOCK_READONLY);
        const auto& scaled=*static_cast<Vertex*>(data);
        Check(scaled.x==(vertex.x-display.left+.5f)*.25f-.5f &&
              scaled.y==(vertex.y-display.top+.5f)*.25f-.5f,"enabled background follows scene pixels");
        app->PictureOverlayVertices()->Unlock();
        // Destructor must also restore geometry when a scene exits early.
    }
    app->PictureOverlayVertices()->Lock(0,0,&data,D3DLOCK_READONLY);
    Check(std::memcmp(data,&vertex,sizeof(vertex))==0,"early frame exit restores background coordinates");
    app->PictureOverlayVertices()->Unlock();
    app->PictureBackgroundEnabled()=0;
    app->PictureBackgroundTexture()->Release();app->PictureBackgroundTexture()=nullptr;
    app->PictureOverlayVertices()->Release();app->PictureOverlayVertices()=nullptr;
    Check(scale::Configure(app,true,50,error), "restore auto");
    { scale::FrameScope scope(app); ComPtr<IDirect3DSurface9> target; device->GetRenderTarget(0,&target);
      Check(scope.Ready() && target.Get()==original.Get(), "auto has no offscreen override"); }
    app->RecordingWindow() = app->MainWindow(); app->viewportRender = {false,50};
    { scale::FrameScope scope(app); Check(!scale::Rendering(app), "export bypasses preview scaling"); }
    app->RecordingWindow() = nullptr;
    scale::Release(app); original.Reset();
    Check(SUCCEEDED(device->Reset(&pp)), "preview resources do not block device reset");
    app->OverlayVertices()->Release(); app->OverlayVertices()=nullptr; app->TextOverlayPrimitiveCount()=0;
    device->Release(); app->Renderer()=nullptr;
}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    auto app=std::make_unique<MMDApp>(); app->state={}; g_Block=app.get();
    app->MainWindow()=CreateWindowW(L"STATIC",L"Viewport scale tests",WS_POPUP,0,0,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(app->MainWindow()!=nullptr,"hidden window");
    HMENU menu=LoadMenuW(GetModuleHandleW(nullptr),L"SAMPLE02E"); SetMenu(app->MainWindow(),menu);
    app->RenderWidth()=1920; app->RenderHeight()=1080; app->ViewportRect()={0,0,1920,1080};
    UiTests(app.get());
    GpuTests(app.get(),D3DMULTISAMPLE_NONE);
    GpuTests(app.get(),D3DMULTISAMPLE_4_SAMPLES);
    DestroyWindow(app->MainWindow()); g_Block=nullptr;
    std::printf("PASS viewport render scale: %u checks\n",checks);
    return 0;
}
