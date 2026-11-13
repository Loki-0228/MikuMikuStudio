// Opt-in, single-purpose process. Capture bytes from the editor's actual
// SRCCOPY in a nonempty BeginPaint, never ask the window to render a copy.
namespace expression_paint {
struct Frame {
    RECT dirty{};
    int width = 0, height = 0;
    int paints = 0;
    std::vector<unsigned> pixels;
    std::wstring intervalReadout;
    bool intervalEnabled = false;
};
Frame canvas, mmd;
int shellPaints;
HWND owner;
HDC paintDc;
Frame* painting;
decltype(&BeginPaint) begin;
decltype(&EndPaint) end;
decltype(&BitBlt) blit;

void Require(bool ok, const char* what) {
    Check(ok, what);
    if (!ok) std::exit(1);
}
HDC WINAPI Begin(HWND w, LPPAINTSTRUCT ps) {
    HDC dc = begin(w, ps);
    if (owner && GetWindow(w,GW_OWNER)==owner && IsWindowVisible(w) && !IsRectEmpty(&ps->rcPaint))
        ++shellPaints;
    const int id = GetDlgCtrlID(w);
    if (owner && (id == 1020 || id == 1031) && GetWindow(GetParent(w), GW_OWNER) == owner &&
        IsWindowVisible(w) && !IsRectEmpty(&ps->rcPaint)) {
        Require(!painting, "first-paint observer does not overwrite a nested paint context");
        painting = id == 1020 ? &canvas : &mmd;
        ++painting->paints;
        if (painting->paints == 1) {
            painting->dirty = ps->rcPaint;
            if (id == 1031) {
                wchar_t text[256]{};
                GetDlgItemTextW(GetParent(w),1032,text,256);
                painting->intervalReadout = text;
                painting->intervalEnabled = IsWindowEnabled(w) != FALSE;
            }
        }
        paintDc = dc;
    }
    return dc;
}
BOOL WINAPI End(HWND w, const PAINTSTRUCT* ps) {
    if (ps->hdc == paintDc) { painting = nullptr; paintDc = nullptr; }
    return end(w, ps);
}
BOOL WINAPI Blit(HDC dst, int x, int y, int width, int height, HDC src, int sx, int sy, DWORD op) {
    const BOOL result = blit(dst, x, y, width, height, src, sx, sy, op);
    if (dst != paintDc || !painting || painting->paints != 1 || !painting->pixels.empty()) return result;
    const RECT r = {x, y, x + width, y + height};
    Require(result && op == SRCCOPY && EqualRect(&r, &painting->dirty) && width > 0 && height > 0,
            "first expression WM_PAINT presents its actual dirty rectangle");
    Frame* const captured = painting;
    static_assert(sizeof(unsigned) == 4, "32-bit DIB pixels require four-byte storage");
    Require(size_t(height) <= captured->pixels.max_size()/size_t(width), "capture dimensions fit the pixel vector");
    const size_t count = size_t(width)*size_t(height);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC copy = CreateCompatibleDC(src);
    HBITMAP bitmap = CreateDIBSection(src, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(copy && bitmap && bits, "allocate first-frame pixel capture");
    DIBSECTION section{};
    Require(GetObjectW(bitmap,sizeof(section),&section)==sizeof(section) &&
            section.dsBm.bmBits==bits && section.dsBm.bmWidth==width && section.dsBm.bmHeight==height &&
            section.dsBm.bmBitsPixel==32 && section.dsBm.bmPlanes==1 &&
            size_t(section.dsBm.bmWidthBytes)==size_t(width)*sizeof(unsigned),
            "capture DIB dimensions stride and pixel type match the owned buffer");
    const auto previous = SelectObject(copy, bitmap);
    Require(previous && previous != HGDI_ERROR && blit(copy, 0, 0, width, height, src, sx, sy, SRCCOPY) &&
            GdiFlush(), "copy pixels from the buffer submitted by this WM_PAINT");
    const auto* data = static_cast<const unsigned*>(bits);
    Require(painting==captured && paintDc==dst, "capture keeps its original paint context through the GDI copy");
    captured->pixels.assign(data, data + count);
    captured->width=width; captured->height=height;
    std::printf("EXPRESSION_CAPTURE surface=%s width=%d height=%d count=%zu pixel_bytes=%zu stride=%ld dirty=%ld,%ld,%ld,%ld\n",
                captured==&canvas?"canvas":"mmd",width,height,captured->pixels.size(),sizeof(unsigned),
                section.dsBm.bmWidthBytes,r.left,r.top,r.right,r.bottom);
    SelectObject(copy, previous); DeleteObject(bitmap); DeleteDC(copy);
    return result;
}
template<class F> void Hook(F api, F detour, F& original) {
    auto* address = reinterpret_cast<void*>(api);
    Require(MH_CreateHook(address, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) == MH_OK &&
            MH_EnableHook(address) == MH_OK, "observe GDI presentation in the isolated test process");
}
// All supplied theme colours are grey. The one active channel is the only
// saturated stroke in these interior columns (away from handles/playhead).
unsigned Ink(const Frame& f, int x, int y, unsigned wanted = 0, int* measured = nullptr) {
    Require(f.width>0 && f.height>0 && f.width==f.dirty.right-f.dirty.left &&
            f.height==f.dirty.bottom-f.dirty.top &&
            size_t(f.height)<=f.pixels.max_size()/size_t(f.width) &&
            f.pixels.size()==size_t(f.width)*size_t(f.height), "saved first frame dimensions match its complete pixel buffer");
    Require(!f.pixels.empty() && x >= f.dirty.left && x < f.dirty.right &&
            y - 3 >= f.dirty.top && y + 3 < f.dirty.bottom, "sample lies inside the first presented rectangle");
    for (int row = y - 3; row <= y + 3; ++row) {
        const size_t index=size_t(row-f.dirty.top)*size_t(f.width)+size_t(x-f.dirty.left);
        Require(index<f.pixels.size(), "first-paint sample index stays inside the owned pixel buffer");
        const auto c = f.pixels.at(index) & 0xffffff;
        const unsigned r = c >> 16, g = (c >> 8) & 255, b = c & 255;
        const auto chroma = [](unsigned a,unsigned b,unsigned c) { return std::max({a,b,c})-std::min({a,b,c}); };
        const auto span = chroma(r,g,b);
        if (span <= 45) continue;
        if (!wanted) return c;
        // GDI+ antialiases the main curve against the grey paper. Compare
        // hue to the MMD pen, tolerating that blend at subpixel positions.
        const int wr = wanted >> 16, wg = (wanted >> 8)&255, wb = wanted&255;
        const double scale = double(span)/chroma(wr,wg,wb);
        if (std::abs(int(r)-int(g)-scale*(wr-wg))<10 && std::abs(int(b)-int(g)-scale*(wb-wg))<10) {
            if (measured) *measured=row;
            return c;
        }
    }
    return 0;
}
RECT Square(HWND panel) {
    RECT r{}; GetClientRect(panel, &r);
    using Dpi = UINT(WINAPI*)(HWND);
    const auto getDpi = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    HDC dc = GetDC(panel); const int dpi = getDpi ? int(getDpi(panel)) : GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(panel, dc);
    const int margin = MulDiv(6, dpi, 96), side = int(std::min(r.right,r.bottom))-2*margin;
    Require(side > 64, "MMD panel has a measurable square");
    return {margin,margin,margin+side,margin+side};
}
POINT OnSquare(const RECT& r, double u, double v) {
    return {r.left + LONG(std::lround(u*(r.right-r.left))), r.bottom - LONG(std::lround(v*(r.bottom-r.top)))};
}
unsigned Geometry(HWND editor, const ac::Track& authored, bool curved, unsigned ink, int middleFrame = 10) {
    CurveEditorAxisState axis{};
    Require(SendMessageW(editor, WM_APP+394, 0, reinterpret_cast<LPARAM>(&axis)) != 0 &&
            axis.viewLow == 0 && axis.viewHigh == 1 && axis.timeEnd > axis.timeStart &&
            !IsRectEmpty(&axis.plot), "OpenMorph starts at weight 0..1 with a measurable plot");
    // Record UI state captured inside BeginPaint, before querying the hook:
    // the hook recomputes controls and cannot prove what the first paint used.
    char readout[1024]{};
    WideCharToMultiByte(CP_UTF8,0,mmd.intervalReadout.c_str(),-1,readout,sizeof(readout),nullptr,nullptr);
    std::printf("EXPRESSION_FIRST_STATE curved=%d canvas_paints=%d mmd_paints=%d enabled=%d readout=%s\n",
                int(curved),canvas.paints,mmd.paints,int(mmd.intervalEnabled),readout);
    std::fflush(stdout);
    double controls[4]{};
    Require(mmd.intervalEnabled &&
            mmd.intervalReadout == L"\u533a\u95f4 0\u201320 \u5e27" &&
            SendMessageW(editor,WM_APP+302,0,reinterpret_cast<LPARAM>(controls))!=0,
            "a representable expression interval was displayed in the captured first MMD paint");
    std::puts("EXPRESSION_STAGE interval query complete; reading MMD square");
    const RECT square = Square(GetDlgItem(editor,1031));
    for (const int frame : {5, middleFrame, 14}) {
        std::printf("EXPRESSION_STAGE evaluate frame=%d keys=%zu\n",frame,authored.keys.size());
        Require(authored.keys.size()==2,"pixel reference still owns both authored expression keys");
        const double u = frame/20., value = ac::Evaluate(authored, frame).value;
        const double v = (value-authored.keys[0].value)/(authored.keys[1].value-authored.keys[0].value);
        const int x = int(std::lround(axis.plot.left+(frame-axis.timeStart)/(axis.timeEnd-axis.timeStart)*(axis.plot.right-axis.plot.left)));
        const double at = axis.timeStart+(x-axis.plot.left)*(axis.timeEnd-axis.timeStart)/(axis.plot.right-axis.plot.left);
        const double canvasValue=ac::Evaluate(authored,at).value;
        const double chord = authored.keys[0].value*(1-at/20.) + authored.keys[1].value*(at/20.);
        const auto y = [&](double a) { return int(std::lround(axis.plot.bottom-a*(axis.plot.bottom-axis.plot.top))); };
        const auto p = OnSquare(square,u,v), diagonal = OnSquare(square,u,u);
        std::printf("EXPRESSION_STAGE sample frame=%d canvas=%d,%d mmd=%ld,%ld\n",frame,x,y(canvasValue),p.x,p.y);
        if (!ink) ink = Ink(mmd,p.x,p.y);
        int cy=-1,my=-1;
        const auto miniInk=Ink(mmd,p.x,p.y,ink,&my),canvasInk=Ink(canvas,x,y(canvasValue),ink,&cy);
        std::printf("EXPRESSION_FIRST_PAINT curved=%d frame=%d canvas_x=%d measured=%d expected=%d chord=%d "
                    "mmd_x=%ld measured=%d expected=%ld chord=%ld ink=%06x\n",
                    int(curved),frame,x,cy,y(canvasValue),y(chord),p.x,my,p.y,diagonal.y,ink);
        Require(ink && miniInk && canvasInk,
                "both first painted surfaces contain the expression channel at its authored height");
        if (curved) Require(std::abs(p.y-diagonal.y)>7 && std::abs(y(canvasValue)-y(chord))>7 &&
                            !Ink(mmd,p.x,diagonal.y,ink) && !Ink(canvas,x,y(chord),ink),
                            "neither first paint substitutes a straight chord for the expression");
    }
    return ink;
}
} // namespace expression_paint
