#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <Hypervisor/hv.h>
#include <mach/mach_time.h>
#include <Hypervisor/hv_vmx.h>

static const char *hvErrStr(hv_return_t err) {
    switch (err) {
        case HV_SUCCESS:      return "HV_SUCCESS";
        case HV_ERROR:        return "HV_ERROR";
        case HV_BUSY:         return "HV_BUSY";
        case HV_BAD_ARGUMENT: return "HV_BAD_ARGUMENT";
        case HV_NO_RESOURCES: return "HV_NO_RESOURCES";
        case HV_NO_DEVICE:    return "HV_NO_DEVICE";
        case HV_UNSUPPORTED:  return "HV_UNSUPPORTED";
        default:              return "HV_UNKNOWN";
    }
}

int main(void) {
    int status=1;
    bool vmCreated=false,vcpuCreated=false,mapped=false;
    void *guestRam=NULL;
    hv_vcpuid_t vcpu=0;
    hv_return_t ret;
    uint64_t exitReason=0,exitQual=0,rax=0,rip=0;
    printf("=== AMDHV bounded live smoke test (requires an accepted backend) ===\n");
#define HV_CHECK(call) do { \
    ret=(call); \
    if(ret!=HV_SUCCESS) { \
        printf("[FAIL] %s: 0x%x (%s)\n",#call,(unsigned)ret,hvErrStr(ret)); \
        goto cleanup; \
    } \
} while(0)
    HV_CHECK(hv_vm_create(HV_VM_DEFAULT));vmCreated=true;
    if(posix_memalign(&guestRam,4096,4096) || !guestRam) {
        printf("[FAIL] guest RAM allocation\n");goto cleanup;
    }
    memset(guestRam,0x90,4096);
    const uint8_t code[]={0xb8,0x37,0x13,0xe6,0x80,0xf4};
    memcpy(guestRam,code,sizeof(code));
    HV_CHECK(hv_vm_map(guestRam,0x1000,4096,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC));mapped=true;
    HV_CHECK(hv_vcpu_create(&vcpu,HV_VCPU_DEFAULT));vcpuCreated=true;
    // 4. Initialize vCPU state for 16-bit Unpaged Real Mode (Unrestricted Guest)
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CR0, 0x00000020ULL)); // NE=1, PE=0, PG=0
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CR4, 0x00002000ULL));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CS, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CS_BASE, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CS_LIMIT, 0xFFFF));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_CS_AR, 0x009B));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_DS, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_DS_BASE, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_DS_LIMIT, 0xFFFF));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_DS_AR, 0x0093));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_ES, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_ES_BASE, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_ES_LIMIT, 0xFFFF));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_ES_AR, 0x0093));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_SS, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_SS_BASE, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_SS_LIMIT, 0xFFFF));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_SS_AR, 0x0093));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_TR, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_TR_BASE, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_TR_LIMIT, 0xFFFF));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_TR_AR, 0x008B));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_LDTR, 0x0000));
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_LDTR_AR, 0x10000)); // Unusable

    // Enable Unconditional I/O exiting and HLT exiting
    uint64_t procCtls = 0;
    HV_CHECK(hv_vmx_vcpu_read_vmcs(vcpu, VMCS_CTRL_CPU_BASED, &procCtls));
    procCtls |= (1ULL << 7) | (1ULL << 24); // HLT + UNCOND_IO
    HV_CHECK(hv_vmx_vcpu_write_vmcs(vcpu, VMCS_CTRL_CPU_BASED, procCtls));

    HV_CHECK(hv_vcpu_write_register(vcpu, HV_X86_RIP, 0x1000));
    HV_CHECK(hv_vcpu_write_register(vcpu, HV_X86_RFLAGS, 0x0002));
    HV_CHECK(hv_vcpu_write_register(vcpu, HV_X86_RAX, 0x0000));


    mach_timebase_info_data_t timebase;
    if(mach_timebase_info(&timebase)!=KERN_SUCCESS || !timebase.numer || !timebase.denom) {
        printf("[FAIL] deadline clock unavailable\n");goto cleanup;
    }
    uint64_t ticks=(2000000000ULL*timebase.denom)/timebase.numer;
    uint64_t now=mach_absolute_time();
    if(!ticks || ticks>UINT64_MAX-now) {printf("[FAIL] invalid deadline\n");goto cleanup;}
    printf("[INFO] Entering guest with a two-second deadline\n");
    // HV_UNSUPPORTED is a failure: there is no unbounded run fallback.
    HV_CHECK(hv_vcpu_run_until(vcpu,now+ticks));
    HV_CHECK(hv_vmx_vcpu_read_vmcs(vcpu,VMCS_RO_EXIT_REASON,&exitReason));
    HV_CHECK(hv_vmx_vcpu_read_vmcs(vcpu,VMCS_RO_EXIT_QUALIFIC,&exitQual));
    HV_CHECK(hv_vcpu_read_register(vcpu,HV_X86_RAX,&rax));
    HV_CHECK(hv_vcpu_read_register(vcpu,HV_X86_RIP,&rip));
    printf("Exit reason=%llu qualification=0x%llx RAX=0x%llx RIP=0x%llx\n",
           (unsigned long long)exitReason,(unsigned long long)exitQual,
           (unsigned long long)rax,(unsigned long long)rip);
    if(exitReason==30 && ((exitQual>>16)&0xffff)==0x80 && !(exitQual&0x3f) && rip==0x1003 && (rax&0xffff)==0x1337) {
        status=0;printf("[PASS] Expected guest state and port-I/O exit observed\n");
    } else printf("[FAIL] Unexpected guest state or exit\n");
cleanup:
    if(vcpuCreated) {
        ret=hv_vcpu_destroy(vcpu);
        if(ret==HV_SUCCESS) vcpuCreated=false;
        else {status=1;printf("[FAIL] vCPU cleanup: %s\n",hvErrStr(ret));}
    }
    if(mapped && !vcpuCreated) {
        ret=hv_vm_unmap(0x1000,4096);
        if(ret==HV_SUCCESS) mapped=false;
        else {status=1;printf("[FAIL] guest unmap: %s\n",hvErrStr(ret));}
    }
    if(vmCreated) {
        ret=hv_vm_destroy();
        if(ret==HV_SUCCESS) {vmCreated=false;mapped=false;}
        else {status=1;printf("[FAIL] VM cleanup: %s\n",hvErrStr(ret));}
    }
    if(!mapped) free(guestRam);
    else printf("[FAIL] Retaining mapped backing memory until process teardown\n");
    return status;
}
