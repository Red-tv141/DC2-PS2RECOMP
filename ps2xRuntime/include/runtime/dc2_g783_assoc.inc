// Included inside dc2_g775_capture after dc2_g776_attribution.inc.
// G783: target-scoped (narrow) E91F00 association -- the shipping association contract.
//
// The G775/G776 machinery records every mesh draw's state packet, (state, geometry) pairs,
// AddPacket list nodes and DMA CALL edges, then resolves every DMA CALL of every VIF1 chain
// against those maps under one mutex and tags every chain chunk with an origin. Only draws of
// the supported mesh (self 0xe8b940) can ever produce a native association, so the narrow
// contract keeps the IDENTICAL resolution algorithm for target entries and replaces every
// non-target entry by a conservative collision mark:
//   * target keys (state, (state, geometry), list node, call edge) live in small frame tables;
//   * a non-target draw's state / node / call keys set a lock-free bloom bit (for its direct
//     chain: every CALL tag in it, a superset of the broad registration). A target key that a
//     non-target also used (in either
//     order) is POISONED, exactly where the broad maps poison it (and on bloom false
//     positives, which only cost coverage);
//   * a DMA CALL into a non-target key resolves to a NonTarget sentinel draw: never a target,
//     and it refuses nested target edges (reason 5) where the broad map may only have reason 4;
//   * a chain that starts while no target entry exists is inert (no lookups, no origins);
//   * only target chunks get an origin; a VIF position outside them reads Draw{} where the broad
//     path read some non-target draw. Both are rejected by dc2_g778::find and both close an
//     active VifScope, so the native path cannot tell them apart.
// Narrow may refuse where broad accepts (coverage); it never accepts where broad refuses or
// differs. DC2_G783_DIFF=1 runs both and counts the two directions (must be 0 for the second).
constexpr uint32_t kG783TargetSelf=0xe8b940u;
constexpr uint64_t kG783NonTargetId=~0ull;
inline bool g783BroadForced(){static const bool on=std::getenv("DC2_G783_BROAD")!=nullptr;return on;}
inline bool g783DiffOn(){static const bool on=std::getenv("DC2_G783_DIFF")!=nullptr;return on;}
// Narrow runs whenever native tracking is requested outside a G775 census.
inline bool narrowOn(){static const bool on=tracking() && !enabled() && !g783BroadForced();return on;}
// Broad runs for the census, the explicit rollback, and the differential.
inline bool broadOn(){static const bool on=tracking() && (!narrowOn() || g783DiffOn());return on;}
inline bool narrowDrives(){static const bool on=narrowOn() && !g783DiffOn();return on;}
inline Draw g783NonTarget(){Draw d;d.id=kG783NonTargetId;d.reason=6;return d;}
inline bool g783IsTarget(const Draw& d){return d.id && d.id!=kG783NonTargetId && d.self==kG783TargetSelf;}

namespace g783 {
enum Key : uint32_t { KeyState=1u, KeyNode=2u, KeyCall=3u };
inline std::array<std::atomic<uint64_t>,1024> bloom{}; // 64 Kbit, k=2, per key kind
inline uint32_t bloomHash(uint32_t kind,uint32_t address,uint32_t salt)
{ uint32_t h=(address*0x9E3779B1u)^(kind*0x85EBCA77u)^salt;h^=h>>15;h*=0x2C1B3C6Du;h^=h>>12;return h&0xFFFFu; }
inline void bloomMark(uint32_t kind,uint32_t address)
{
    for(uint32_t salt:{0u,0x5bd1e995u}){const uint32_t b=bloomHash(kind,address,salt);
        auto& w=bloom[b>>6];const uint64_t bit=1ull<<(b&63u);
        if(!(w.load(std::memory_order_relaxed)&bit))w.fetch_or(bit,std::memory_order_relaxed);}
}
inline bool bloomTest(uint32_t kind,uint32_t address)
{
    for(uint32_t salt:{0u,0x5bd1e995u}){const uint32_t b=bloomHash(kind,address,salt);
        if(!(bloom[b>>6].load(std::memory_order_relaxed)&(1ull<<(b&63u))))return false;}
    return true;
}
struct StateEntry { uint32_t state=0; Draw draw; };
struct PairEntry { uint32_t state=0,geometry=0; Draw draw; };
struct ListEntry { uint32_t node=0; ListOwner owner; };
struct CallEntry { uint32_t call=0; CallEdge edge; };
// Frame tables. Written by the EE probes under `mutex`, read by DMA chains under `mutex`.
inline std::vector<StateEntry> states;
inline std::vector<PairEntry> pairs;
inline std::vector<ListEntry> lists;
inline std::vector<CallEntry> calls;
inline uint32_t frame=0;
inline std::atomic<uint32_t> targetEntries{0}; // target keys this frame; 0 => DMA chains inert
// Lock-free prefilter of target keys (state address, pair geometry, call address): a DMA CALL
// whose target and tag address both miss it cannot hit a target table.
inline std::array<std::atomic<uint64_t>,16> targetKeys{};
inline uint32_t keyBit(uint32_t address){uint32_t h=address*0x9E3779B1u;return h>>22;}
inline void markTargetKey(uint32_t address)
{auto& w=targetKeys[keyBit(address)>>6];w.fetch_or(1ull<<(keyBit(address)&63u),std::memory_order_relaxed);}
inline bool mayBeTargetKey(uint32_t address)
{return (targetKeys[keyBit(address)>>6].load(std::memory_order_relaxed)>>(keyBit(address)&63u))&1u;}
inline std::atomic<uint32_t> published{0};     // narrow streams awaiting their VIF input
inline std::unordered_map<const uint8_t*,std::shared_ptr<std::vector<Origin>>> streams;
inline thread_local Draw vifNarrow{};
inline thread_local std::shared_ptr<std::vector<Origin>> inputOrigins;
inline thread_local size_t originIndex=0;
inline thread_local size_t nextChange=0; // VIF position before which vifDraw cannot change
struct Diff { std::atomic<uint64_t> agree{0},narrowOnly{0},mismatch{0},broadOnly{0},hookBroad{0},hookNarrow{0},hookBoth{0};
    // Where a broad-only hook went in the narrow attribution.
    std::atomic<uint64_t> missNoStream{0},missGap{0},missNonTarget{0},missOther{0};
    // Target keys poisoned: at registration by an earlier foreign mark / later by a foreign key /
    // by a second target invocation. Index = Key kind.
    std::atomic<uint64_t> poisonAtRegister[4]{},poisonLater[4]{},poisonTarget[4]{};
    std::atomic<uint64_t> sentinelState{0},sentinelCall{0}; };
inline Diff diff;

template<class T,class F> inline T* findIn(std::vector<T>& v,F&& match)
{ for(auto& e:v)if(match(e))return &e; return nullptr; }
// EE thread only (every writer of `frame` is an EE probe), mirrors the broad per-frame clear.
inline void beginFrame(uint32_t sf)
{
    if(frame==sf)return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    if(frame==sf)return;
    states.clear();pairs.clear();lists.clear();calls.clear();
    for(auto& w:bloom)w.store(0,std::memory_order_relaxed);
    for(auto& w:targetKeys)w.store(0,std::memory_order_relaxed);
    targetEntries.store(0,std::memory_order_relaxed);
    frame=sf;
}
// Non-target key: mark, then poison a target entry already holding it.
inline void foreignKey(uint32_t kind,uint32_t address)
{
    bloomMark(kind,address);
    // Every target key is in the prefilter, so a miss there cannot poison anything.
    if(!targetEntries.load(std::memory_order_relaxed) || !mayBeTargetKey(address))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    bool hit=false;
    if(kind==KeyState){if(auto* e=findIn(states,[&](const StateEntry& s){return s.state==address;})){hit=e->draw.id!=0;e->draw={};}}
    else if(kind==KeyNode){if(auto* e=findIn(lists,[&](const ListEntry& l){return l.node==address;})){hit=e->owner.draw.id!=0;e->owner={};}}
    else if(auto* e=findIn(calls,[&](const CallEntry& c){return c.call==address;})){hit=e->edge.owner.draw.id!=0;e->edge={};}
    if(hit)++diff.poisonLater[kind];
}
inline void rememberState(uint32_t state,const Draw& draw)
{
    if(!draw.id || !valid(state,16))return;
    if(!g783IsTarget(draw)){foreignKey(KeyState,state);return;}
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    if(auto* e=findIn(states,[&](const StateEntry& s){return s.state==state;}))
    {if(e->draw.id!=draw.id){if(e->draw.id)++diff.poisonTarget[KeyState];e->draw={};}return;}
    const bool foreign=bloomTest(KeyState,state);if(foreign)++diff.poisonAtRegister[KeyState];
    states.push_back({state,foreign?Draw{}:draw});
    markTargetKey(state);
    targetEntries.fetch_add(1u,std::memory_order_relaxed);
}
// Target only: a (state, geometry) collision implies a state collision (both draws registered
// the state packet through CreateRenderInfoPacket), which already poisons the state entry.
inline void rememberStateGeometry(uint32_t state,uint32_t geometry,const Draw& draw)
{
    if(!g783IsTarget(draw) || !valid(state,16) || !valid(geometry,16))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    if(auto* e=findIn(pairs,[&](const PairEntry& p){return p.state==state && p.geometry==geometry;}))
    {if(e->draw.id!=draw.id)e->draw={};return;}
    pairs.push_back({state,geometry,draw});
    markTargetKey(geometry);
    targetEntries.fetch_add(1u,std::memory_order_relaxed);
}
inline void rememberList(uint32_t node,uint32_t state,uint32_t geometry,const Draw& draw)
{
    if(!draw.id || !valid(node,16))return;
    beginFrame(draw.sf);
    if(!g783IsTarget(draw)){foreignKey(KeyNode,node);return;}
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    const ListOwner value{draw,state&0x1fffffffu,geometry&0x1fffffffu};
    if(auto* e=findIn(lists,[&](const ListEntry& l){return l.node==node;}))
    {if(e->owner.draw.id!=draw.id){if(e->owner.draw.id)++diff.poisonTarget[KeyNode];e->owner={};}else e->owner=value;return;}
    const bool foreign=bloomTest(KeyNode,node);if(foreign)++diff.poisonAtRegister[KeyNode];
    lists.push_back({node,foreign?ListOwner{}:value});
    markTargetKey(node);
    targetEntries.fetch_add(1u,std::memory_order_relaxed);
}
inline void registerCall(uint32_t call,const CallEdge& value)
{
    // Caller holds `mutex`.
    if(auto* e=findIn(calls,[&](const CallEntry& c){return c.call==call;}))
    {if(e->edge.owner.draw.id!=value.owner.draw.id){if(e->edge.owner.draw.id)++diff.poisonTarget[KeyCall];e->edge={};}return;}
    const bool foreign=bloomTest(KeyCall,call);if(foreign)++diff.poisonAtRegister[KeyCall];
    calls.push_back({call,foreign?CallEdge{}:value});
    markTargetKey(call);
    targetEntries.fetch_add(1u,std::memory_order_relaxed);
}
inline void rememberCall(uint32_t call,uint32_t node,const uint8_t* ram)
{
    if(!valid(call,16) || !valid(node,16))return;
    if(targetEntries.load(std::memory_order_relaxed))
    {
        std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
        if(const auto* l=findIn(lists,[&](const ListEntry& e){return e.node==node;}))
        {
            // Target node: the broad rememberCall, verbatim.
            const auto& owner=l->owner;
            if(!owner.draw.id)return;
            if((readWord(ram,node)&0x1fffffffu)!=owner.state ||
               (readWord(ram,node+4)&0x1fffffffu)!=owner.geometry)return;
            registerCall(call,CallEdge{owner,node});
            return;
        }
    }
    // Broad records a foreign edge only for a node some other invocation owns; an unowned node
    // (AddPacket outside any mesh draw) records nothing and the CALL still resolves through the
    // (state, geometry) pair. Mark only nodes a non-target registered (node bloom: superset).
    if(bloomTest(KeyNode,node))foreignKey(KeyCall,call);
}
inline void rememberDirectChain(uint32_t begin,size_t bytes,const Draw& draw,const uint8_t* ram)
{
    if(!draw.id || !valid(begin,bytes) || bytes>0x10000)return;
    // Exactly the broad registration set: CALL tags in the chain into the draw's own geometry.
    thread_local std::vector<uint32_t> geometry;geometry.clear();
    uint32_t bucket=readWord(ram,draw.self+0x48)&0x1fffffffu;
    for(uint32_t i=0;i<256 && valid(bucket,0x20);++i)
    {geometry.push_back(readWord(ram,bucket+0x10)&0x1fffffffu);bucket=readWord(ram,bucket+8)&0x1fffffffu;}
    if(!g783IsTarget(draw))
    {
        for(size_t offset=0;offset+16<=bytes;offset+=16)
        {
            const uint32_t call=begin+uint32_t(offset);
            if(readWord(ram,call)==0x50000000u &&
               std::find(geometry.begin(),geometry.end(),readWord(ram,call+4)&0x1fffffffu)!=geometry.end())
                foreignKey(KeyCall,call);
        }
        return;
    }
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    for(size_t offset=0;offset+16<=bytes;offset+=16)
    {
        const uint32_t call=begin+uint32_t(offset),tag=readWord(ram,call),target=readWord(ram,call+4)&0x1fffffffu;
        if(tag!=0x50000000u || std::find(geometry.begin(),geometry.end(),target)==geometry.end())continue;
        Draw owned=draw;owned.bucket=exactBucket(draw,target,ram);
        registerCall(call,CallEdge{{owned,0,target},0});
    }
}
// CallContext::tag with the narrow lookups. Everything after the lookups is the broad code.
struct Context
{
    Draw active{},current{};
    uint32_t state=0;
    static constexpr uint32_t kForeignState=0xffffffffu;
    struct Parent {Draw active;uint32_t state;};
    std::vector<Parent> parents;
    void tag(uint32_t address,uint32_t id,uint32_t qwc,uint32_t target,bool scratch)
    {
        current=active;
        if(id==5)
        {
            target&=0x1fffffffu;
            const bool mayHit=mayBeTargetKey(target) || (!scratch && mayBeTargetKey(address));
            std::unique_lock<dc2_g783::ProfMutex> lock(mutex,std::defer_lock);
            if(mayHit)lock.lock();
            const auto* setup=mayHit?findIn(states,[&](const StateEntry& s){return s.state==target;}):nullptr;
            if(!qwc && setup)
            {
                state=setup->draw.id?target:0;
                current=setup->draw;
                current.geometry=0;current.listNode=0;
                if(!current.id)current.reason=4;
            }
            else if(!qwc && bloomTest(KeyState,target))
            {
                // Broad: a non-target (or poisoned) state packet. Never a target pairing.
                state=kForeignState;current=g783NonTarget();
                if(g783DiffOn())++diff.sentinelState;
            }
            else if(!qwc && state==kForeignState)current=g783NonTarget();
            else if(!qwc && state)
            {
                const auto* pair=mayHit?findIn(pairs,[&](const PairEntry& p){return p.state==state && p.geometry==target;}):nullptr;
                if(pair)
                {
                    current=pair->draw;
                    current.geometry=target;current.listNode=0;
                    if(!current.id)current.reason=4;
                }
            }
            const auto* edge=(scratch || !mayHit)?nullptr:findIn(calls,[&](const CallEntry& c){return c.call==address;});
            if(edge)
            {
                const auto draw=edge->edge.owner.draw;
                if(!draw.id || qwc || (target&0x1fffffffu)!=edge->edge.owner.geometry)
                {current={};current.reason=4;}
                else if(active.id && active.id!=draw.id){current={};current.reason=5;}
                else {current=draw;current.geometry=target;current.listNode=edge->edge.node;}
            }
            else if(!scratch && bloomTest(KeyCall,address))
            {current=g783NonTarget();if(g783DiffOn())++diff.sentinelCall;}
        }
        if(current.id && current.id==active.id && active.bucket)
        {current.bucket=active.bucket;current.geometry=active.geometry;current.listNode=active.listNode;current.state=active.state;current.call=active.call;}
        else if(id==5 && current.id && current.geometry){current.state=current.state?current.state:state;current.call=address;}
        if(id==5){parents.push_back({active,state});active=current;state=0;}
        else if(id==6){active=parents.empty()?Draw{}:parents.back().active;state=parents.empty()?0:parents.back().state;if(!parents.empty())parents.pop_back();}
        else if(id==0 || id==7){active={};state=0;parents.clear();}
    }
};
inline bool sameAssociation(const Draw& a,const Draw& b)
{
    return a.id==b.id && a.self==b.self && a.bucket==b.bucket && a.geometry==b.geometry &&
           a.state==b.state && a.call==b.call && a.listNode==b.listNode;
}
// DC2_G783_DIFF, at every E91F00 hook command: which attribution would have offered the hook.
inline void noteHook(const Draw& broad)
{
    // G784: every E8B940 FixMDT bucket in the contract table is a potential native target.
    const bool b=broad.id && broad.self==kG783TargetSelf && dc2_g784::indexOf(broad.bucket)>=0;
    const bool n=g783IsTarget(vifNarrow) && dc2_g784::indexOf(vifNarrow.bucket)>=0;
    if(b && n){if(sameAssociation(broad,vifNarrow))++diff.hookBoth;else ++diff.mismatch;}
    else if(b)
    {
        ++diff.hookBroad;
        if(!inputOrigins)++diff.missNoStream;
        else if(!vifNarrow.id)++diff.missGap;
        else if(vifNarrow.id==kG783NonTargetId)++diff.missNonTarget;
        else ++diff.missOther;
    }
    else if(n)++diff.hookNarrow;
}
inline void report(uint32_t sf)
{
    std::fprintf(stderr,"[G783:diff] sf=%u agree=%llu narrowOnly=%llu mismatch=%llu broadOnly=%llu hookBoth=%llu hookBroadOnly=%llu hookNarrowOnly=%llu\n",sf,
        (unsigned long long)diff.agree.load(),(unsigned long long)diff.narrowOnly.load(),
        (unsigned long long)diff.mismatch.load(),(unsigned long long)diff.broadOnly.load(),
        (unsigned long long)diff.hookBoth.load(),(unsigned long long)diff.hookBroad.load(),
        (unsigned long long)diff.hookNarrow.load());
    std::fprintf(stderr,"[G783:diff-why] sf=%u noStream=%llu gap=%llu nonTarget=%llu other=%llu sentinelState=%llu sentinelCall=%llu poisonReg=%llu/%llu/%llu poisonLater=%llu/%llu/%llu poisonTarget=%llu/%llu/%llu\n",sf,
        (unsigned long long)diff.missNoStream.load(),(unsigned long long)diff.missGap.load(),
        (unsigned long long)diff.missNonTarget.load(),(unsigned long long)diff.missOther.load(),
        (unsigned long long)diff.sentinelState.load(),(unsigned long long)diff.sentinelCall.load(),
        (unsigned long long)diff.poisonAtRegister[1].load(),(unsigned long long)diff.poisonAtRegister[2].load(),(unsigned long long)diff.poisonAtRegister[3].load(),
        (unsigned long long)diff.poisonLater[1].load(),(unsigned long long)diff.poisonLater[2].load(),(unsigned long long)diff.poisonLater[3].load(),
        (unsigned long long)diff.poisonTarget[1].load(),(unsigned long long)diff.poisonTarget[2].load(),(unsigned long long)diff.poisonTarget[3].load());
}
}
