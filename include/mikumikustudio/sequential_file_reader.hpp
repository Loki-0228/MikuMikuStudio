#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <io.h>

namespace mikumikustudio::fileio {
struct ReadStatistics {
    std::uint64_t requests=0, systemReads=0, bytes=0;
};
inline thread_local ReadStatistics readStatistics{};
// Test harnesses can compare identical parser code with buffering disabled.
inline thread_local bool bufferedReads=true;

// Owns one binary, seekable CRT descriptor from its current position onward.
// All reads and position queries must go through this object until Close().
// Bounded read-ahead avoids a syscall for every PMX scalar/index field.
class SequentialFileReader {
public:
    explicit SequentialFileReader(int descriptor)
        : descriptor_(descriptor), buffered_(bufferedReads), position_(_telli64(descriptor)) {}
    ~SequentialFileReader(){Close();}
    SequentialFileReader(const SequentialFileReader&)=delete;
    SequentialFileReader& operator=(const SequentialFileReader&)=delete;

    int Read(void* destination,unsigned count) {
        ++statistics_.requests;
        if(descriptor_<0){errno=EBADF;return -1;}
        if(!count)return 0;
        if(!destination||count>INT_MAX){errno=EINVAL;return -1;}
        if(!buffered_) {
            ++statistics_.systemReads;
            const int result=_read(descriptor_,destination,count);
            if(result>0){position_+=result;statistics_.bytes+=result;}
            return result;
        }
        unsigned copied=0;
        while(copied<count) {
            if(cursor_==available_) {
                ++statistics_.systemReads;
                const int result=_read(descriptor_,buffer_.data(),static_cast<unsigned>(buffer_.size()));
                cursor_=available_=0;
                if(result<=0)return copied?static_cast<int>(copied):result;
                available_=static_cast<unsigned>(result);
            }
            const unsigned take=std::min(count-copied,available_-cursor_);
            std::memcpy(static_cast<char*>(destination)+copied,buffer_.data()+cursor_,take);
            cursor_+=take;copied+=take;position_+=take;statistics_.bytes+=take;
        }
        return static_cast<int>(copied);
    }
    std::int64_t Tell()const{return position_;}
    // Cached parsed sections may skip their source bytes. Reset read-ahead only
    // after a successful seek, so a failed seek leaves the reader usable.
    bool SeekAbsolute(std::int64_t position) {
        if(descriptor_<0){errno=EBADF;return false;}
        if(position<0){errno=EINVAL;return false;}
        const auto result=_lseeki64(descriptor_,position,SEEK_SET);
        if(result<0)return false;
        position_=result;cursor_=available_=0;
        return true;
    }
    int Close() {
        if(descriptor_<0)return 0;
        const int descriptor=descriptor_;descriptor_=-1;
        readStatistics.requests+=statistics_.requests;
        readStatistics.systemReads+=statistics_.systemReads;
        readStatistics.bytes+=statistics_.bytes;
        return _close(descriptor);
    }
private:
    int descriptor_;
    bool buffered_;
    std::int64_t position_;
    unsigned cursor_=0,available_=0;
    std::array<unsigned char,64*1024> buffer_;
    ReadStatistics statistics_{};
};
}
