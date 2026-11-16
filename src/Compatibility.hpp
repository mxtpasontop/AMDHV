#ifndef AMDHV_COMPATIBILITY_HPP
#define AMDHV_COMPATIBILITY_HPP
#include <stddef.h>
#include <stdint.h>
#include <CompatibilityInfo.hpp>
namespace Compatibility {
inline bool equalUUID(const unsigned char *a,const unsigned char *b) {
    if (!a || !b) return false;
    unsigned difference=0;
    for(unsigned i=0;i<16;++i) difference|=a[i]^b[i];
    return difference==0;
}
inline uint32_t little32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
// Caller owns a readable image span. Never follows load-command pointers.
inline bool matchesImage(const void *image,size_t size,const unsigned char expected[16]) {
    if (!image || size<32) return false;
    auto bytes=static_cast<const unsigned char *>(image);
    if (little32(bytes)!=0xfeedfacf || little32(bytes+4)!=0x01000007) return false;
    uint32_t count=little32(bytes+16), commandBytes=little32(bytes+20);
    if (commandBytes>size-32 || count>commandBytes/8) return false;
    size_t cursor=32,end=32+commandBytes;
    bool found=false;
    for(uint32_t i=0;i<count;++i) {
        if (end-cursor<8) return false;
        uint32_t type=little32(bytes+cursor),length=little32(bytes+cursor+4);
        if (length<8 || (length&7) || length>end-cursor) return false;
        if (type==0x1b) {
            if(found || length!=24 || !equalUUID(bytes+cursor+8,expected)) return false;
            found=true;
        }
        cursor+=length;
    }
    return found && cursor==end;
}
}
#endif
