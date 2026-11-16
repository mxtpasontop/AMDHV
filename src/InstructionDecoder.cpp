#include <src/InstructionDecoder.hpp>

namespace InstructionDecoder {

uint64_t getRegister(const XNUSavedState64 *state, uint8_t regIdx) {
    if (!state || regIdx>=16) return 0;
    switch (regIdx) {
        case 0:  return state->rax;
        case 1:  return state->rcx;
        case 2:  return state->rdx;
        case 3:  return state->rbx;
        case 4:  return state->rsp;
        case 5:  return state->rbp;
        case 6:  return state->rsi;
        case 7:  return state->rdi;
        case 8:  return state->r8;
        case 9:  return state->r9;
        case 10: return state->r10;
        case 11: return state->r11;
        case 12: return state->r12;
        case 13: return state->r13;
        case 14: return state->r14;
        case 15: return state->r15;
        default: return 0;
    }
}

void setRegister(XNUSavedState64 *state, uint8_t regIdx, uint64_t value) {
    if (!state || regIdx>=16) return;
    switch (regIdx) {
        case 0:  state->rax = value; break;
        case 1:  state->rcx = value; break;
        case 2:  state->rdx = value; break;
        case 3:  state->rbx = value; break;
        case 4:  state->rsp = value; break;
        case 5:  state->rbp = value; break;
        case 6:  state->rsi = value; break;
        case 7:  state->rdi = value; break;
        case 8:  state->r8  = value; break;
        case 9:  state->r9  = value; break;
        case 10: state->r10 = value; break;
        case 11: state->r11 = value; break;
        case 12: state->r12 = value; break;
        case 13: state->r13 = value; break;
        case 14: state->r14 = value; break;
        case 15: state->r15 = value; break;
        default: break;
    }
}

static bool decodeModRM64(const uint8_t *insn, size_t size, size_t offset, uint8_t rex, const XNUSavedState64 *state,
                          DecodedInstruction &outInst, size_t &outTotalLen) {
    if (offset >= size) return false;
    uint8_t modrm = insn[offset++];
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm  = modrm & 0x07;

    uint8_t rexR = (rex & 0x04) ? 8 : 0;
    uint8_t rexX = (rex & 0x02) ? 8 : 0;
    uint8_t rexB = (rex & 0x01) ? 8 : 0;

    outInst.regIndex = reg | rexR;

    if (mod == 3) {
        outInst.isMemOperand = false;
        outInst.rmRegIndex   = rm | rexB;
        outInst.memAddress   = 0;
        outTotalLen          = offset;
        return true;
    }

    outInst.isMemOperand = true;
    uint64_t effectiveAddr = 0;
    bool ripRelative = false;

    if (rm == 4) {
        // SIB byte present
        if (offset >= size) return false;
        uint8_t sib   = insn[offset++];
        uint8_t scale = (sib >> 6) & 0x03;
        uint8_t index = ((sib >> 3) & 0x07) | rexX;
        uint8_t base  = (sib & 0x07) | rexB;

        if ((sib & 0x07) == 5 && mod == 0) {
            // Disp32 with no base
            effectiveAddr = 0;
        } else {
            effectiveAddr = getRegister(state, base);
        }

        // Index == 4 (without REX.X) means no index register
        if (index != 4) {
            effectiveAddr += (getRegister(state, index) << scale);
        }
    } else if (mod == 0 && rm == 5) {
        // RIP-relative addressing (disp32 added to next instruction's RIP)
        ripRelative = true;
    } else {
        effectiveAddr = getRegister(state, rm | rexB);
    }

    int64_t disp = 0;
    if (mod == 0) {
        if (rm == 5 || (rm == 4 && (insn[offset - 1] & 0x07) == 5)) {
            if (size - offset < 4) return false;
            int32_t d32 = static_cast<int32_t>(
                static_cast<uint32_t>(insn[offset]) |
                (static_cast<uint32_t>(insn[offset + 1]) << 8) |
                (static_cast<uint32_t>(insn[offset + 2]) << 16) |
                (static_cast<uint32_t>(insn[offset + 3]) << 24));
            disp = d32;
            offset += 4;
        }
    } else if (mod == 1) {
        if (offset >= size) return false;
        disp = static_cast<int8_t>(insn[offset++]);
    } else if (mod == 2) {
        if (size - offset < 4) return false;
        int32_t d32 = static_cast<int32_t>(
            static_cast<uint32_t>(insn[offset]) |
            (static_cast<uint32_t>(insn[offset + 1]) << 8) |
            (static_cast<uint32_t>(insn[offset + 2]) << 16) |
            (static_cast<uint32_t>(insn[offset + 3]) << 24));
        disp = d32;
        offset += 4;
    }

    if (ripRelative) {
        effectiveAddr = state->rip + offset + static_cast<uint64_t>(disp);
    } else {
        effectiveAddr += static_cast<uint64_t>(disp);
    }

    outInst.memAddress = effectiveAddr;
    outTotalLen        = offset;
    return true;
}

bool decodeInstruction(const uint8_t *insn, size_t size, const XNUSavedState64 *state, DecodedInstruction &outInst) {
    outInst = DecodedInstruction{};
    if (!state || !insn || !size) {
        return false;
    }

    if (size > 15) size = 15;
    size_t idx = 0;
    bool has66 = false;
    bool hasF2 = false;
    bool hasF3 = false;
    uint8_t rex = 0;

    // Parse legacy & REX prefixes (up to 4 bytes)
    while (idx < size) {
        uint8_t b = insn[idx];
        if (b == 0x66) {
            if (has66 || rex) return false;
            has66 = true;
            idx++;
        } else if (b == 0xF2) {
            if (hasF2 || hasF3 || rex) return false;
            hasF2 = true;
            idx++;
        } else if (b == 0xF3) {
            if (hasF2 || hasF3 || rex) return false;
            hasF3 = true;
            idx++;
        } else if (b == 0xF0 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x67) {
            return false; // Unsupported addressing/illegal LOCK is never ignored.
        } else if ((b & 0xF0) == 0x40) {
            if (rex) return false;
            rex = b;
            idx++;
        } else {
            break;
        }
    }

    // All VMX and MSR instructions start with two-byte opcode escape 0x0F
    if (idx >= size || insn[idx] != 0x0F) {
        return false;
    }
    idx++;

    if (idx >= size) return false;
    uint8_t op2 = insn[idx++];

    // 1. WRMSR (0F 30) and RDMSR (0F 32)
    if (op2 == 0x30 && !has66 && !hasF2 && !hasF3) {
        outInst.op     = DecodedOp::Wrmsr;
        outInst.length = static_cast<uint8_t>(idx);
        return true;
    }
    if (op2 == 0x32 && !has66 && !hasF2 && !hasF3) {
        outInst.op     = DecodedOp::Rdmsr;
        outInst.length = static_cast<uint8_t>(idx);
        return true;
    }

    // 2. VMXOFF (0F 01 C4)
    if (op2 == 0x01) {
        if (idx >= size) return false;
        uint8_t modrm = insn[idx];
        if (modrm == 0xC4 && !has66 && !hasF2 && !hasF3) {
            outInst.op     = DecodedOp::VMXOff;
            outInst.length = static_cast<uint8_t>(idx + 1);
            return true;
        }
        return false;
    }

    // 3. VMREAD (0F 78 /r) and VMWRITE (0F 79 /r)
    if ((op2 == 0x78 || op2 == 0x79) && !has66 && !hasF2 && !hasF3) {
        size_t totalLen = 0;
        if (!decodeModRM64(insn, size, idx, rex, state, outInst, totalLen)) {
            return false;
        }
        outInst.op     = (op2 == 0x78) ? DecodedOp::VMRead : DecodedOp::VMWrite;
        outInst.length = static_cast<uint8_t>(totalLen);
        return true;
    }

    // 4. VMPTRLD (0F C7 /6), VMCLEAR (66 0F C7 /6), VMXON (F3 0F C7 /6), VMPTRST (0F C7 /7)
    if (op2 == 0xC7 && !hasF2) {
        size_t totalLen = 0;
        if (!decodeModRM64(insn, size, idx, rex, state, outInst, totalLen)) {
            return false;
        }
        if (!outInst.isMemOperand) {
            return false;
        }
        uint8_t subop = outInst.regIndex & 0x07;
        if (subop == 6) {
            if (has66 && !hasF3) {
                outInst.op = DecodedOp::VMClear;
            } else if (hasF3 && !has66) {
                outInst.op = DecodedOp::VMXOn;
            } else if (!has66 && !hasF3) {
                outInst.op = DecodedOp::VMPtrLd;
            } else {
                return false;
            }
            outInst.length = static_cast<uint8_t>(totalLen);
            return true;
        } else if (subop == 7 && !has66 && !hasF3) {
            outInst.op     = DecodedOp::VMPtrSt;
            outInst.length = static_cast<uint8_t>(totalLen);
            return true;
        }
        return false;
    }

    // 5. INVEPT (66 0F 38 80 /r) and INVVPID (66 0F 38 81 /r)
    if (op2 == 0x38 && has66 && !hasF2 && !hasF3) {
        if (idx >= size) return false;
        uint8_t op3 = insn[idx++];
        if (op3 == 0x80 || op3 == 0x81) {
            size_t totalLen = 0;
            if (!decodeModRM64(insn, size, idx, rex, state, outInst, totalLen)) {
                return false;
            }
            if (!outInst.isMemOperand) {
                return false;
            }
            outInst.op     = (op3 == 0x80) ? DecodedOp::InvEpt : DecodedOp::InvVpid;
            outInst.length = static_cast<uint8_t>(totalLen);
            return true;
        }
        return false;
    }

    return false;
}

bool decodeKernelInstruction(const XNUSavedState64 *state, DecodedInstruction &outInst) {
    (void)state; outInst={}; return false;
}

// Deliberately restricted decoder for known exit instructions. Unknown or
// truncated encodings return 0; a guessed length must never advance guest RIP.
uint32_t decodeInstructionLength64(const uint8_t *bytes, size_t size) {
    if (!bytes || !size) return 0;
    if (size > 15) size = 15;
    size_t i = 0;
    while (i < size && (bytes[i] == 0x66 || bytes[i] == 0xf3 ||
                        (bytes[i] >= 0x40 && bytes[i] <= 0x4f))) ++i;
    if (i == size) return 0;
    uint8_t op = bytes[i++];
    if (op == 0xf4 || op == 0x90 || (op >= 0xec && op <= 0xef)) return static_cast<uint32_t>(i);
    if ((op >= 0xe4 && op <= 0xe7) || op == 0xcd)
        return i < size ? static_cast<uint32_t>(i + 1) : 0;
    if (op != 0x0f || i == size) return 0;
    op = bytes[i++];
    if (op == 0xa2 || op == 0x30 || op == 0x31 || op == 0x32 ||
        op == 0x33 || op == 0x08 || op == 0x09) return static_cast<uint32_t>(i);
    if (op == 0x01 && i < size && (bytes[i] == 0xc1 || bytes[i] == 0xd9 ||
                                  bytes[i] == 0xd1 || bytes[i] == 0xf9))
        return static_cast<uint32_t>(i + 1);
    return 0;
}

} // namespace InstructionDecoder
