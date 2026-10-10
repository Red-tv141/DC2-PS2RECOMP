// Included inside dc2_g775_capture. Diagnostic provenance only; no render changes.
inline bool instanceContext()
{
#if defined(PS2X_G775_DIAG)
    static const bool value=std::getenv("DC2_G776_CONTEXT")!=nullptr; return value || dc2_g778::requested();
#else
    return dc2_g778::requested();
#endif
}
struct ListOwner { Draw draw; uint32_t state=0,geometry=0; };
struct CallEdge { ListOwner owner; uint32_t node=0; };
inline std::unordered_map<uint32_t,ListOwner> listOwners;
inline std::unordered_map<uint32_t,CallEdge> callEdges;
inline std::unordered_map<uint32_t,Draw> stateOwners;
inline std::map<std::pair<uint32_t,uint32_t>,Draw> stateGeometryOwners;
inline uint32_t listFrame=0;
inline thread_local bool managerScope=false;
inline uint32_t readWord(const uint8_t* ram,uint32_t address)
{ uint32_t value=0;if(valid(address,4))std::memcpy(&value,ram+address,4);return value; }
inline uint32_t exactBucket(Draw draw,uint32_t geometry,const uint8_t* ram)
{
    uint32_t bucket=readWord(ram,draw.self+0x48)&0x1fffffffu,match=0;
    for(uint32_t i=0;i<256 && valid(bucket,0x20);++i)
    {
        if((readWord(ram,bucket+0x10)&0x1fffffffu)==(geometry&0x1fffffffu))
        {if(match)return 0;match=bucket;}
        bucket=readWord(ram,bucket+8)&0x1fffffffu;
    }
    return match;
}
inline void rememberList(uint32_t node,uint32_t state,uint32_t geometry,Draw draw)
{
    if(!instanceContext() || !draw.id || !valid(node,16))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    if(listFrame!=draw.sf){listOwners.clear();callEdges.clear();stateOwners.clear();stateGeometryOwners.clear();listFrame=draw.sf;}
    // Allocated node, not reusable mesh identity, owns this state/geometry pair.
    const ListOwner value{draw,state&0x1fffffffu,geometry&0x1fffffffu};
    const auto it=listOwners.find(node);
    if(it!=listOwners.end() && it->second.draw.id!=draw.id)
    { listOwners[node]={}; return; }
    listOwners[node]=value;
}
inline void rememberState(uint32_t state,Draw draw)
{
    if(!instanceContext() || !draw.id || !valid(state,16))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    const auto it=stateOwners.find(state);
    if(it!=stateOwners.end() && it->second.id!=draw.id){stateOwners[state]={};return;}
    stateOwners[state]=draw;
}
inline void rememberStateGeometry(uint32_t state,uint32_t geometry,Draw draw)
{
    if(!instanceContext() || !draw.id || !valid(state,16) || !valid(geometry,16))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    const auto key=std::make_pair(state,geometry);
    const auto it=stateGeometryOwners.find(key);
    if(it!=stateGeometryOwners.end() && it->second.id!=draw.id){stateGeometryOwners[key]={};return;}
    stateGeometryOwners[key]=draw;
}
inline void rememberCall(uint32_t call,uint32_t node,const uint8_t* ram)
{
    if(!instanceContext() || !valid(call,16) || !valid(node,16))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    const auto it=listOwners.find(node);
    if(it==listOwners.end() || !it->second.draw.id)return;
    const auto& owner=it->second;
    if((readWord(ram,node)&0x1fffffffu)!=owner.state ||
       (readWord(ram,node+4)&0x1fffffffu)!=owner.geometry)return;
    const CallEdge value{owner,node};
    const auto edge=callEdges.find(call);
    if(edge!=callEdges.end() && edge->second.owner.draw.id!=owner.draw.id)
    {callEdges[call]={};return;}
    callEdges[call]=value;
}
inline void rememberDirectChain(uint32_t begin,size_t bytes,Draw draw,const uint8_t* ram)
{
    if(!instanceContext() || !draw.id || !valid(begin,bytes) || bytes>0x10000)return;
    std::vector<uint32_t> geometry;
    uint32_t bucket=readWord(ram,draw.self+0x48)&0x1fffffffu;
    for(uint32_t i=0;i<256 && valid(bucket,0x20);++i)
    {geometry.push_back(readWord(ram,bucket+0x10)&0x1fffffffu);bucket=readWord(ram,bucket+8)&0x1fffffffu;}
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    for(size_t offset=0;offset+16<=bytes;offset+=16)
    {
        const uint32_t call=begin+uint32_t(offset),tag=readWord(ram,call),target=readWord(ram,call+4)&0x1fffffffu;
        if(tag!=0x50000000u || std::find(geometry.begin(),geometry.end(),target)==geometry.end())continue;
        Draw owned=draw;owned.bucket=exactBucket(draw,target,ram);
        const CallEdge value{{owned,0,target},0};
        const auto it=callEdges.find(call);
        if(it!=callEdges.end() && it->second.owner.draw.id!=draw.id){callEdges[call]={};continue;}
        callEdges[call]=value;
    }
}
struct CallContext
{
    Draw active{},current{};
    uint32_t state=0;
    struct Parent {Draw active;uint32_t state;};
    std::vector<Parent> parents;
    void tag(uint32_t address,uint32_t id,uint32_t qwc,uint32_t target,bool scratch)
    {
        if(!instanceContext() || dc2_g783::off(dc2_g783::OffChain))return;
        dc2_g783::Scope g783(dc2_g783::ChainTag);
        current=active;
        if(id==5)
        {
            std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
            target&=0x1fffffffu;
            const auto setup=stateOwners.find(target);
            if(!qwc && setup!=stateOwners.end())
            {
                state=setup->second.id?target:0;
                current=setup->second;
                current.geometry=0;current.listNode=0;
                if(!current.id)current.reason=4;
            }
            else if(!qwc && state)
            {
                const auto pair=stateGeometryOwners.find({state,target});
                if(pair!=stateGeometryOwners.end())
                {
                    current=pair->second;
                    current.geometry=target;current.listNode=0;
                    if(!current.id)current.reason=4;
                }
            }
            const auto edge=scratch?callEdges.end():callEdges.find(address);
            if(edge!=callEdges.end())
            {
                const auto draw=edge->second.owner.draw;
                if(!draw.id || qwc || (target&0x1fffffffu)!=edge->second.owner.geometry)
                {current={};current.reason=4;}
                else if(active.id && active.id!=draw.id){current={};current.reason=5;}
                else {current=draw;current.geometry=target;current.listNode=edge->second.node;}
            }
        }
        // A bucket may contain nested CALLs. Preserve the independently proven
        // outer ordering unit through those children, only for this invocation.
        if(current.id && current.id==active.id && active.bucket)
        {current.bucket=active.bucket;current.geometry=active.geometry;current.listNode=active.listNode;current.state=active.state;current.call=active.call;}
        else if(id==5 && current.id && current.geometry){current.state=current.state?current.state:state;current.call=address;}
        if(id==5){parents.push_back({active,state});active=current;state=0;}
        else if(id==6){active=parents.empty()?Draw{}:parents.back().active;state=parents.empty()?0:parents.back().state;if(!parents.empty())parents.pop_back();}
        else if(id==0 || id==7){active={};state=0;parents.clear();}
    }
};
inline void recordInstance(Draw draw,uint32_t caller,uint32_t destination,const uint8_t* ram)
{
    if(!instanceContext())return;
    {
        std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
        // Direct-chain draws precede manager-list draws. Epoch retirement must
        // happen at the first invocation, not the first later AddPacket call.
        if(listFrame!=draw.sf){listOwners.clear();callEdges.clear();stateOwners.clear();stateGeometryOwners.clear();listFrame=draw.sf;}
    }
    if(!enabled())return;
    static FILE* file=[] { FILE* f=std::fopen("logs/g775/draw_instances.csv","wb");if(f)std::fprintf(f,"draw,sf,self,vtable,parent,world,manager,destination,caller,world_hash\n");return f; }();
    if(file)
    {
        uint64_t worldHash=14695981039346656037ull;
        if(valid(draw.world,64))for(uint32_t i=0;i<64;++i)worldHash=(worldHash^ram[draw.world+i])*1099511628211ull;
        std::fprintf(file,"%llu,%u,%x,%x,%x,%x,%x,%x,%x,%llx\n",static_cast<unsigned long long>(draw.id),draw.sf,draw.self,draw.vtable,draw.parent,draw.world,draw.manager,destination,caller,static_cast<unsigned long long>(worldHash));
        if(!(draw.id%1024))std::fflush(file);
    }
}
