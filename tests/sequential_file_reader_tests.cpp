// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include <cerrno>
#include <cstdint>
#include <io.h>
namespace {
bool rejectSeek=false;
std::int64_t TestSeek(int descriptor,std::int64_t position,int origin) {
    if(rejectSeek){errno=EIO;return -1;}
    return _lseeki64(descriptor,position,origin);
}
}
#define _lseeki64 TestSeek
#include "mikumikustudio/sequential_file_reader.hpp"
#undef _lseeki64
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
using namespace mikumikustudio::fileio;
void Check(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}}
struct ObservedBytes {
    const unsigned char* next;
    std::size_t remaining;
    unsigned calls = 0;
    static bool Add(void* context, const unsigned char* bytes, unsigned count) noexcept {
        auto& state = *static_cast<ObservedBytes*>(context);
        ++state.calls;
        if (count > state.remaining || std::memcmp(state.next, bytes, count)) return false;
        state.next += count;
        state.remaining -= count;
        return true;
    }
};
int main(){
    FILE* file=nullptr;Check(tmpfile_s(&file)==0,"temporary file");
    std::vector<unsigned char> data(3*65536+37);
    for(size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>((i*131+i/17)%251);
    Check(fwrite(data.data(),1,data.size(),file)==data.size(),"write fixture");Check(fflush(file)==0,"flush fixture");
    for(bool buffered:{false,true}){
        bufferedReads=buffered;readStatistics={};
        int fd=_dup(_fileno(file));Check(fd>=0,"duplicate descriptor");Check(_setmode(fd,_O_BINARY)>=0,"binary mode");
        Check(_lseeki64(fd,3,SEEK_SET)==3,"existing parser prefix");
        {
            SequentialFileReader reader(fd);size_t offset=3;
            // Tiny fields, refill boundaries, a read larger than the buffer,
            // a short final read, and repeated EOF must match raw binary reads.
            const unsigned sizes[]={1,2,4,17,65510,7,131073,79};
            for(unsigned size:sizes){
                std::vector<unsigned char> actual(size,0xcd);
                const size_t count=std::min<size_t>(size,data.size()-offset);
                Check(reader.Read(actual.data(),size)==int(count),"read length");
                Check(std::memcmp(actual.data(),data.data()+offset,count)==0,"read bytes");
                for(size_t j=count;j<size;++j)Check(actual[j]==0xcd,"short read preserves tail");
                offset+=count;Check(reader.Tell()==static_cast<std::int64_t>(offset),"logical offset");
            }
            char byte;Check(reader.Read(&byte,1)==0&&reader.Read(&byte,1)==0,"repeated EOF");
            Check(reader.Read(nullptr,0)==0,"empty read");
            Check(reader.Close()==0&&reader.Close()==0,"idempotent close");
            Check(reader.Read(&byte,1)==-1&&errno==EBADF,"read after close");
        }
        Check(readStatistics.bytes==data.size()-3,"byte accounting");
        if(buffered)Check(readStatistics.systemReads<readStatistics.requests,"refills reduce system reads");
    }
    for(bool buffered:{false,true}){
        bufferedReads=buffered;
        int fd=_dup(_fileno(file));Check(fd>=0,"duplicate seek descriptor");
        Check(_setmode(fd,_O_BINARY)>=0,"seek binary mode");
        Check(_lseeki64(fd,0,SEEK_SET)==0,"seek fixture start");
        SequentialFileReader reader(fd);
        unsigned char byte=0;
        Check(reader.Read(&byte,1)==1&&byte==data[0],"prime seek window");
        Check(!reader.SeekAbsolute(-1)&&errno==EINVAL&&reader.Tell()==1,"negative seek preserves position");
        rejectSeek=true;
        Check(!reader.SeekAbsolute(17)&&errno==EIO&&reader.Tell()==1,"valid descriptor seek failure");
        rejectSeek=false;
        Check(reader.Read(&byte,1)==1&&byte==data[1],"failed seek preserves unread bytes");
        Check(reader.SeekAbsolute(65535)&&reader.Tell()==65535,"absolute forward seek");
        unsigned char pair[2]{};
        Check(reader.Read(pair,2)==2&&pair[0]==data[65535]&&pair[1]==data[65536],"seek discards old window");
        Check(reader.SeekAbsolute(3)&&reader.Read(&byte,1)==1&&byte==data[3],"absolute backward seek");
        Check(reader.SeekAbsolute(static_cast<std::int64_t>(data.size()))&&reader.Read(&byte,1)==0,"seek to EOF");
        Check(reader.SeekAbsolute(7)&&reader.Read(&byte,1)==1&&byte==data[7],"seek after EOF");
        Check(reader.Close()==0&&!reader.SeekAbsolute(0)&&errno==EBADF,"seek after close");
    }
    for (bool buffered : {false, true}) {
        bufferedReads = buffered;
        const int fd = _dup(_fileno(file));
        Check(fd >= 0 && _setmode(fd, _O_BINARY) >= 0 && _lseeki64(fd, 0, SEEK_SET) == 0,
            "open observation descriptor");
        SequentialFileReader reader(fd);
        unsigned char byte = 0;
        Check(reader.Read(&byte, 1) == 1, "read prefix before observing");
        std::vector<unsigned char> slice(65536 + 3);
        ObservedBytes exact{data.data() + 1, slice.size()};
        Check(reader.BeginReadObservation(ObservedBytes::Add, &exact), "start observing a buffered suffix");
        Check(!reader.BeginReadObservation(ObservedBytes::Add, &exact), "reject nested observation");
        Check(reader.Read(slice.data(), static_cast<unsigned>(slice.size())) == static_cast<int>(slice.size()),
            "consume across a refill");
        Check(reader.FinishReadObservation() && exact.remaining == 0 &&
            exact.calls == (buffered ? 2u : 1u), "report just the consumed slice");
        ObservedBytes rejected{data.data(), 0};
        Check(reader.BeginReadObservation(ObservedBytes::Add, &rejected) &&
            reader.Read(&byte, 1) == 1 && !reader.FinishReadObservation(), "observer failure preserves reading");
        Check(reader.BeginReadObservation(ObservedBytes::Add, &rejected) &&
            reader.SeekAbsolute(static_cast<std::int64_t>(data.size()) - 1) &&
            !reader.FinishReadObservation(), "seek invalidates the observed run");
        unsigned char pair[2]{};
        Check(reader.BeginReadObservation(ObservedBytes::Add, &rejected) &&
            reader.Read(pair, 2) == 1 && pair[0] == data.back() &&
            !reader.FinishReadObservation(), "short read invalidates the observed run");
        Check(reader.BeginReadObservation(ObservedBytes::Add, &rejected) &&
            reader.Close() == 0 && !reader.FinishReadObservation(), "close detaches without a callback");
    }
    bufferedReads=true;
    fclose(file);std::puts("PASS buffered binary reads, seek success/failure, ownership, boundaries and EOF");
}
