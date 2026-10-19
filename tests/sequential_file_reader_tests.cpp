// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/sequential_file_reader.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
using namespace mikumikustudio::fileio;
void Check(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}}
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
    fclose(file);std::puts("PASS buffered binary reads, offsets, boundaries and EOF");
}
