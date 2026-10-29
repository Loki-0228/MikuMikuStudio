#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace mikumikustudio { class MMDApp; namespace mdl {struct ModelRecord;}
namespace save_compatibility {
struct Report {
    bool needsNewPmm=false;
    bool invalidData=false;
    bool namesNeedExtension=false;
    bool canCopyLegacy=true;
    std::wstring reason;
    std::wstring nameIssue;
    std::vector<std::wstring> details;
    unsigned omittedDetails=0;
    unsigned language=1;
};
Report InspectPmm(MMDApp&);
Report InspectMotion(const mdl::ModelRecord&);
bool CanSavePmd(const mdl::ModelRecord&);
// The complete source has already been flushed. Never replace an existing copy.
bool CopyPrefix(const wchar_t* source,std::uint64_t bytes,std::wstring& destination);
std::wstring Describe(const Report&,const std::wstring& copy,bool attempted);
void Notify(MMDApp&,const Report&,const std::wstring& copy,bool attempted);
}}
