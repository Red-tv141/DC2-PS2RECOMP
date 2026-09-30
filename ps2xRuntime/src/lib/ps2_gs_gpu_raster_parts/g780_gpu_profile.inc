#if defined(PS2X_G684_HOT_DIAG)
// Diagnostic-only native GPU stage timestamps. Never wait for an unavailable query.
// 0 geometry start, 1 geometry/compaction end, 2 indirect end,
// 3 raster start, 4 raster end. The 2->3 gap is backend preparation.
static void g780GpuStamp(unsigned stage)
{
    static const bool enabled=std::getenv("DC2_G780_GPU_TS")!=nullptr;
    if(!enabled)return;
    using Gen=void(APIENTRY*)(GLsizei,GLuint*);
    using Counter=void(APIENTRY*)(GLuint,GLenum);
    using Get64=void(APIENTRY*)(GLuint,GLenum,uint64_t*);
    using Get32=void(APIENTRY*)(GLuint,GLenum,GLuint*);
    constexpr unsigned slots=128,marks=5;
    struct State {
        Gen gen=nullptr;Counter counter=nullptr;Get64 get64=nullptr;Get32 get32=nullptr;
        GLuint query[slots][marks]{};uint8_t count[slots]{};
        unsigned cursor=0,active=slots;uint64_t n=0,dropped=0,ns[4]{};
        bool tried=false,ok=false;
    };
    static State s;
    if(!s.tried){
        s.tried=true;
        s.ok=loadProc(s.gen,"glGenQueries") && loadProc(s.counter,"glQueryCounter") &&
             loadProc(s.get64,"glGetQueryObjectui64v") && loadProc(s.get32,"glGetQueryObjectuiv");
        if(s.ok)s.gen(slots*marks,&s.query[0][0]);
        std::fprintf(stderr,"[G780:gpu] timestamps %s\n",s.ok?"loaded":"UNAVAILABLE");
    }
    if(!s.ok || stage>=marks)return;
    if(stage==0){
        const unsigned slot=s.cursor;
        if(s.count[slot]){
            GLuint available=0;
            s.get32(s.query[slot][s.count[slot]-1],GL765_QUERY_RESULT_AVAILABLE,&available);
            if(!available){s.active=slots;++s.dropped;return;}
            if(s.count[slot]==marks){
                uint64_t time[marks]{};
                for(unsigned i=0;i<marks;++i)s.get64(s.query[slot][i],GL765_QUERY_RESULT,&time[i]);
                for(unsigned i=0;i<4;++i)if(time[i+1]>=time[i])s.ns[i]+=time[i+1]-time[i];
                if((++s.n%64u)==0u)
                    std::fprintf(stderr,"[G780:gpu] n=%llu drop=%llu avgMs(geom=%.3f indirect=%.3f prepGap=%.3f raster=%.3f)\n",
                        static_cast<unsigned long long>(s.n),static_cast<unsigned long long>(s.dropped),
                        double(s.ns[0])/1e6/s.n,double(s.ns[1])/1e6/s.n,
                        double(s.ns[2])/1e6/s.n,double(s.ns[3])/1e6/s.n);
            }
            s.count[slot]=0;
        }
        s.active=slot;s.cursor=(slot+1)%slots;
    }
    if(s.active==slots || stage!=s.count[s.active])return;
    s.counter(s.query[s.active][stage],GL765_TIMESTAMP);
    ++s.count[s.active];
    if(stage==marks-1)s.active=slots;
}
#else
static inline void g780GpuStamp(unsigned){}
#endif
