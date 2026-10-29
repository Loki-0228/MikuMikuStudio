#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>
#include <set>
#include <cwctype>
#include "pmm_extension.hpp"

namespace mikumikustudio::save_naming {
inline constexpr wchar_t BackupDefault[] = L"[/NAME]_autosave_[/YYYY][/MM][/DD]_[/hh][/mm][/ss]";
inline constexpr wchar_t PreviousBackupDefault[] = L"[/NAME]_autosave_[/YYYY][/MM][/DD]_[/hh][/mm][/ss]_[/fff]";
inline constexpr wchar_t IncrementDefault[] = L"[/NAME]_[/N]";
inline constexpr char BaseKey[] = "org.mikudancestudio.save.naming.base";
inline constexpr char FileKey[] = "org.mikudancestudio.save.naming.file";
inline std::wstring Number(unsigned value, unsigned width) {
    auto result=std::to_wstring(value);
    if(result.size()<width)result.insert(0,width-result.size(),L'0');
    return result;
}
inline bool Expand(const std::wstring& pattern,const std::wstring& name,const SYSTEMTIME& time,
                   const std::wstring& sequence,std::wstring& result,std::wstring& error) {
    result.clear();error.clear();
    for(std::size_t i=0;i<pattern.size();) {
        if(pattern.compare(i,2,L"[/")!=0){result+=pattern[i++];continue;}
        const auto end=pattern.find(L']',i+2);
        const auto token=pattern.substr(i,end==std::wstring::npos?end:end-i+1);
        if(token==L"[/NAME]")result+=name;
        else if(token==L"[/N]")result+=sequence;
        else if(token==L"[/YYYY]")result+=Number(time.wYear,4);
        else if(token==L"[/YY]")result+=Number(time.wYear%100,2);
        else if(token==L"[/MM]")result+=Number(time.wMonth,2);
        else if(token==L"[/DD]")result+=Number(time.wDay,2);
        else if(token==L"[/hh]")result+=Number(time.wHour,2);
        else if(token==L"[/mm]")result+=Number(time.wMinute,2);
        else if(token==L"[/ss]")result+=Number(time.wSecond,2);
        else if(token==L"[/fff]")result+=Number(time.wMilliseconds,3);
        else {error=L"未知占位符 / Unknown token: "+token;return false;}
        i=end+1;
    }
    const auto invalid=[](wchar_t ch){return ch<32 || wcschr(L"<>:\"/\\|?*",ch);};
    if(result.empty() || result.size()>240 || result.back()==L'.' || result.back()==L' '){
        error=L"名称不能为空、过长，或以空格和句点结尾。 / Invalid filename length or ending.";return false;
    }
    for(auto ch:result)if(invalid(ch)){error=L"文件名不能包含 < > : \" / \\ | ? *。 / Invalid filename character.";return false;}
    auto base=result.substr(0,result.find(L'.'));
    for(auto& ch:base)ch=towupper(ch);
    if(base==L"CON" || base==L"PRN" || base==L"AUX" || base==L"NUL" ||
       (base.size()==4 && (base.substr(0,3)==L"COM" || base.substr(0,3)==L"LPT") &&
        (base[3]>=L'1' && base[3]<=L'9'))) {
        error=L"此名称由系统保留。 / This filename is reserved.";return false;
    }
    return true;
}
inline bool Validate(const std::wstring& pattern,std::wstring& error) {
    SYSTEMTIME time{};GetLocalTime(&time);std::wstring expanded;
    if(pattern.size()>240){error=L"命名规则过长。 / Naming pattern is too long.";return false;}
    return Expand(pattern,L"Project",time,L"001",expanded,error);
}
inline std::wstring ProjectStem(const std::filesystem::path& path,const pmm_extension::Document& document) {
    const auto* file=pmm_extension::Find(document,FileKey);
    const auto* base=pmm_extension::Find(document,BaseKey);
    if(file && base && std::string(file->value.begin(),file->value.end())==path.filename().u8string()) {
        try {
            auto stem=std::filesystem::u8path(std::string(base->value.begin(),base->value.end())).wstring();
            SYSTEMTIME time{};std::wstring result,error;
            if(Expand(L"[/NAME]",stem,time,L"001",result,error))return stem;
        } catch(...) {}
    }
    return path.stem().wstring();
}
inline void Increment(std::wstring& number) {
    for(auto i=number.size();i>0;--i){if(number[i-1]!=L'9'){++number[i-1];return;}number[i-1]=L'0';}
    number.insert(number.begin(),L'1');
}
// PMM and EMM both reserve a name. Existing files are never moved or replaced.
inline std::filesystem::path Next(const std::filesystem::path& directory,const std::wstring& pattern,
                                  const std::wstring& name,bool crash=false) {
    SYSTEMTIME time{};GetLocalTime(&time);
    std::set<std::wstring> occupied;
    std::error_code ec;
    for(std::filesystem::directory_iterator it(directory,ec),end;!ec && it!=end;it.increment(ec)) {
        auto filename=it->path().filename().wstring();for(auto& ch:filename)ch=towlower(ch);
        occupied.insert(std::move(filename));
    }
    if(ec && ec!=std::errc::no_such_file_or_directory)return {};
    const bool numbered=pattern.find(L"[/N]")!=std::wstring::npos;
    std::wstring sequence=numbered?L"001":L"000";
    for(bool first=true;;first=false,Increment(sequence)) {
        std::wstring stem,error;
        if(!Expand(pattern,name,time,sequence,stem,error))return {};
        if(crash)stem+=L"_crash";
        if(!numbered && !first)stem+=L"_"+sequence;
        const auto path=directory/(stem+L".pmm");
        if(path.wstring().size()>=250)return {};
        auto key=stem;for(auto& ch:key)ch=towlower(ch);
        if(!occupied.count(key+L".pmm") && !occupied.count(key+L".emm"))return path;
    }
}
}
