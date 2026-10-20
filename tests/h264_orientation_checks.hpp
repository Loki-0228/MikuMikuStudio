// Included by mf_h264_startup_tests.cpp. The four quadrants are intentionally
// asymmetric so both row order and UV order are observable at the writer.
std::vector<unsigned char> BottomUpQuadrants(int width,int height) {
    const unsigned char colors[4][4]={{0,0,255,255},{0,255,0,255},{255,0,0,255},{255,255,255,255}};
    std::vector<unsigned char> pixels(std::size_t(width)*height*4);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const auto* color=colors[(y>=height/2?2:0)+(x>=width/2?1:0)];
        std::memcpy(pixels.data()+(std::size_t(height-1-y)*width+x)*4,color,4);
    }
    return pixels;
}
void H264InputOrientation() {
    constexpr int width=8,height=8;
    const auto pixels=BottomUpQuadrants(width,height);
    for(bool nv12:{true,false}){
        captureFrame=true;capturedFrame.clear();capturedStride=0;
        DWORD stream=0;
        Check(SUCCEEDED(AddH264VideoStream(&writers[0],width,height,30,1000000,nv12,&stream)),"negotiate production encoder input");
        Check(capturedStride==(nv12?width:-width*4),"NV12 is top-down and software RGB32 remains bottom-up");
        DShowRecorder recorder{};auto recording=std::make_unique<Recording>();
        recording->writer=&writers[0];recording->width=width;recording->height=height;
        recording->nv12Input=nv12;recording->frameBytes=pixels.size();recording->frameDuration=333333;
        gRecordings.emplace(&recorder,std::move(recording));
        Check(WriteBuiltInH264Frame(&recorder,pixels.data(),pixels.size()),"submit real production frame conversion");
        if(nv12){
            Check(capturedFrame.size()==width*height*3/2,"NV12 sample retains exact dimensions");
            const auto y=[&](int x,int row){return capturedFrame[std::size_t(row)*width+x];};
            Check(y(0,0)==62&&y(width-1,0)==172&&y(0,height-1)==32&&y(width-1,height-1)==234,
                "NV12 luma keeps red/green on top and blue/white on the bottom");
            const auto uv=[&](int x,int row,int component){return capturedFrame[width*height+std::size_t(row)*width+x+component];};
            Check(uv(0,0,0)==102&&uv(0,0,1)==240&&uv(width-2,0,0)==41&&uv(width-2,0,1)==26&&
                uv(0,height/2-1,0)==239&&uv(0,height/2-1,1)==117&&uv(width-2,height/2-1,0)==128&&uv(width-2,height/2-1,1)==128,
                "NV12 chroma follows the same vertical direction without swapping U and V");
        }else Check(capturedFrame==pixels,"software RGB32 bytes are unchanged");
        gRecordings.erase(&recorder);captureFrame=false;
    }
}
void H264EncodedOrientation(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    constexpr int width=128,height=96;
    const auto pixels=BottomUpQuadrants(width,height);
    for(bool nv12:{false,true}){
        const auto path=root/(nv12?L"nv12-input.mp4":L"rgb32-input.mp4");
        Check(!std::filesystem::exists(path),"encode only fresh owned output files");
        DShowRecorder recorder{};auto recording=std::make_unique<Recording>();
        recording->width=width;recording->height=height;recording->frameBytes=pixels.size();
        recording->frameDuration=333333;recording->nv12Input=nv12;
        // Exercise exactly the NV12 payload used by hardware, through the
        // system software encoder so this test does not require an AMD GPU.
        Check(SUCCEEDED(OpenH264SinkWriterGuarded(&recording->writer,path.c_str(),false,width,height,
            30,1000000,nv12,&recording->videoStream)),"create real MP4 encoder with RGB32 or NV12 input");
        Check(SUCCEEDED(recording->writer->BeginWriting()),"start real orientation encoding");
        gRecordings.emplace(&recorder,std::move(recording));
        for(int frame=0;frame<3;++frame)Check(WriteBuiltInH264Frame(&recorder,pixels.data(),pixels.size()),"encode asymmetric colour frame");
        Check(FinishBuiltInH264Recording(&recorder,3),"finalize real orientation MP4");

        IMFAttributes* attributes=nullptr;IMFSourceReader* reader=nullptr;
        Check(SUCCEEDED(MFCreateAttributes(&attributes,2)),"create decoder attributes");
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,FALSE);
        Check(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(),attributes,&reader)),"decode encoded MP4");attributes->Release();
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
        IMFMediaType* type=nullptr;Check(SUCCEEDED(MFCreateMediaType(&type)),"create NV12 decode request");
        type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
        Check(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type)),"request top-down NV12 decoded pixels");type->Release();
        IMFMediaType* actual=nullptr;Check(SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&actual)),"read decoded geometry");
        UINT32 decodedWidth=0,decodedHeight=0,stride=width;
        Check(SUCCEEDED(MFGetAttributeSize(actual,MF_MT_FRAME_SIZE,&decodedWidth,&decodedHeight))&&decodedWidth==width&&decodedHeight==height,"decoded dimensions match the source");
        actual->GetUINT32(MF_MT_DEFAULT_STRIDE,&stride);actual->Release();
        Check(stride>=width&&stride<width*4,"decoded NV12 has a positive luma stride");
        IMFSample* sample=nullptr;
        for(int attempt=0;attempt<20&&!sample;++attempt){DWORD flags=0;LONGLONG time=0;
            Check(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&time,&sample)),"read encoded first frame");
            Check(!(flags&(MF_SOURCE_READERF_ERROR|MF_SOURCE_READERF_ENDOFSTREAM)),"first frame decodes before end of stream");
        }
        Check(sample!=nullptr,"decoded first frame is present");
        IMFMediaBuffer* buffer=nullptr;Check(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)),"read decoded frame buffer");
        BYTE* data=nullptr;DWORD size=0;Check(SUCCEEDED(buffer->Lock(&data,nullptr,&size)),"lock decoded frame");
        Check(size>=std::size_t(stride)*height*3/2,"decoded buffer contains both planes");
        const auto y=[&](int x,int row){return int(data[std::size_t(row)*stride+x]);};
        Check(y(width/4,height/4)>y(width/4,3*height/4)+15&&y(3*width/4,3*height/4)>y(3*width/4,height/4)+25,
            "decoded MP4 retains the vertical brightness pattern");
        const auto uv=[&](int x,int row,int component){return int(data[std::size_t(stride)*height+std::size_t(row/2)*stride+(x&~1)+component]);};
        Check(uv(width/4,height/4,1)>uv(width/4,height/4,0)+50&&
            uv(width/4,3*height/4,0)>uv(width/4,3*height/4,1)+50&&
            uv(3*width/4,height/4,0)<110&&uv(3*width/4,height/4,1)<110&&
            std::abs(uv(3*width/4,3*height/4,0)-128)<15&&std::abs(uv(3*width/4,3*height/4,1)-128)<15,
            "decoded MP4 keeps red/green above blue/white without mirroring or colour-plane inversion");
        buffer->Unlock();buffer->Release();sample->Release();reader->Release();
        std::printf("PASS decoded orientation input=%s file=%ls\n",nv12?"NV12":"RGB32",path.c_str());
    }
}
