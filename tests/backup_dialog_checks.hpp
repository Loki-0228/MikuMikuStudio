void CaptureBackupDialog(HWND dialog, int language) {
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right-rect.left, height=rect.bottom-rect.top;
    HDC dc=GetWindowDC(dialog), memory=CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr; auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    const auto old=SelectObject(memory,bitmap);
    SendMessageW(dialog,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+width*height*4;
    std::ofstream stream(root/(L"backup-dialog-"+std::to_wstring(language)+L".bmp"),std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels),width*height*4);
    Check(bool(stream),"capture compact backup dialog without displaying it");
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(dialog,dc);
}
void BackupDialogChecks(HWND dialog,const as::DialogContext& context) {
    Check(Title(GetDlgItem(dialog,as::KeepPrevious))==as::Text(context.app,L"同时保存 .pmm1",L"Also keep .pmm1",L".pmm1 も保存する"),
        "previous-version checkbox has a short extension-based caption");
    Check(Title(GetDlgItem(dialog,as::Folder))==as::DisplayDirectory(context.value.directory),
        "backup folder displays an EXE-relative path");
    Check(context.tooltip && SendMessageW(context.tooltip,TTM_GETTOOLCOUNT,0,0)==2,
        "version checkbox and folder both have registered hover tools");
    wchar_t hint[1024]{};
    TOOLINFOW tool{sizeof(tool)};tool.hwnd=dialog;
    tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(dialog,as::KeepPrevious));tool.lpszText=hint;
    SendMessageW(context.tooltip,TTM_GETTEXTW,1024,reinterpret_cast<LPARAM>(&tool));
    Check(wcsstr(hint,L".pmm1") && wcsstr(hint,L"MME") && wcsstr(hint,L".emm"),
        "hover text explains old versions and MME's conditional effect saving in all languages");
    RECT bounds{};GetClientRect(dialog,&bounds);
    RECT units{0,0,300,195};MapDialogRect(dialog,&units);
    Check(bounds.right<=units.right && bounds.bottom<=units.bottom,"backup settings use the compact dialog size");
}
void BackupDirectoryChecks(MMDApp* app,const fs::path& config) {
    const auto before=as::Current();std::wstring error;
    const auto executable=local_data::ExecutableDirectory();
    Check(as::DisplayDirectory((executable/L"data"/L"backups").wstring())==L"data\\backups",
        "default backup folder is displayed relatively");
    auto custom=before;custom.directory=(executable/L"custom"/L"中文备份").wstring();
    Check(as::Configure(app,custom,error),"configure an EXE-contained custom backup folder");
    wchar_t stored[512]{};GetPrivateProfileStringW(L"autosave",L"directory",L"",stored,512,config.c_str());
    Check(std::wstring(stored)==L"custom\\中文备份","EXE-contained folder is stored relatively");
    const auto elsewhere=root/L"other-cwd";fs::create_directories(elsewhere);
    Check(SetCurrentDirectoryW(elsewhere.c_str())!=FALSE,"change working directory like a file picker");
    Check(as::LoadSettings(config).directory==custom.directory,"relative settings resolve against the EXE, not working directory");
    Check(SetCurrentDirectoryW(executable.c_str())!=FALSE,"restore test working directory");
    const auto sibling=executable.parent_path()/(executable.filename().wstring()+L"-other")/L"backups";
    Check(as::DisplayDirectory(sibling.wstring())==sibling.wstring(),"a sibling with the same prefix retains its absolute path");
    Check(as::Configure(app,before,error),"restore backup settings after relative-path checks");
}
