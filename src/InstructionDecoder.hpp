#ifndef AMDHV_INSTRUCTION_DECODER_HPP
#define AMDHV_INSTRUCTION_DECODER_HPP

#include <src/IntelVMX.hpp>

namespace InstructionDecoder {

enum class DecodedOp {
    None = 0,
    VMRead,         // 0F 78 /r: vmread r/m64, r64
    VMWrite,        // 0F 79 /r: vmwrite r64, r/m64
    VMClear,        // 66 0F C7 /6: vmclear m64
    VMPtrLd,        // 0F C7 /6: vmptrld m64
    VMPtrSt,        // 0F C7 /7: vmptrst m64
    VMXOn,          // F3 0F C7 /6: vmxon m64
    VMXOff,         // 0F 01 C4: vmxoff
    InvEpt,         // 66 0F 38 80 /r: invept r64, m128
    InvVpid,        // 66 0F 38 81 /r: invvpid r64, m128
    Rdmsr,          // 0F 32: rdmsr
    Wrmsr,          // 0F 30: wrmsr
};

struct DecodedInstruction {
    DecodedOp op {DecodedOp::None};
    uint8_t   length {0};
    bool      isMemOperand {false};
    uint8_t   regIndex {0};     // 0..15 (ModRM.reg + REX.R)
    uint8_t   rmRegIndex {0};   // 0..15 (ModRM.rm + REX.B when mod == 3)
    uint64_t  memAddress {0};   // Effective virtual address when mod != 3
};

// Reads a 64-bit general purpose register (0=RAX, 1=RCX, 2=RDX, 3=RBX, 4=RSP, 5=RBP, 6=RSI, 7=RDI, 8..15=R8..R15)
uint64_t getRegister(const XNUSavedState64 *state, uint8_t regIdx);

// Writes a 64-bit general purpose register in XNUSavedState64
void setRegister(XNUSavedState64 *state, uint8_t regIdx, uint64_t value);

// Decode a bounded snapshot. This API never reads through state->rip.
bool decodeInstruction(const uint8_t *bytes, size_t size, const XNUSavedState64 *state, DecodedInstruction &outInst);

// Disabled compatibility adapter; callers must provide a bounded snapshot.
bool decodeKernelInstruction(const XNUSavedState64 *state, DecodedInstruction &outInst);

// Restricted known-exit decoder; unknown/truncated instructions return zero.
uint32_t decodeInstructionLength64(const uint8_t *bytes, size_t maxLen = 15);

} // namespace InstructionDecoder

#endif // AMDHV_INSTRUCTION_DECODER_HPP
