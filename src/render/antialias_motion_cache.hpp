#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mikumikustudio/vertex_capture.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>

namespace mikumikustudio::antialias::motion::detail {
// A cache is valid for one scene frame only. Contents, rather than buffer
// addresses alone, are compared so an effect rewriting a stream between its
// material draws cannot reuse an earlier vertex-shader result.
class VertexCache {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    using Position = std::array<float,4>;
    struct LockedStream {
        ComPtr<IDirect3DVertexBuffer9> buffer;
        const unsigned char* data=nullptr;
        UINT bytes=0;
        void Unlock(){if(data){buffer->Unlock();data=nullptr;}}
        ~LockedStream(){Unlock();}
    };
    struct Input {
        std::vector<unsigned char> state;
        std::array<LockedStream,16> streams;
        unsigned count=0;
        void Append(const void* data,std::size_t bytes) {
            if(!bytes)return;
            const auto* begin=static_cast<const unsigned char*>(data);
            state.insert(state.end(),begin,begin+bytes);
        }
        template<class T>void Append(const T& value){Append(&value,sizeof(value));}
        void Unlock(){for(auto& stream:streams)stream.Unlock();}
    };
    struct Entry {
        std::vector<unsigned char> state;
        std::vector<std::vector<unsigned char>> input;
        std::vector<Position> clip;
        std::shared_ptr<const vertex_capture::GpuPositions> gpu;
    };
    std::map<std::uint64_t,std::vector<Entry>> entries_;
    std::size_t bytes_=0;
    vertex_capture::GpuCapture gpuCapture_;
    static constexpr std::size_t kBudget=64*1024*1024;
    static bool Read(IDirect3DDevice9* device,const std::vector<DWORD>& shader,
                     UINT first,UINT count,Input& input) {
        // Texture content can change through rendering without Lock(). Reuse
        // nothing when a vertex texture is bound, even if a shader ignores it.
        for(unsigned i=0;i<4;++i) {
            ComPtr<IDirect3DBaseTexture9> texture;
            if(FAILED(device->GetTexture(D3DVERTEXTEXTURESAMPLER0+i,&texture))||texture)return false;
        }
        D3DCAPS9 caps{};
        if(FAILED(device->GetDeviceCaps(&caps))||!caps.MaxVertexShaderConst||caps.MaxVertexShaderConst>4096)return false;
        std::vector<float> floats(std::size_t(caps.MaxVertexShaderConst)*4);
        std::array<int,16*4> integers{};std::array<BOOL,16> booleans{};
        if(FAILED(device->GetVertexShaderConstantF(0,floats.data(),caps.MaxVertexShaderConst))||
           FAILED(device->GetVertexShaderConstantI(0,integers.data(),16))||
           FAILED(device->GetVertexShaderConstantB(0,booleans.data(),16)))return false;
        input.Append(first);input.Append(count);input.Append(shader.size());input.Append(shader.data(),shader.size()*sizeof(DWORD));
        input.Append(floats.data(),floats.size()*sizeof(float));input.Append(integers);input.Append(booleans);
        ComPtr<IDirect3DVertexDeclaration9> declaration;
        if(FAILED(device->GetVertexDeclaration(&declaration))||!declaration)return false;
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT size=MAXD3DDECLLENGTH+1;
        if(FAILED(declaration->GetDeclaration(elements,&size))||!size||size>MAXD3DDECLLENGTH+1)return false;
        input.Append(size);input.Append(elements,size*sizeof(elements[0]));
        constexpr unsigned typeBytes[]={4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
        std::array<unsigned,16> extents{};
        for(UINT i=0;i<size&&elements[i].Stream!=0xff;++i) {
            const auto& element=elements[i];
            if(element.Stream>=16||element.Type>=std::size(typeBytes))return false;
            extents[element.Stream]=std::max(extents[element.Stream],unsigned(element.Offset)+typeBytes[element.Type]);
        }
        for(unsigned i=0;i<16;++i)if(extents[i]) {
            auto& stream=input.streams[input.count++];UINT offset=0,stride=0,frequency=1;
            if(FAILED(device->GetStreamSource(i,&stream.buffer,&offset,&stride))||!stream.buffer||
               FAILED(device->GetStreamSourceFreq(i,&frequency))||frequency!=1||!stride)return false;
            D3DVERTEXBUFFER_DESC desc{};
            if(FAILED(stream.buffer->GetDesc(&desc))||(desc.Usage&D3DUSAGE_WRITEONLY))return false;
            const std::uint64_t start=std::uint64_t(offset)+std::uint64_t(first)*stride;
            const std::uint64_t bytes=std::uint64_t(count-1)*stride+extents[i];
            if(start+bytes>desc.Size||bytes>kBudget)return false;
            input.Append(i);input.Append(reinterpret_cast<std::uintptr_t>(stream.buffer.Get()));
            input.Append(offset);input.Append(stride);input.Append(frequency);input.Append(bytes);
            void* data=nullptr;
            if(FAILED(stream.buffer->Lock(UINT(start),UINT(bytes),&data,D3DLOCK_READONLY)))return false;
            stream.data=static_cast<const unsigned char*>(data);stream.bytes=UINT(bytes);
        }
        return input.count!=0;
    }
public:
    unsigned captures=0,hits=0,gpuCaptures=0,cpuReadbacks=0;
    void Clear(){entries_.clear();bytes_=0;captures=hits=gpuCaptures=cpuReadbacks=0;}
    bool CaptureGpu(IDirect3DDevice9* device,const std::vector<DWORD>& shader,INT base,UINT minVertex,
                    UINT vertexCount,UINT first,UINT count,
                    std::shared_ptr<const vertex_capture::GpuPositions>& output,std::string& error) {
        std::vector<Position> unused;
        return Capture(device,shader,base,minVertex,vertexCount,first,count,unused,error,&output);
    }
    bool Capture(IDirect3DDevice9* device,const std::vector<DWORD>& shader,INT base,UINT minVertex,
                 UINT vertexCount,UINT first,UINT count,std::vector<Position>& output,std::string& error,
                 std::shared_ptr<const vertex_capture::GpuPositions>* gpu=nullptr) {
        output.clear();if(gpu)gpu->reset();
        const auto capture=[&](UINT start,UINT size,std::vector<Position>& positions,
                               std::shared_ptr<const vertex_capture::GpuPositions>& texture) {
            ++captures;
            if(gpu){++gpuCaptures;return gpuCapture_.Capture(device,start,size,texture,error);}
            ++cpuReadbacks;return vertex_capture::Capture(device,start,size,positions,error);
        };
        std::shared_ptr<const vertex_capture::GpuPositions> unused;
        const auto direct=[&](){return capture(first,count,output,gpu?*gpu:unused);};
        const std::int64_t declaredFirst=std::int64_t(base)+minVertex;
        Input input;
        const bool cacheable=vertexCount&&vertexCount<=2000000&&declaredFirst>=0&&
            std::uint64_t(declaredFirst)+vertexCount<=std::numeric_limits<UINT>::max()&&
            first>=std::uint64_t(declaredFirst)&&std::uint64_t(first)+count<=std::uint64_t(declaredFirst)+vertexCount&&
            Read(device,shader,UINT(declaredFirst),vertexCount,input);
        if(!cacheable) {
            input.Unlock();return direct();
        }
        std::uint64_t key=14695981039346656037ull;
        for(auto byte:input.state)key=(key^byte)*1099511628211ull;
        auto& candidates=entries_[key];
        for(const auto& entry:candidates)if(entry.state==input.state&&entry.input.size()==input.count&&
            (gpu?bool(entry.gpu):!entry.clip.empty())) {
            bool equal=true;
            for(unsigned i=0;i<input.count;++i)if(entry.input[i].size()!=input.streams[i].bytes||
                std::memcmp(entry.input[i].data(),input.streams[i].data,input.streams[i].bytes)!=0){equal=false;break;}
            if(equal) {
                const auto offset=first-UINT(declaredFirst);
                if(gpu)*gpu=entry.gpu;
                else output.assign(entry.clip.begin()+offset,entry.clip.begin()+offset+count);
                ++hits;return true;
            }
        }
        std::size_t entryBytes=input.state.size()+std::size_t(vertexCount)*(gpu?32:sizeof(Position));
        for(unsigned i=0;i<input.count;++i)entryBytes+=input.streams[i].bytes;
        if(entryBytes>kBudget-bytes_) {
            input.Unlock();return direct();
        }
        Entry entry;entry.state=std::move(input.state);
        for(unsigned i=0;i<input.count;++i)
            entry.input.emplace_back(input.streams[i].data,input.streams[i].data+input.streams[i].bytes);
        input.Unlock();
        if(!capture(UINT(declaredFirst),vertexCount,entry.clip,entry.gpu)) {
            // Some effects declare a wider range than the one actually used.
            // A cache miss must not make a previously supported draw fail.
            if(first==UINT(declaredFirst)&&count==vertexCount)return false;
            return direct();
        }
        const auto offset=first-UINT(declaredFirst);
        if(gpu)*gpu=entry.gpu;
        else output.assign(entry.clip.begin()+offset,entry.clip.begin()+offset+count);
        bytes_+=entryBytes;candidates.push_back(std::move(entry));return true;
    }
};
}
