#pragma once
#include <src/NPTManager.hpp>
#include <src/GuestMemory.hpp>
#include <cstring>
#include <cstdlib>
// Test-owned physical memory. Deliberately rejects fabricated/out-of-range PAs.
struct PhysicalFixture {
    struct Span { uint64_t address; size_t size; };
    Span spans[32] {}, mappings[32] {}; size_t count=0,mappingCount=0; uint64_t ownedOwner=0;bool allowFlush=true; unsigned flushes=0;
    GuestMemory::Registry *guestMemory=nullptr;
    bool limitGPA=false;uint64_t allowedGPA=0;
    void add(void *p,size_t size=4096) { if(count==32) abort();spans[count++]={reinterpret_cast<uint64_t>(p),size}; }
    void allowHPA(uint64_t pa,size_t size=4096) {if(mappingCount==32) abort();mappings[mappingCount++]={pa,size};}
    static bool allowMapping(void *ctx,uint64_t,uint64_t gpa,uint64_t pa,uint64_t n,uint8_t) {
        auto &self=*static_cast<PhysicalFixture *>(ctx);
        if(self.limitGPA && (gpa!=self.allowedGPA || n!=4096)) return false;
        for(size_t i=0;i<self.mappingCount;++i) {auto s=self.mappings[i];if(pa>=s.address && pa-s.address<=s.size && n<=s.size-(pa-s.address)) return true;}
        return false;
    }
    static bool begin(void *,uint64_t) {return true;}
    static void end(void *) {}
    static bool beginOwned(void *ctx,const NPTManager::SpaceKey &key) {return key.owner()==static_cast<PhysicalFixture *>(ctx)->ownedOwner;}
    static bool canRetire(void *ctx,const NPTManager::SpaceKey &key) {
        auto &s=*static_cast<PhysicalFixture *>(ctx);return !s.guestMemory || s.guestMemory->canRetire(key);
    }
    static bool read(void *ctx,uint64_t pa,void *out,size_t n) {
        auto &self=*static_cast<PhysicalFixture *>(ctx);
        for(size_t i=0;i<self.count;++i) {
            auto s=self.spans[i];
            if(pa>=s.address && pa-s.address<=s.size && n<=s.size-(pa-s.address)) {
                memcpy(out,reinterpret_cast<const void *>(pa),n);return true;
            }
        }
        return false;
    }
    static bool flush(void *ctx,uint64_t,uint64_t,const uint64_t[2]) {
        auto &self=*static_cast<PhysicalFixture *>(ctx);++self.flushes;return self.allowFlush;
    }
    bool install() {return NPTManager::configure({this,begin,read,end,allowMapping,beginOwned,canRetire},{this,flush});}
};
