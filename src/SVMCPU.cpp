#include <src/SVMCPU.hpp>
namespace SVMCPU {
bool Driver::configure(Operations ops,const uint64_t *pages,size_t count) {
    Runtime::Guard guard(gate_);
    if(!guard || configured_ || !ops.cpus || ops.cpus>128 || count!=ops.cpus || !pages || ops.requiredASIDs<2 ||
       !ops.currentCPU || !ops.onCPU || !ops.features || !ops.read || !ops.write) return false;
    for(size_t i=0;i<count;++i) {
        if(!pages[i] || (pages[i]&~0x000ffffffffff000ULL)) return false;
        for(size_t j=0;j<i;++j) if(pages[i]==pages[j]) return false;
    }
    for(size_t i=0;i<count;++i) saved_[i].ours=pages[i];
    ops_=ops;configured_=true;return true;
}
bool Driver::ownsCPU(uint32_t cpu) const {
    return cpu<128 && __atomic_load_n(&saved_[cpu].dirty,__ATOMIC_ACQUIRE);
}
bool Driver::readyCPU(uint32_t cpu) const {return cpu<128 && saved_[cpu].enabled;}
void Driver::dispatch(void *argument) {
    auto &job=*static_cast<Job *>(argument);
    auto &driver=*job.driver;
    if(driver.ops_.currentCPU(driver.ops_.context)!=job.cpu) return;
    if(job.visited) {job.ok=false;return;}
    job.visited=true;
    job.ok=job.enable?driver.enableLocal(job.cpu):driver.restoreLocal(job.cpu);
}
bool Driver::enableLocal(uint32_t cpu) {
    auto &s=saved_[cpu];
    if(s.dirty || s.enabled) return false;
    Features f;
    uint64_t vmcr=0,pat=0;
    auto read=[&](uint32_t index,uint64_t &value) {return ops_.read(ops_.context,index,value);};
    if(!ops_.features(ops_.context,f) || !f.svm || !f.npt || !f.nrips || f.asids<ops_.requiredASIDs ||
       f.physicalBits<32 || f.physicalBits>52 || s.ours>=(1ULL<<f.physicalBits) ||
       !read(MSR_AMD_VM_CR,vmcr) || (vmcr&VM_CR_SVMDIS) ||
       !read(MSR_AMD_EFER,s.efer) || (s.efer&EFER_SVME) ||
       !read(MSR_AMD_VM_HSAVE_PA,s.hsave) || !read(MSR_IA32_PAT,pat) || (pat&255)!=6) return false;
    // Set before the first write: even a callback that fails after performing
    // its write must be restored by the caller's rollback path.
    __atomic_store_n(&s.dirty,true,__ATOMIC_RELEASE);
    if(!ops_.write(ops_.context,MSR_AMD_EFER,s.efer|EFER_SVME) ||
       !ops_.write(ops_.context,MSR_AMD_VM_HSAVE_PA,s.ours)) return false;
    uint64_t efer=0,hsave=0;
    if(!read(MSR_AMD_EFER,efer) || !read(MSR_AMD_VM_HSAVE_PA,hsave) ||
       efer!=(s.efer|EFER_SVME) || hsave!=s.ours) return false;
    s.enabled=true;return true;
}
bool Driver::restoreLocal(uint32_t cpu) {
    auto &s=saved_[cpu];
    s.enabled=false; // Close admission before any restoration attempt can fail.
    if(!s.dirty) return true;
    uint64_t efer=0,hsave=0;
    if(!ops_.read(ops_.context,MSR_AMD_EFER,efer) ||
       !ops_.read(ops_.context,MSR_AMD_VM_HSAVE_PA,hsave)) return false;
    // Never overwrite a foreign SVM owner or unexpected firmware/OS state.
    if((efer!=s.efer && efer!=(s.efer|EFER_SVME)) || (hsave!=s.hsave && hsave!=s.ours)) return false;
    if(hsave!=s.hsave && !ops_.write(ops_.context,MSR_AMD_VM_HSAVE_PA,s.hsave)) return false;
    if(efer!=s.efer && !ops_.write(ops_.context,MSR_AMD_EFER,s.efer)) return false;
    if(!ops_.read(ops_.context,MSR_AMD_EFER,efer) || efer!=s.efer ||
       !ops_.read(ops_.context,MSR_AMD_VM_HSAVE_PA,hsave) || hsave!=s.hsave) return false;
    __atomic_store_n(&s.dirty,false,__ATOMIC_RELEASE);return true;
}
bool Driver::enable(uint32_t cpu) {
    Runtime::Guard guard(gate_);
    if(!guard || !configured_ || cpu>=ops_.cpus) return false;
    Job job{this,cpu,true,false,false};
    bool success=ops_.onCPU(ops_.context,cpu,dispatch,&job) && job.visited && job.ok;
    if(!success) saved_[cpu].enabled=false;
    return success;
}
bool Driver::restore(uint32_t cpu) {
    Runtime::Guard guard(gate_);
    if(!guard || !configured_ || cpu>=ops_.cpus) return false;
    saved_[cpu].enabled=false;
    Job job{this,cpu,false,false,false};
    return ops_.onCPU(ops_.context,cpu,dispatch,&job) && job.visited && job.ok;
}
HostLifecycle::Adapter Driver::adapter() {
    return {this,ops_.cpus,
        [](void *c,uint32_t cpu) {return static_cast<Driver *>(c)->enable(cpu);},
        [](void *c,uint32_t cpu) {return static_cast<Driver *>(c)->restore(cpu);}};
}
}
