#ifndef AMDHV_MEMORY_ACCESS_HPP
#define AMDHV_MEMORY_ACCESS_HPP
#include <stddef.h>
#include <stdint.h>
namespace MemoryAccess {
inline bool canonical(uint64_t a) { return (a>>47)==0 || (a>>47)==0x1ffff; }
inline bool span(uint64_t a,size_t n) {
    return n && n-1<=UINT64_MAX-a && canonical(a) && canonical(a+n-1) && ((a>>47)==((a+n-1)>>47));
}
struct Access {
    void *context {};
    bool (*read)(void *,uint64_t,void *,size_t) {};
    // On failure, write must leave the destination unchanged.
    bool (*write)(void *,uint64_t,const void *,size_t) {};
    bool (*physicalRead)(void *,uint64_t,void *,size_t) {};
    bool load(uint64_t a,void *p,size_t n) const {return p && span(a,n) && read && read(context,a,p,n);}
    bool store(uint64_t a,const void *p,size_t n) const {return p && span(a,n) && write && write(context,a,p,n);}
};
}
#endif
