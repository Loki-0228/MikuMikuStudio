#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/background_picture_source.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <thread>

namespace {
thread_local bool allocationsArmed=false, allocationFailed=false;
thread_local std::size_t allocationIndex=0, failAt=(std::numeric_limits<std::size_t>::max)();
}
void* operator new(std::size_t size) {
    if (allocationsArmed && allocationIndex++ == failAt) { allocationFailed=true; throw std::bad_alloc(); }
    if (auto* p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::media;
namespace {
unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(false)
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit File(HANDLE h):value(h){}
    ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    File(const File&)=delete;
};
struct Fixture {
    std::wstring path,directory;
    bool ownsDirectory=false;
    Fixture(){
        wchar_t temp[MAX_PATH]{},name[MAX_PATH]{};
        const auto n=GetTempPathW(MAX_PATH,temp);
        if(!n||n>=MAX_PATH||!GetTempFileNameW(temp,L"mbs",0,name))throw std::runtime_error("create isolated temporary file");
        path=name;directory=path+L".directory";
        ownsDirectory=CreateDirectoryW(directory.c_str(),nullptr)!=FALSE;
        if(!ownsDirectory){DeleteFileW(path.c_str());throw std::runtime_error("create isolated directory");}
    }
    ~Fixture(){DeleteFileW(path.c_str());if(ownsDirectory)RemoveDirectoryW(directory.c_str());}
    void Write(const std::vector<unsigned char>& bytes){
        File file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        CHECK(file.value!=INVALID_HANDLE_VALUE);if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("fixture writer");
        DWORD written=0;CHECK(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size());
    }
    bool Exclusive(){File file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));return file.value!=INVALID_HANDLE_VALUE;}
};
const auto unlimited=(std::numeric_limits<std::uint64_t>::max)();
void FirstGlobalBudgetFailure(){
    Fixture f;f.Write({2,3,5,7});
    PictureSourceBudget privateBudget;
    BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,privateBudget,source)==PictureSourceResult::Ok);
    const auto sentinel=source;
    allocationIndex=0;allocationFailed=false;failAt=0;allocationsArmed=true;
    const auto status=CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source);
    allocationsArmed=false;
    CHECK(status==PictureSourceResult::NoMemory&&allocationFailed&&source==sentinel);
    CHECK(f.Exclusive());
    const auto initial=GlobalPictureSourceBudget().Usage();
    CHECK(initial.limit==128ull*1024*1024&&initial.used==0);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::Ok);
    CHECK(GlobalPictureSourceBudget().Usage().used==source->RetainedBytes());
    source.reset();CHECK(GlobalPictureSourceBudget().Usage().used==0);

    bool threw=false;allocationIndex=0;allocationFailed=false;failAt=0;allocationsArmed=true;
    try{PictureSourceBudget unavailable;}
    catch(const std::bad_alloc&){threw=true;}
    allocationsArmed=false;
    CHECK(threw&&allocationFailed);
}
void ImmutableLeaseAndBudget(){
    Fixture f;std::vector<unsigned char> bytes(1024*1024+37);
    for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<unsigned char>((i*29+7)%251);
    f.Write(bytes);BackgroundPictureSourcePtr source;
    const auto exact=BackgroundPictureSource::OverheadBytes()+bytes.size();
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,source)==PictureSourceResult::Ok&&source);
    CHECK(source&&source->Size()==bytes.size()&&source->RetainedBytes()==exact&&std::memcmp(source->Data(),bytes.data(),bytes.size())==0);
    CHECK(f.Exclusive());auto shared=source;source.reset();CHECK(shared.use_count()==1);
    f.Write({9,8,7});CHECK(shared->Size()==bytes.size()&&std::memcmp(shared->Data(),bytes.data(),bytes.size())==0);
    CHECK(DeleteFileW(f.path.c_str()));CHECK(std::memcmp(shared->Data(),bytes.data(),bytes.size())==0);
    f.Write(bytes);source=shared;const auto before=source.get();
    for(auto budget:{std::uint64_t(0),BackgroundPictureSource::OverheadBytes(),exact-1}){
        allocationIndex=0;allocationFailed=false;failAt=0;allocationsArmed=true;
        const auto result=CaptureBackgroundPictureSource(f.path.c_str(),budget,source);allocationsArmed=false;
        CHECK(result==PictureSourceResult::TooLarge&&!allocationFailed&&allocationIndex==0&&source.get()==before);CHECK(f.Exclusive());
    }
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,source)==PictureSourceResult::Ok&&source->RetainedBytes()==exact);
    CHECK(std::memcmp(shared->Data(),source->Data(),bytes.size())==0);
}
void InvalidAndLockedFiles(){
    Fixture f;f.Write({1,2,3});BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::Ok);const auto before=source.get();
    CHECK(CaptureBackgroundPictureSource(nullptr,unlimited,source)==PictureSourceResult::Invalid&&source.get()==before);
    CHECK(CaptureBackgroundPictureSource(L"",unlimited,source)==PictureSourceResult::Invalid&&source.get()==before);
    const auto missing=f.path+L".missing";
    CHECK(CaptureBackgroundPictureSource(missing.c_str(),unlimited,source)==PictureSourceResult::OpenFailed&&source.get()==before);
    CHECK(CaptureBackgroundPictureSource(f.directory.c_str(),unlimited,source)==PictureSourceResult::Invalid&&source.get()==before);
    // A pre-existing writer must prevent capture even if it permits readers.
    {File writer(CreateFileW(f.path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        CHECK(writer.value!=INVALID_HANDLE_VALUE);
        CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::OpenFailed&&source.get()==before);}
    CHECK(f.Exclusive());f.Write({});
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::Invalid&&source.get()==before);
    CHECK(f.Exclusive());CHECK(DeleteFileW(f.path.c_str()));
    CHECK(source->Size()==3&&source->Data()[0]==1&&source->Data()[2]==3);
}
void AllocationFailures(){
    Fixture f;const std::vector<unsigned char> bytes{17,19,23,29};f.Write(bytes);
    BackgroundPictureSourcePtr source;CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::Ok);
    const auto sentinel=source;
    allocationIndex=0;failAt=(std::numeric_limits<std::size_t>::max)();allocationsArmed=true;
    const auto success=CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source);allocationsArmed=false;
    const auto count=allocationIndex;CHECK(success==PictureSourceResult::Ok&&count>=3&&count<20);
    for(std::size_t index=0;index<count;++index){
        source=sentinel;allocationIndex=0;allocationFailed=false;failAt=index;allocationsArmed=true;
        const auto result=CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source);allocationsArmed=false;
        CHECK(allocationFailed&&result==PictureSourceResult::NoMemory&&source==sentinel);
        CHECK(source->Size()==bytes.size()&&std::memcmp(source->Data(),bytes.data(),bytes.size())==0);CHECK(f.Exclusive());
    }
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,source)==PictureSourceResult::Ok&&source!=sentinel);
    CHECK(f.Exclusive());CHECK(DeleteFileW(f.path.c_str()));
}
void TotalBudgetAndPeak(){
    Fixture f;f.Write({11,13,17,19});
    const auto exact=BackgroundPictureSource::OverheadBytes()+4;
    PictureSourceBudget budget(exact-1);
    BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,budget,source)==PictureSourceResult::TooLarge&&!source);
    CHECK(budget.Usage().used==0&&budget.Usage().limit==exact-1);
    budget.Configure(exact);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    CHECK(budget.Usage().used==exact);
    const auto* soleSource=source.get();
    CHECK(source.use_count()==1);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::TooLarge&&source.get()==soleSource);
    CHECK(budget.Usage().used==exact);
    auto lease=source;auto another=source;
    CHECK(budget.Usage().used==exact);
    PictureSourceBudget sharedBudget(budget);
    CHECK(sharedBudget.Usage().used==exact);
    // Keeping out unchanged requires capacity for both the published source
    // and its candidate, even when out was otherwise the only reference.
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),unlimited,budget,source)==PictureSourceResult::TooLarge&&source==lease);
    CHECK(budget.Usage().used==exact&&f.Exclusive());
    sharedBudget.Configure(2*exact);
    CHECK(budget.Usage().limit==2*exact);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok&&source!=lease);
    CHECK(budget.Usage().used==2*exact);
    lease.reset();CHECK(budget.Usage().used==2*exact);
    another.reset();CHECK(budget.Usage().used==exact);
    const auto retained=source;
    budget.Configure(exact-1);
    CHECK(budget.Usage().used==exact&&budget.Usage().limit==exact-1);
    CHECK(source->Data()[0]==11);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::TooLarge&&source==retained);
    budget.Configure(0);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::TooLarge&&source==retained);
    source.reset();CHECK(budget.Usage().used==exact);
}
void BudgetRecoveryAndLifetime(){
    Fixture f;f.Write({23,29,31});
    const auto exact=BackgroundPictureSource::OverheadBytes()+3;
    PictureSourceBudget budget(exact);
    BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    std::weak_ptr<const BackgroundPictureSource> weak=source;
    budget.Configure(0);source.reset();
    CHECK(weak.expired()&&budget.Usage().used==0);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::TooLarge&&!source);
    budget.Configure(exact);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    CHECK(budget.Usage().used==exact);source.reset();CHECK(budget.Usage().used==0);
    {
        PictureSourceBudget local(exact);
        CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,local,source)==PictureSourceResult::Ok);
        CHECK(local.Usage().used==exact);
    }
    // Source owns the accounting state after every public budget handle dies.
    CHECK(source&&source->Size()==3&&source->Data()[0]==23);
    weak=source;source.reset();CHECK(weak.expired());
    CHECK(f.Exclusive());
}
void BudgetAllocationFailures(){
    Fixture f;f.Write({37,41,43,47});
    const auto exact=BackgroundPictureSource::OverheadBytes()+4;
    PictureSourceBudget budget(2*exact);
    BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    auto sentinel=source;
    allocationIndex=0;failAt=(std::numeric_limits<std::size_t>::max)();allocationsArmed=true;
    const auto status=CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source);
    allocationsArmed=false;const auto count=allocationIndex;
    CHECK(status==PictureSourceResult::Ok&&count>=3&&count<20&&budget.Usage().used==2*exact);
    source=sentinel;CHECK(budget.Usage().used==exact);
    for(std::size_t index=0;index<count;++index){
        allocationIndex=0;allocationFailed=false;failAt=index;allocationsArmed=true;
        const auto result=CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source);
        allocationsArmed=false;
        CHECK(allocationFailed&&result==PictureSourceResult::NoMemory&&source==sentinel);
        CHECK(budget.Usage().used==exact&&source->Data()[0]==37&&f.Exclusive());
    }
    source.reset();sentinel.reset();CHECK(budget.Usage().used==0);
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    CHECK(budget.Usage().used==exact);source.reset();CHECK(budget.Usage().used==0);
}
void ConcurrentBudgetReservations(){
    Fixture f;f.Write({53,59,61,67});
    const auto exact=BackgroundPictureSource::OverheadBytes()+4;
    constexpr std::size_t workers=8,capacity=3;
    PictureSourceBudget budget(capacity*exact);
    std::array<BackgroundPictureSourcePtr,workers> sources{};
    std::array<PictureSourceResult,workers> results{};
    results.fill(PictureSourceResult::Invalid);
    std::array<std::thread,workers> threads{};
    std::atomic<unsigned> ready{0};std::atomic<bool> start{false};
    try{
        for(std::size_t i=0;i<workers;++i)threads[i]=std::thread([&,i]{
            ready.fetch_add(1);
            while(!start.load())std::this_thread::yield();
            results[i]=CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,sources[i]);
        });
    }catch(...){start.store(true);for(auto& thread:threads)if(thread.joinable())thread.join();throw;}
    while(ready.load()!=workers)std::this_thread::yield();
    start.store(true);for(auto& thread:threads)thread.join();
    std::size_t successes=0;
    for(std::size_t i=0;i<workers;++i){
        CHECK(results[i]==PictureSourceResult::Ok||results[i]==PictureSourceResult::TooLarge);
        if(results[i]==PictureSourceResult::Ok){++successes;CHECK(sources[i]&&sources[i]->Data()[0]==53);}
        else CHECK(!sources[i]);
    }
    CHECK(successes==capacity&&budget.Usage().used==capacity*exact);
    for(auto& source:sources)source.reset();
    CHECK(budget.Usage().used==0&&f.Exclusive());
    BackgroundPictureSourcePtr source;
    CHECK(CaptureBackgroundPictureSource(f.path.c_str(),exact,budget,source)==PictureSourceResult::Ok);
    source.reset();CHECK(budget.Usage().used==0);
}
}
int main(){
    try{FirstGlobalBudgetFailure();ImmutableLeaseAndBudget();InvalidAndLockedFiles();AllocationFailures();
        TotalBudgetAndPeak();BudgetRecoveryAndLifetime();BudgetAllocationFailures();ConcurrentBudgetReservations();
        CHECK(GlobalPictureSourceBudget().Usage().used==0);}
    catch(const std::exception& e){allocationsArmed=false;std::fprintf(stderr,"exception: %s\n",e.what());CHECK(false);}
    catch(...){allocationsArmed=false;CHECK(false);}
    std::printf("background_picture_source_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
