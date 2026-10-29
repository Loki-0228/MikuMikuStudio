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
    Check(Title(GetDlgItem(dialog,as::Explanation)).find(L".emm")!=std::wstring::npos &&
        Title(GetDlgItem(dialog,as::Explanation)).find(L'\n')==std::wstring::npos,
        "footer contains only the one-line actual EMM save status");
    Check((IsDlgButtonChecked(dialog,as::NumberedManual)==BST_CHECKED)==context.value.numberedManualVersions,
        "independent manual history checkbox reflects the saved setting");
    for(int id:{as::Explanation,as::NumberedManual}) {
        HWND control=GetDlgItem(dialog,id);RECT available{};GetClientRect(control,&available);
        HDC dc=GetDC(control);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
        const auto text=Title(control);SIZE measured{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&measured);
        SelectObject(dc,old);ReleaseDC(control,dc);
        Check(measured.cx+(id==as::NumberedManual?24:0)<=available.right,"new footer labels fit in every language");
    }
    RECT bounds{};GetClientRect(dialog,&bounds);
    RECT units{0,0,300,183};MapDialogRect(dialog,&units);
    Check(bounds.right<=units.right && bounds.bottom<=units.bottom,"backup settings use the compact dialog size");
}
void BackupDirectoryChecks(MMDApp* app,const fs::path& config) {
    const auto before=as::Current();std::wstring error;
    const auto executable=local_data::ExecutableDirectory();
    Check(as::DisplayDirectory((executable/L"backups").wstring())==L"backups",
        "default backup folder is displayed relatively");
    Check(before.directory==(executable/L"backups").wstring(),
        "fresh installation defaults to backups beside data");
    const auto legacy=config.parent_path()/L"legacy-default.ini";
    const auto oldDirectory=executable/L"data"/L"backups";
    fs::create_directories(oldDirectory);
    const auto retained=oldDirectory/L"existing-backup.pmm";
    std::ofstream(retained)<<"retain existing backup";
    Check(WritePrivateProfileStringW(L"autosave",L"directoryMode",L"executable",legacy.c_str()) &&
        WritePrivateProfileStringW(L"autosave",L"directory",L"data/backups",legacy.c_str()),
        "seed the previous version's default folder setting");
    Check(as::LoadSettings(legacy).directory==before.directory,
        "previous default setting follows the new EXE-level backups folder");
    Check(Bytes(retained)=="retain existing backup","changing the default preserves existing backup files");
    Check(WritePrivateProfileStringW(L"autosave",L"directoryMode",L"custom",legacy.c_str()) &&
        WritePrivateProfileStringW(L"autosave",L"directory",oldDirectory.c_str(),legacy.c_str()),
        "seed an explicitly selected custom folder at the old location");
    Check(as::LoadSettings(legacy).directory==oldDirectory.wstring(),
        "explicit custom directories retain their selected location");
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

INT_PTR CALLBACK NamingProbe(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg!=WM_INITDIALOG)return as::NamingProc(dialog,msg,wp,lp);
    as::NamingProc(dialog,msg,wp,lp);
    auto* c=reinterpret_cast<as::NamingContext*>(lp);
    Check(Title(GetDlgItem(dialog,as::Preview)).find(L".pmm")!=std::wstring::npos,"naming preview includes automatic extension");
    const auto preview=GetDlgItem(dialog,as::Preview);
    Check((GetWindowLongW(preview,GWL_STYLE)&ES_READONLY)!=0,"naming preview is a read-only field");
    HDC previewDc=GetDC(dialog);
    Check(reinterpret_cast<HBRUSH>(SendMessageW(dialog,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(previewDc),
        reinterpret_cast<LPARAM>(preview)))==GetSysColorBrush(COLOR_3DFACE),"naming preview paints a grey read-only background");
    ReleaseDC(dialog,previewDc);
    SetDlgItemTextW(dialog,as::Pattern,L"start_end");
    SendDlgItemMessageW(dialog,as::Pattern,EM_SETSEL,5,5);
    as::NamingProc(dialog,WM_COMMAND,as::TokenFirst+3,0);
    Check(Title(GetDlgItem(dialog,as::Pattern))==L"start[/YYYY]_end","four-digit year button inserts at caret");
    SYSTEMTIME now{};GetLocalTime(&now);
    Check(Title(GetDlgItem(dialog,as::Preview))==L"start"+save_naming::Number(now.wYear,4)+L"_end.pmm","preview expands four-digit year");
    SetDlgItemTextW(dialog,as::Pattern,L"[/unknown]");
    Check(!IsWindowEnabled(GetDlgItem(dialog,IDOK)),"unknown token disables confirmation");
    SetDlgItemTextW(dialog,as::Pattern,L"[/NAME]_[/YY][/MM][/DD]_[/N]");
    CaptureBackupDialog(dialog,10+c->app->EnglishUI());
    as::NamingProc(dialog,WM_COMMAND,IDOK,0);
    Check(c->pattern==L"[/NAME]_[/YY][/MM][/DD]_[/N]","naming editor accepts ASCII tokens verbatim");
    return TRUE;
}
void NamingTests(MMDApp* app) {
    SYSTEMTIME time{};time.wYear=2026;time.wMonth=10;time.wDay=23;time.wHour=4;time.wMinute=5;time.wSecond=6;time.wMilliseconds=7;
    std::wstring result,error;
    Check(save_naming::Expand(L"[/NAME]_[/YYYY]_[/YY][/MM][/DD]_[/hh][/mm][/ss]_[/fff]_[/N]",L"工程",time,L"012",result,error) &&
        result==L"工程_2026_261023_040506_007_012","all ASCII tokens expand with correct padding and month/minute case");
    for(const auto* invalid:{L"",L"../bad",L"[/yy]",L"[/YYYY",L"CON",L"LPT1",L"bad.",L"bad ",L"bad\nline"})
        Check(!save_naming::Validate(invalid,error),"invalid filenames and tokens are rejected");
    for(int language:{0,1,2}) {
        app->EnglishUI()=language;as::NamingContext context{app,save_naming::IncrementDefault,L"工程",false};
        Check(UiDialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(as::kNamingDialog),app->MainWindow(),NamingProbe,
            reinterpret_cast<LPARAM>(&context))==IDOK,"naming dialog works in every UI language");
    }
    app->EnglishUI()=2;
}
