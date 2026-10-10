#pragma once
#include <cstdlib>
#include <cstring>
namespace dc2_g778 {
inline bool shadowRequested(){static const bool on=std::getenv("DC2_G778_SHADOW_RASTER")!=nullptr;return on;}
inline bool nativeRequested(){
#if defined(PS2X_G777_NATIVE_GL)
    static const bool on=!std::getenv("DC2_G778_NO_NATIVE_E91F00") && !shadowRequested();return on;
#else
    return false;
#endif
}
inline bool nonblockingRequested(){
#if defined(PS2X_G777_NATIVE_GL)
    // G791 release: DEFAULT ON (the G788-AA setting); rollback DC2_G782_NONBLOCKING=0.
    static const char* e=std::getenv("DC2_G782_NONBLOCKING");
    static const bool on=nativeRequested() && (!e || (*e && std::strcmp(e,"0")!=0));
    return on;
#else
    return false;
#endif
}
// G782 timing DECOMPOSITION knobs (never a shipping configuration):
//   DC2_G782_NO_MARKER  the EE rejects the token itself: no GIF drain, no G150 marker.
//   DC2_G782_NO_DRAIN   the G150 marker is enqueued without closing the open GIF window
//                       first. Unsafe for native correctness (stale GS state); timing only.
inline bool g782NoMarker(){static const bool on=std::getenv("DC2_G782_NO_MARKER")!=nullptr;return on;}
inline bool g782EndBracket(){static const bool on=std::getenv("DC2_G782_END_BRACKET")!=nullptr;return on;} // g782_f behaviour
inline bool g782NoDrain(){static const bool on=std::getenv("DC2_G782_NO_DRAIN")!=nullptr;return on;}
inline bool statsRequested(){static const bool on=std::getenv("DC2_G778_STAT")!=nullptr;return on;}
inline bool requested(){return shadowRequested() || nativeRequested() || statsRequested();}
inline bool shadowSelected(unsigned frame){
    static const unsigned every=[](){const char* p=std::getenv("DC2_G778_SHADOW_EVERY");return p?unsigned(std::strtoul(p,nullptr,0)):30u;}();
    static const unsigned lo=[](){const char* p=std::getenv("DC2_G778_SHADOW_SF_LO");return p?unsigned(std::strtoul(p,nullptr,0)):0u;}();
    static const unsigned hi=[](){const char* p=std::getenv("DC2_G778_SHADOW_SF_HI");return p?unsigned(std::strtoul(p,nullptr,0)):3000u;}();
    return shadowRequested() && frame>=lo && frame<=hi && (!every || frame%every==0);
}
}
