namespace {
// Literal VMD file order is X1, X2, Y1, Y2. The six lanes deliberately differ.
constexpr std::uint8_t kCameraFileLanes[6][4] = {
    {18,103,95,12}, {4,76,112,19}, {55,111,5,124},
    {7,120,119,9}, {34,95,96,45}, {1,126,17,104}
};
Bytes NativeCameraFixture(int version, const Bytes& extension) {
    Bytes bytes;
    Text(bytes,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002",30);
    Text(bytes,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",version==1?10:20);
    U32(bytes,0); U32(bytes,0); U32(bytes,2);
    for (int key=0;key<2;++key) {
        U32(bytes,key?23:0); Put(bytes,float(key));
        for (int value=0;value<6;++value) Put(bytes,float(key));
        for (int lane=0;lane<(version==1?1:6);++lane)
            for (auto control:kCameraFileLanes[lane]) bytes.push_back(control);
        if (version==2) { U32(bytes,key?90:30); bytes.push_back(std::uint8_t(key)); }
    }
    U32(bytes,1); U32(bytes,0);
    for (float value:{.2f,.3f,.4f,1.f,-1.f,0.f}) Put(bytes,value);
    if (version==2) { U32(bytes,1); U32(bytes,0); bytes.push_back(2); Put(bytes,.04f); }
    U32(bytes,1); U32(bytes,5); bytes.push_back(1); U32(bytes,1); Text(bytes,"bone",20); bytes.push_back(0);
    bytes.insert(bytes.end(),extension.begin(),extension.end());
    return bytes;
}
std::uint32_t CameraValueBits(float value) {
    std::uint32_t bits=0; std::memcpy(&bits,&value,sizeof bits); return bits;
}
std::uint32_t CameraWord(const Bytes& bytes,std::size_t offset) {
    Check(offset<=bytes.size()&&bytes.size()-offset>=4,"camera compatibility word is inside the file");
    std::uint32_t value=0; std::memcpy(&value,bytes.data()+offset,4); return value;
}
void CheckNativeCameraFrame(Fixture& fixture,int version,int frame) {
    fixture.app->CurrentFrame()=frame; ReloadModels(fixture.app.get());
    const float values[]={fixture.app->CameraPositionAxis(0),fixture.app->CameraPositionAxis(1),fixture.app->CameraPositionAxis(2),
                          fixture.app->CameraRotation()[0],fixture.app->CameraDistance(),fixture.app->CameraFov()};
    if (frame==7) {
        // x64 12-step, float-SSE reference values for t=float(7/23).
        // Frozen outputs detect a changed iteration count, double promotion or
        // camera-lane order; the test does not duplicate the native solver.
        constexpr std::uint32_t expected[]={0x3ecc399fu,0x3f0056f3u,0x3e1e40a1u,0x3ef4e6a7u,0x3ee14643u,0x4243d60du};
        for (int channel=0;channel<6;++channel) {
            const auto bits=version==1?(channel==5?CameraValueBits(45.f):expected[0]):expected[channel];
            if (CameraValueBits(values[channel])!=bits)
                std::fprintf(stderr,"camera bits version=%d channel=%d expected=%08x actual=%08x\n",version,channel,bits,CameraValueBits(values[channel]));
            Check(CameraValueBits(values[channel])==bits,"native camera retains 12-step single-precision compatibility");
        }
        Check(fixture.app->CameraRotation()[0]==fixture.app->CameraRotation()[1]&&fixture.app->CameraRotation()[1]==fixture.app->CameraRotation()[2],
              "one native rotation lane drives all three Euler components");
        Check(fixture.app->CameraPerspective()==0,"native perspective holds its prior value between keys");
    } else if (frame==23) {
        Check(values[0]==1&&values[1]==1&&values[2]==1&&values[3]==1&&values[4]==1,"native camera exact endpoint does not run easing");
        Check(values[5]==(version==1?45.f:90.f)&&fixture.app->CameraPerspective()==(version==1?0:1),
              "native FOV and perspective switch exactly at their endpoint");
    }
}
void NativeCameraCurveCompatibility() {
    namespace cm=mikudancestudio::curve_motion;
    for (int version:{1,2}) {
        Fixture fixture(true); fixture.EnableAccessory();
        accessory_motion_io::Track track; track.name="prop.x";
        accessory_motion_io::Key first; first.position[0]=2.5f;
        auto second=first; second.frame=23; second.position[0]=6; second.opacity=.375f; second.visible=0;
        track.keys={first,second}; const auto extension=accessory_motion_io::Encode({track});
        const auto source=NativeCameraFixture(version,extension);
        Check(fixture.Load(source)==0,"real native VMD1/VMD2 camera load retains historical result");
        const auto linked=fixture.cameraKeys[0].next;
        Check(linked&&linked<fixture.cameraKeys.size()&&fixture.cameraKeys[linked].frame==23,"native camera imports both records with the correct stride");
        for (int lane=0;lane<6;++lane) {
            const auto& bytes=kCameraFileLanes[version==1?0:lane]; const auto& key=fixture.cameraKeys[linked];
            Check(key.interpolation[0][lane]==bytes[0]&&key.interpolation[2][lane]==bytes[1]&&
                  key.interpolation[1][lane]==bytes[2]&&key.interpolation[3][lane]==bytes[3],
                  "native camera maps standard lane bytes to the original transposed control table");
        }
        CheckNativeCameraFrame(fixture,version,7); CheckNativeCameraFrame(fixture,version,23);
        fixture.app->CurrentFrame()=0;
        cm::Motion curves; std::string error;
        Check(cm::Decode(source.data(),source.size(),cm::Kind::Camera,curves,error),"curve transport reads real native VMD1/VMD2 layout with the same standard boundary");
        Check(curves.tracks.size()==9,"curve transport keeps all camera channels beside the accessory tail");
        wchar_t savedPath[MAX_PATH]{};
        Check(MultiByteToWideChar(CP_ACP,0,fixture.file.path,-1,savedPath,MAX_PATH)!=0,"convert camera compatibility save path");
        SaveVmdFile(savedPath);
        std::ifstream savedFile(std::filesystem::path(savedPath),std::ios::binary);
        Check(bool(savedFile),"open real camera writer output");
        const Bytes saved((std::istreambuf_iterator<char>(savedFile)),std::istreambuf_iterator<char>()); savedFile.close();
        Check(saved.size()>62&&std::memcmp(saved.data(),"Vocaloid Motion Data 0002",25)==0,"native writer exports the VMD2 signature for both source versions");
        Check(CameraWord(saved,50)==0&&CameraWord(saved,54)==0&&CameraWord(saved,58)==2,"native writer camera counts preserve standard section routing");
        for (int key=0;key<2;++key) for (int lane=0;lane<6;++lane) {
            const auto position=62+std::size_t(key)*61+32+std::size_t(lane)*4;
            Check(position+4<=saved.size()&&std::memcmp(saved.data()+position,kCameraFileLanes[version==1?0:lane],4)==0,
                  "native writer keeps each camera lane in X1/X2/Y1/Y2 file order");
        }
        const auto standardEnd=std::size_t(224+(version==2?9:0));
        Check(CameraWord(saved,184)==1&&CameraWord(saved,216)==(version==2?1u:0u)&&CameraWord(saved,standardEnd-4)==0,
              "native writer closes light, version-dependent shadow and display sections before the extension");
        Check(saved.size()==standardEnd+extension.size()&&std::equal(extension.begin(),extension.end(),saved.begin()+standardEnd),
              "native writer preserves the MMS accessory extension after the complete standard prefix");
        for (bool keepExtension:{false,true}) {
            fixture.app->CurrentFrame()=0;
            auto input=saved; if (!keepExtension) input.resize(standardEnd);
            fixture.accessory->position[0]=-77;
            Check(fixture.Load(input)==0,"real exported camera VMD reimports with or without its extension");
            CheckNativeCameraFrame(fixture,version,7); CheckNativeCameraFrame(fixture,version,23);
            if (keepExtension) {
                Check(fixture.accessoryKeys[0].position[0]==2.5f&&fixture.accessoryKeys[fixture.accessoryKeys[0].next].position[0]==6,
                      "real exported accessory extension preserves both motion keys on reimport");
            }
        }
    }
}
} // namespace
