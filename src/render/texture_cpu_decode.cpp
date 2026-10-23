#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "texture_cpu_decode.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstring>

namespace mikumikustudio::texture_cpu_decode {
namespace {
bool Png(const Bytes& b) noexcept {
    static constexpr unsigned char signature[]{137,80,78,71,13,10,26,10};
    return b.size() >= 33 && !std::memcmp(b.data(), signature, 8)
        && !std::memcmp(b.data()+12, "IHDR", 4) && b[24] == 8
        && (b[25] == 2 || b[25] == 6) && b[26] == 0 && b[27] == 0 && b[28] == 0;
}
bool Tga(const Bytes& b) noexcept {
    return b.size() >= 18 && b[1] == 0 && (b[2] == 2 || b[2] == 10)
        && (b[16] == 24 || b[16] == 32) && !(b[17] & 0xc0);
}
bool DecodeTga(const Bytes& b, unsigned width, unsigned height, unsigned char* output) {
    if (width != b[12] + 256u*b[13] || height != b[14] + 256u*b[15]) return false;
    const unsigned components = b[16]/8;
    const std::size_t count = std::size_t(width)*height;
    std::size_t cursor = 18+b[0], pixel = 0;
    while (pixel < count) {
        unsigned run = 1; bool repeated = false;
        if (b[2] == 10) {
            if (cursor >= b.size()) return false;
            const unsigned packet = b[cursor++];
            run = (packet & 127)+1; repeated = (packet & 128) != 0;
        }
        if (run > count-pixel) return false;
        // Native D3DX rejects repeated packets crossing a scanline. Leave
        // these streams to its original loader instead of accepting more data.
        if (b[2]==10 && run>width-pixel%width) return false;
        const std::size_t needed = (repeated ? 1 : run)*components;
        if (cursor > b.size() || needed > b.size()-cursor) return false;
        for (unsigned i=0; i<run; ++i, ++pixel) {
            const auto* source = b.data()+cursor+(repeated ? 0 : i*components);
            unsigned x = static_cast<unsigned>(pixel % width);
            unsigned y = static_cast<unsigned>(pixel / width);
            if (b[17]&16) x=width-1-x;
            if (!(b[17]&32)) y=height-1-y;
            auto* target = output+(std::size_t(y)*width+x)*4;
            std::memcpy(target, source, 3); target[3] = components==4 ? source[3] : 255;
        }
        cursor += needed;
    }
    return true;
}
bool DecodePng(const Bytes& bytes, unsigned width, unsigned height, unsigned char* output) {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result)) return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w=0,h=0;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))
        && SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w,&h))
        && w==width && h==height && SUCCEEDED(factory->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(converter->CopyPixels(nullptr, width*4, width*height*4, output));
}
}
bool Supported(const Bytes& input) noexcept { return Png(input) || Tga(input); }
bool Decode(const Bytes& input, unsigned width, unsigned height, Bytes& dds) {
    const std::uint64_t bytes = std::uint64_t(width)*height*4;
    if (!width || !height || width>16384 || height>16384 || bytes>128ull*1024*1024
        || !Supported(input)) return false;
    Bytes result(static_cast<std::size_t>(bytes)+128);
    // DDS magic + DDS_HEADER; all reserved fields remain zero.
    const std::uint32_t header[]{0x20534444,124,0x100f,height,width,width*4,0,1};
    std::memcpy(result.data(),header,sizeof header);
    const std::uint32_t pixelFormat[]{32,0x41,0,32,0x00ff0000,0x0000ff00,0x000000ff,0xff000000,0x1000};
    std::memcpy(result.data()+76,pixelFormat,sizeof pixelFormat);
    if (!(Png(input) ? DecodePng(input,width,height,result.data()+128)
                    : DecodeTga(input,width,height,result.data()+128))) return false;
    dds = std::move(result); return true;
}
}
