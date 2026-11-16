#ifndef AMDHV_SOFT_VMCS_HPP
#define AMDHV_SOFT_VMCS_HPP

#include <src/IntelVMX.hpp>
#include <src/AMDSVM.hpp>
#include <src/NPTManager.hpp>
#include <src/MemoryAccess.hpp>

namespace SoftVMCS {

static constexpr size_t MAX_CPUS          = 128;
static constexpr size_t MAX_VMCS_SLOTS    = 64;
static constexpr size_t VMCS_FIELD_SLOTS  = 512;

struct SoftVMCSInstance {
    NPTManager::SpaceKey space;
    uint64_t appleOwner {},appleLifetime {};
    bool      inUse {false};
    uint8_t   leased {0};
    uint32_t  ownerCPU {128};
    uint64_t  lifetime {0};
    uint32_t  lastCPU {128};
    uint64_t  lastRoot {0};
    bool      launched {false};
    uint64_t  vmcsPhysAddr {0};     // Key passed to vmclear / vmptrld (pinned vCPU +0x90)
    VMCB     *vmcbVA {nullptr};     // 4KB-aligned AMD VMCB virtual address
    uint64_t  vmcbPA {0};           // 4KB-aligned AMD VMCB physical address
    uint8_t  *msrpmVA {nullptr};    // 8KB (2-page) AMD MSR permission bitmap VA
    uint64_t  msrpmPA {0};          // 8KB AMD MSR permission bitmap PA
    uint8_t  *iopmVA {nullptr};     // 12KB (3-page) AMD I/O permission bitmap VA
    uint64_t  iopmPA {0};           // 12KB AMD I/O permission bitmap PA
    uint32_t  asid {1};             // Assigned AMD SVM ASID
    uint64_t  lastNptGen {0};       // Last synced NPT generation for TLB flush tracking
    uint32_t  preparedEvent {0};
    uint64_t  guestEferShadow {0};  // Guest's logical IA32_EFER (without forced SVME bit)
    uint64_t  fields[VMCS_FIELD_SLOTS] {};
};

// Initialize the global SoftVMCS pool and shared IOPM/MSRPM bitmaps
void init();

// Convert an Intel 16-bit VMCS field encoding (0x0000..0x6C16) to a dense index (0..511)
// Returns -1 if the encoding is invalid.
int fieldEncodingToIndex(uint32_t encoding);

// Read/write a field in a SoftVMCSInstance
bool readField(const SoftVMCSInstance *vmcs, uint32_t encoding, uint64_t &outValue);
bool writeField(SoftVMCSInstance *vmcs, uint32_t encoding, uint64_t value);

// A lease exclusively pins mutable vCPU state. Registry retirement cannot recycle
// a slot while a lease exists. Pointers obtained with get() must not escape it.
class Lease {
    SoftVMCSInstance *slot_ {nullptr};
    friend struct LeaseFactory;
    explicit Lease(SoftVMCSInstance *slot) : slot_(slot) {}
public:
    Lease() = default;
    ~Lease() { reset(); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }
    Lease &operator=(Lease &&other) noexcept {
        if (this != &other) { reset(); slot_ = other.slot_; other.slot_ = nullptr; }
        return *this;
    }
    void reset() { if (slot_) __atomic_store_n(&slot_->leased, uint8_t(0), __ATOMIC_RELEASE); slot_ = nullptr; }
    explicit operator bool() const { return slot_ != nullptr; }
    SoftVMCSInstance *get() const { return slot_; }
    SoftVMCSInstance *operator->() const { return slot_; }
};
// Sleepable setup preallocates all guest/host storage. Failure rolls back fully.
bool prepareResources(uint32_t cpuCount, uint32_t hardwareASIDCount);
// Teardown rejects outstanding leases and CPU bindings.
bool shutdown();
Lease getOrCreateByPhysAddr(uint64_t vmcsPA);
// Atomic ownership admission. An existing untagged/different-lifetime slot is
// refused; callers cannot race find() against a separate creation/tagging step.
Lease claimOwned(uint64_t vmcsPA,uint64_t owner,uint64_t lifetime,const NPTManager::SpaceKey &space);
Lease findByPhysAddr(uint64_t vmcsPA);
bool releaseByPhysAddr(uint64_t vmcsPA);
Lease getActiveVMCS(uint32_t cpuId);
struct ActiveIdentity {uint64_t physical {},owner {},lifetime {};};
// False distinguishes registry contention from a successfully observed empty
// CPU binding. No mutable VMCS pointer escapes this snapshot.
bool snapshotActive(uint32_t cpuId,ActiveIdentity &out);
bool setActiveVMCS(uint32_t cpuId, const Lease &vmcs);
bool clearActiveVMCS(uint32_t cpuId);
// Lookup only, never allocates on a rendezvous CPU.
uint64_t getHostSavePA(uint32_t cpuId);

// Read a synthesized Intel VMX / Architectural MSR
bool readSynthesizedMSR(uint32_t msrIndex, uint64_t &outValue);

// Write a synthesized Intel VMX / Architectural MSR
bool writeSynthesizedMSR(uint32_t msrIndex, uint64_t value);

// Emulate a trapping VMX or MSR instruction in kernel mode (#UD = 6 or #GP = 13).
// Returns true if the instruction was handled and state->rip was advanced.
bool handleKernelTrap(XNUSavedState64 *state);
// Bounded offline emulator / adapter contract. No instruction or operand pointer
// is dereferenced directly; a kernel adapter must provide fault recovery.
// ownedSVMCPU is reserved for the retained trap adapter: it supplies verified
// CPU ownership instead of a fabricated VMXON region and permits no allocation
// or synchronous invalidation in exception context.
bool emulateInstruction(XNUSavedState64 *state, const uint8_t *bytes, size_t length,
                        const MemoryAccess::Access &memory, uint32_t cpu,bool ownedSVMCPU=false);

} // namespace SoftVMCS

#endif // AMDHV_SOFT_VMCS_HPP
