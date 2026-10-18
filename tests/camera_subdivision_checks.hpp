static void CheckCameraSubdivision(){
    for(const std::array<double,4> controls: {std::array<double,4>{20,20,107,107},{20,0,107,127},
        {100,10,20,115},{0,0,127,127},{0,127,127,0}}){
        auto app=std::make_unique<MMDApp>();
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        auto pool=std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);app->CameraKeys()=pool.get();
        for(unsigned i=0;i<3;++i){auto&key=pool[i];key.frame=i*50;key.next=i<2?i+1:0;key.previous=i?i-1:0;
            key.parentModel=-1;key.fov=45;key.distance=-30;
            for(int axis=0;axis<3;++axis){key.eye[axis]=float(i*10+axis);key.target[axis]=float(i*.2+axis*.1);}
            for(int lane=0;lane<6;++lane)for(int k=0;k<4;++k)key.interpolation[k][lane]=static_cast<unsigned char>(controls[k]);
        }
        std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"native subdivision read");
        const auto before=channels;
        if(controls[0]==20&&controls[1]==20){
            for(double f:{.123,4.001,10.127,25.479,49.789})
                Check(std::fabs(channels[0].sample(channels[0],f).derivative-.2)<1e-12,"linear native camera speed has no quantization spikes");
        }
        if(controls==std::array<double,4>{20,0,107,127})
            Check(std::fabs(channels[0].sample(channels[0],25).derivative-.2*254/214)<1e-12,"native ease speed matches the analytic midpoint slope");
        Check(curve_host::InsertCameraKey(channels,3,17,error),"subdivide linked camera rotation");
        for(int c=3;c<6;++c)Check(channels[c].track.keys.size()==4&&channels[c].track.keys[1].frame==17,"rotation axes subdivide together");
        Check(channels[0].track.keys.size()==3,"rotation subdivision keeps unrelated authored frames intact");
        Check(curve_host::InsertCameraKey(channels,0,1,error),"insert one-frame half without creating a camera cut");
        Check(curve_host::InsertCameraKey(channels,0,23,error),"subdivide camera repeatedly");
        for(unsigned c=0;c<8;++c){std::string reason;
            Check(animation_curve::Validate(channels[c].track,reason),"subdivision keeps scalar proxy valid");
            for(int step=0;step<=400;++step){const double f=step*.25;
                Check(std::fabs(channels[c].sample(channels[c],f).value-before[c].sample(before[c],f).value)<.04,
                    "subdivision preserves native camera motion within native inverse precision");
            }
        }
        const auto divided=channels;
        Check(curve_host::Write(*app,curve_host::Kind::Camera,0,channels,error),"commit subdivision");
        for(unsigned i=0;i<8;++i)for(int k=0;k<4;++k)for(int lane=0;lane<6;++lane)
            Check(app->CameraKeys()[i].interpolation[k][lane]<=127,"native camera controls stay VMD-compatible integers");
        pmm_extension::Document doc;curve_host::Capture(*app,doc);curve_host::Reset(app.get());curve_host::Load(*app,doc);
        Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"reload subdivided camera authoring");
        for(unsigned c=0;c<8;++c)for(int step=0;step<=400;++step){const double f=step*.25;
            if(std::fabs(channels[c].sample(channels[c],f).value-divided[c].sample(divided[c],f).value)>=1e-10)
            {
                std::fprintf(stderr,"roundtrip controls %.0f %.0f %.0f %.0f, lane %u frame %.2f, before %.12g after %.12g\n",
                    controls[0],controls[1],controls[2],controls[3],c,f,divided[c].sample(divided[c],f).value,channels[c].sample(channels[c],f).value);
            }
            Check(std::fabs(channels[c].sample(channels[c],f).value-divided[c].sample(divided[c],f).value)<1e-10,
                "PMM preserves full precision timing and new one-frame continuity");
        }
        // An unrelated native value edit must not discard precise timing in
        // the other camera lanes during reconciliation.
        app->CameraKeys()[0].eye[1]+=.5f;
        Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"reconcile unrelated native camera edit");
        Check(channels[0].sample(channels[0],.25).value==divided[0].sample(divided[0],.25).value,
            "unrelated native edits retain full precision subdivision");
        auto truncated=doc;
        for(auto&record:truncated.records)if(record.key=="org.mikudancestudio.camera_controls.v1")record.value.pop_back();
        curve_host::Load(*app,truncated);
        Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error)&&std::isfinite(channels[0].sample(channels[0],.25).value),
            "truncated timing metadata is rejected without invalid sampling");
        curve_host::Reset(app.get());app->CameraKeys()=nullptr;
        pmm_extension::Document empty;curve_host::Capture(*app,empty);
        Check(!pmm_extension::Find(empty,"org.mikudancestudio.camera_controls.v1"),"ordinary project does not acquire empty camera timing extension");
    }
    std::puts("PASS native camera subdivision, linked rotation, smooth speed, one-frame continuity and PMM roundtrip");
}
