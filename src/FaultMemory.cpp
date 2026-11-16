#include <src/FaultMemory.hpp>
namespace FaultMemory {
namespace {
class Scope {
    const Context &c_;
    bool disabled_ {},previous_ {};
    TrapRecovery::Record record_;
    TrapRecovery::Registry::Scope armed_;
public:
    explicit Scope(const Context &c):c_(c) {
        if(!c_.cpu || !c_.thread || !c_.disableInterrupts || !c_.restoreInterrupts ||
           !c_.probeInstalled || !c_.probeInstalled(c_.context)) return;
        previous_=c_.disableInterrupts(c_.context);disabled_=true;
        constexpr uint32_t faults=(1U<<13)|(1U<<14);
        static const TrapRecovery::Site sites[]={
            {reinterpret_cast<uint64_t>(amdhv_probe_read8_fault),reinterpret_cast<uint64_t>(amdhv_probe_read8_recover),faults},
            {reinterpret_cast<uint64_t>(amdhv_probe_write8_fault),reinterpret_cast<uint64_t>(amdhv_probe_write8_recover),faults},
            {reinterpret_cast<uint64_t>(amdhv_probe_write64_fault),reinterpret_cast<uint64_t>(amdhv_probe_write64_recover),faults}};
        record_.owner=c_.thread(c_.context);record_.sites=sites;record_.count=3;
        armed_=TrapRecovery::registry().arm(c_.cpu(c_.context),record_);
    }
    ~Scope() {armed_.reset();if(disabled_) c_.restoreInterrupts(c_.context,previous_);}
    explicit operator bool() const {return bool(armed_);}
};
}
bool Adapter::read(uint64_t address,void *out,size_t size) {
    if(!out || !size || size>4096 || !MemoryAccess::span(address,size) ||
       !context_.owns || !context_.owns(context_.context,address,size,false)) return false;
    Scope scope(context_);if(!scope) return false;
    auto bytes=static_cast<uint8_t *>(out);
    for(size_t i=0;i<size;++i) if(!amdhv_probe_read8(reinterpret_cast<const void *>(address+i),bytes+i)) return false;
    return true;
}
bool Adapter::write(uint64_t address,const void *in,size_t size) {
    if(!in || (size!=1 && size!=8) || (size==8 && (address&7)) || !MemoryAccess::span(address,size) ||
       !context_.owns || !context_.owns(context_.context,address,size,true)) return false;
    Scope scope(context_);if(!scope) return false;
    return size==1?amdhv_probe_write8(reinterpret_cast<void *>(address),in):amdhv_probe_write64(reinterpret_cast<void *>(address),in);
}
MemoryAccess::Access Adapter::access() {
    return {this,
        [](void *c,uint64_t a,void *out,size_t n) {return static_cast<Adapter *>(c)->read(a,out,n);},
        [](void *c,uint64_t a,const void *in,size_t n) {return static_cast<Adapter *>(c)->write(a,in,n);},nullptr};
}
}
