// Separate PMM tail record. MMSC v1 and standard PMM/VMD formats stay intact.
constexpr char preservedCameraRecord[]="org.mikumikustudio.camera_intervals.v1";
using PreservedCameraAddress=std::tuple<int,int,int>;
using PreservedCameraTable=std::map<PreservedCameraAddress,std::vector<PreservedCameraInterval>>;
struct CameraIntervalWriter {
    std::vector<std::uint8_t> bytes;
    void U32(std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(std::uint8_t(value>>(8*i)));}
    void F64(double value){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof bits);U32(std::uint32_t(bits));U32(std::uint32_t(bits>>32));}
};
std::vector<std::uint8_t> EncodePreservedCamera(const PreservedCameraTable& table) {
    CameraIntervalWriter out;out.U32(0x4953434d);out.U32(1);out.U32(std::uint32_t(table.size()));
    for(const auto& entry:table) {
        if(entry.second.size()>UINT32_MAX)throw std::invalid_argument("native camera interval count");
        out.U32(std::get<0>(entry.first));out.U32(std::get<1>(entry.first));out.U32(std::get<2>(entry.first));out.U32(std::uint32_t(entry.second.size()));
        for(const auto& value:entry.second) {
            out.U32(value.first);out.U32(value.last);out.U32(value.sourceFirst);out.U32(value.sourceLast);
            out.F64(value.sampleFirst);out.F64(value.sampleLast);out.F64(value.sourceValueFirst);out.F64(value.sourceValueLast);
            for(double component:value.controls)out.F64(component);
            for(double component:value.geometry)out.F64(component);
        }
    }
    return std::move(out.bytes);
}
bool DecodePreservedCamera(const std::vector<std::uint8_t>& bytes,PreservedCameraTable& result) {
    try {
        std::size_t position=0;
        const auto u32=[&](){if(bytes.size()-position<4)throw std::invalid_argument("truncated native camera interval");
            std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(bytes[position++])<<(8*i);return value;};
        const auto f64=[&](){const auto low=u32(),high=u32();const std::uint64_t bits=low|(std::uint64_t(high)<<32);
            double value;std::memcpy(&value,&bits,sizeof value);return value;};
        if(u32()!=0x4953434d||u32()!=1)return false;
        const auto count=u32();if(count>(bytes.size()-position)/16)return false;
        PreservedCameraTable table;
        for(std::uint32_t i=0;i<count;++i) {
            const auto kind=u32(),slot=u32(),channel=u32(),size=u32();
            if(kind!=std::uint32_t(Kind::Camera)||slot!=0||channel>=8||!size||size>(bytes.size()-position)/128)return false;
            auto inserted=table.try_emplace(PreservedCameraAddress{int(kind),int(slot),int(channel)});
            if(!inserted.second)return false;
            auto& values=inserted.first->second;values.reserve(size);
            for(std::uint32_t j=0;j<size;++j) {
                PreservedCameraInterval value;value.first=int(u32());value.last=int(u32());value.sourceFirst=int(u32());value.sourceLast=int(u32());
                value.sampleFirst=f64();value.sampleLast=f64();value.sourceValueFirst=f64();value.sourceValueLast=f64();
                for(double& component:value.controls)component=f64();for(double& component:value.geometry)component=f64();
                if(!ValidPreservedData(value))return false;values.push_back(value);
            }
            std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.first<b.first;});
            for(std::size_t j=1;j<values.size();++j)if(values[j].first<values[j-1].last)return false;
        }
        if(position!=bytes.size())return false;result.swap(table);return true;
    }catch(const std::exception&){return false;}
}
