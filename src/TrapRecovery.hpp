#ifndef AMDHV_TRAP_RECOVERY_HPP
#define AMDHV_TRAP_RECOVERY_HPP
#include <src/IntelVMX.hpp>
#include <src/Runtime.hpp>
namespace TrapRecovery {
constexpr uint32_t MaxCPUs=128;
struct Site {uint64_t fault,recover;uint32_t traps;};
struct Record {uintptr_t owner {};const Site *sites {};size_t count {};uint32_t faults {};};
// Caller disables interrupts/preemption before arming; the record is on its live
// stack. Only explicitly whitelisted instruction addresses can be recovered.
class Registry {
    Record *active_[MaxCPUs] {};
public:
    class Scope {
        Registry *registry_ {};uint32_t cpu_ {};
        friend class Registry;
        Scope(Registry *registry,uint32_t cpu):registry_(registry),cpu_(cpu) {}
    public:
        Scope()=default;~Scope() {reset();}
        Scope(const Scope &)=delete;Scope &operator=(const Scope &)=delete;
        Scope(Scope &&other) noexcept:registry_(other.registry_),cpu_(other.cpu_) {other.registry_=nullptr;}
        Scope &operator=(Scope &&other) noexcept {if(this!=&other) {reset();registry_=other.registry_;cpu_=other.cpu_;other.registry_=nullptr;}return *this;}
        void reset();
        explicit operator bool() const {return registry_!=nullptr;}
    };
    Scope arm(uint32_t cpu,Record &record);
    bool handle(uint32_t cpu,uintptr_t owner,XNUSavedState64 *state);
    bool active(uint32_t cpu) const;
};
Registry &registry();
}
extern "C" {
int amdhv_probe_read8(const void *,void *);
int amdhv_probe_write8(void *,const void *);
int amdhv_probe_write64(void *,const void *);
extern const unsigned char amdhv_probe_read8_fault[],amdhv_probe_read8_recover[];
extern const unsigned char amdhv_probe_write8_fault[],amdhv_probe_write8_recover[];
extern const unsigned char amdhv_probe_write64_fault[],amdhv_probe_write64_recover[];
}
#endif
