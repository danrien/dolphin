// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Arm64Emitter.h"
#include "Common/BitSet.h"
#include "Common/CommonTypes.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSP.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitArm64/Jit.h"
#include "Core/PowerPC/JitArm64/JitArm64_RegCache.h"
#include "Core/PowerPC/JitArm64/Jit_Util.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Arm64Gen;

void JitArm64::SafeLoadToReg(u32 dest, s32 addr, s32 offsetReg, u32 flags, s32 offset, bool update)
{
  // We want to make sure to not get LR as a temp register
  gpr.Lock(ARM64Reg::W0, ARM64Reg::W30);

  gpr.BindToRegister(dest, dest == (u32)addr || dest == (u32)offsetReg);
  ARM64Reg dest_reg = gpr.R(dest);
  ARM64Reg up_reg = ARM64Reg::INVALID_REG;
  ARM64Reg off_reg = ARM64Reg::INVALID_REG;

  if (addr != -1 && !gpr.IsImm(addr))
    up_reg = gpr.R(addr);

  if (offsetReg != -1 && !gpr.IsImm(offsetReg))
    off_reg = gpr.R(offsetReg);

  ARM64Reg addr_reg = ARM64Reg::W0;
  u32 imm_addr = 0;
  bool is_immediate = false;

  if (offsetReg == -1)
  {
    if (addr != -1)
    {
      if (gpr.IsImm(addr))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(addr) + offset;
      }
      else
      {
        ADDI2R(addr_reg, up_reg, offset, addr_reg);
      }
    }
    else
    {
      is_immediate = true;
      imm_addr = offset;
    }
  }
  else
  {
    if (addr != -1)
    {
      if (gpr.IsImm(addr) && gpr.IsImm(offsetReg))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(addr) + gpr.GetImm(offsetReg);
      }
      else if (gpr.IsImm(addr) && !gpr.IsImm(offsetReg))
      {
        u32 reg_offset = gpr.GetImm(addr);
        ADDI2R(addr_reg, off_reg, reg_offset, addr_reg);
      }
      else if (!gpr.IsImm(addr) && gpr.IsImm(offsetReg))
      {
        u32 reg_offset = gpr.GetImm(offsetReg);
        ADDI2R(addr_reg, up_reg, reg_offset, addr_reg);
      }
      else
      {
        ADD(addr_reg, up_reg, off_reg);
      }
    }
    else
    {
      if (gpr.IsImm(offsetReg))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(offsetReg);
      }
      else
      {
        MOV(addr_reg, off_reg);
      }
    }
  }

  ARM64Reg XA = EncodeRegTo64(addr_reg);

  if (is_immediate)
    MOVI2R(XA, imm_addr);

  if (update)
  {
    gpr.BindToRegister(addr, false);
    MOV(gpr.R(addr), addr_reg);
  }

  BitSet32 regs_in_use = gpr.GetCallerSavedUsed();
  BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();
  regs_in_use[DecodeReg(ARM64Reg::W0)] = 0;
  regs_in_use[DecodeReg(dest_reg)] = 0;

  u32 access_size = BackPatchInfo::GetFlagSize(flags);
  u32 mmio_address = 0;
  if (is_immediate)
    mmio_address = PowerPC::IsOptimizableMMIOAccess(imm_addr, access_size);

  if (jo.fastmem_arena && is_immediate && PowerPC::IsOptimizableRAMAddress(imm_addr))
  {
    EmitBackpatchRoutine(flags, true, false, dest_reg, XA, BitSet32(0), BitSet32(0));
  }
  else if (mmio_address)
  {
    MMIOLoadToReg(Memory::mmio_mapping.get(), this, regs_in_use, fprs_in_use, dest_reg,
                  mmio_address, flags);
  }
  else
  {
    EmitBackpatchRoutine(flags, jo.fastmem, jo.fastmem, dest_reg, XA, regs_in_use, fprs_in_use);
  }

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W30);
}

void JitArm64::SafeStoreFromReg(s32 dest, u32 value, s32 regOffset, u32 flags, s32 offset)
{
  // We want to make sure to not get LR as a temp register
  gpr.Lock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W30);

  ARM64Reg RS = gpr.R(value);

  ARM64Reg reg_dest = ARM64Reg::INVALID_REG;
  ARM64Reg reg_off = ARM64Reg::INVALID_REG;

  if (regOffset != -1 && !gpr.IsImm(regOffset))
    reg_off = gpr.R(regOffset);
  if (dest != -1 && !gpr.IsImm(dest))
    reg_dest = gpr.R(dest);

  BitSet32 regs_in_use = gpr.GetCallerSavedUsed();
  BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();
  regs_in_use[DecodeReg(ARM64Reg::W0)] = 0;
  regs_in_use[DecodeReg(ARM64Reg::W1)] = 0;

  ARM64Reg addr_reg = ARM64Reg::W1;

  u32 imm_addr = 0;
  bool is_immediate = false;

  if (regOffset == -1)
  {
    if (dest != -1)
    {
      if (gpr.IsImm(dest))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(dest) + offset;
      }
      else
      {
        ADDI2R(addr_reg, reg_dest, offset, addr_reg);
      }
    }
    else
    {
      is_immediate = true;
      imm_addr = offset;
    }
  }
  else
  {
    if (dest != -1)
    {
      if (gpr.IsImm(dest) && gpr.IsImm(regOffset))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(dest) + gpr.GetImm(regOffset);
      }
      else if (gpr.IsImm(dest) && !gpr.IsImm(regOffset))
      {
        u32 reg_offset = gpr.GetImm(dest);
        ADDI2R(addr_reg, reg_off, reg_offset, addr_reg);
      }
      else if (!gpr.IsImm(dest) && gpr.IsImm(regOffset))
      {
        u32 reg_offset = gpr.GetImm(regOffset);
        ADDI2R(addr_reg, reg_dest, reg_offset, addr_reg);
      }
      else
      {
        ADD(addr_reg, reg_dest, reg_off);
      }
    }
    else
    {
      if (gpr.IsImm(regOffset))
      {
        is_immediate = true;
        imm_addr = gpr.GetImm(regOffset);
      }
      else
      {
        MOV(addr_reg, reg_off);
      }
    }
  }

  ARM64Reg XA = EncodeRegTo64(addr_reg);

  u32 access_size = BackPatchInfo::GetFlagSize(flags);
  u32 mmio_address = 0;
  if (is_immediate)
    mmio_address = PowerPC::IsOptimizableMMIOAccess(imm_addr, access_size);

  if (is_immediate && jo.optimizeGatherPipe && PowerPC::IsOptimizableGatherPipeWrite(imm_addr))
  {
    int accessSize;
    if (flags & BackPatchInfo::FLAG_SIZE_32)
      accessSize = 32;
    else if (flags & BackPatchInfo::FLAG_SIZE_16)
      accessSize = 16;
    else
      accessSize = 8;

    LDR(IndexType::Unsigned, ARM64Reg::X0, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));

    ARM64Reg temp = ARM64Reg::W1;
    temp = ByteswapBeforeStore(this, temp, RS, flags, true);

    if (accessSize == 32)
      STR(IndexType::Post, temp, ARM64Reg::X0, 4);
    else if (accessSize == 16)
      STRH(IndexType::Post, temp, ARM64Reg::X0, 2);
    else
      STRB(IndexType::Post, temp, ARM64Reg::X0, 1);

    STR(IndexType::Unsigned, ARM64Reg::X0, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));

    js.fifoBytesSinceCheck += accessSize >> 3;
  }
  else if (jo.fastmem_arena && is_immediate && PowerPC::IsOptimizableRAMAddress(imm_addr))
  {
    MOVI2R(XA, imm_addr);
    EmitBackpatchRoutine(flags, true, false, RS, XA, BitSet32(0), BitSet32(0));
  }
  else if (mmio_address)
  {
    MMIOWriteRegToAddr(Memory::mmio_mapping.get(), this, regs_in_use, fprs_in_use, RS, mmio_address,
                       flags);
  }
  else
  {
    if (is_immediate)
      MOVI2R(XA, imm_addr);

    EmitBackpatchRoutine(flags, jo.fastmem, jo.fastmem, RS, XA, regs_in_use, fprs_in_use);
  }

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W1, ARM64Reg::W30);
}

FixupBranch JitArm64::BATAddressLookup(ARM64Reg addr_out, ARM64Reg addr_in, ARM64Reg tmp,
                                       const void* bat_table)
{
  tmp = EncodeRegTo64(tmp);

  MOVP2R(tmp, bat_table);
  LSR(addr_out, addr_in, PowerPC::BAT_INDEX_SHIFT);
  LDR(addr_out, tmp, ArithOption(addr_out, true));
  FixupBranch pass = TBNZ(addr_out, IntLog2(PowerPC::BAT_MAPPED_BIT));
  FixupBranch fail = B();
  SetJumpTarget(pass);
  return fail;
}

void JitArm64::lXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(jo.memcheck);

  u32 a = inst.RA, b = inst.RB, d = inst.RD;
  s32 offset = inst.SIMM_16;
  s32 offsetReg = -1;
  u32 flags = BackPatchInfo::FLAG_LOAD;
  bool update = false;

  switch (inst.OPCD)
  {
  case 31:
    offsetReg = b;
    switch (inst.SUBOP10)
    {
    case 55:  // lwzux
      update = true;
      [[fallthrough]];
    case 23:  // lwzx
      flags |= BackPatchInfo::FLAG_SIZE_32;
      break;
    case 119:  // lbzux
      update = true;
      [[fallthrough]];
    case 87:  // lbzx
      flags |= BackPatchInfo::FLAG_SIZE_8;
      break;
    case 311:  // lhzux
      update = true;
      [[fallthrough]];
    case 279:  // lhzx
      flags |= BackPatchInfo::FLAG_SIZE_16;
      break;
    case 375:  // lhaux
      update = true;
      [[fallthrough]];
    case 343:  // lhax
      flags |= BackPatchInfo::FLAG_EXTEND | BackPatchInfo::FLAG_SIZE_16;
      break;
    case 534:  // lwbrx
      flags |= BackPatchInfo::FLAG_REVERSE | BackPatchInfo::FLAG_SIZE_32;
      break;
    case 790:  // lhbrx
      flags |= BackPatchInfo::FLAG_REVERSE | BackPatchInfo::FLAG_SIZE_16;
      break;
    }
    break;
  case 33:  // lwzu
    update = true;
    [[fallthrough]];
  case 32:  // lwz
    flags |= BackPatchInfo::FLAG_SIZE_32;
    break;
  case 35:  // lbzu
    update = true;
    [[fallthrough]];
  case 34:  // lbz
    flags |= BackPatchInfo::FLAG_SIZE_8;
    break;
  case 41:  // lhzu
    update = true;
    [[fallthrough]];
  case 40:  // lhz
    flags |= BackPatchInfo::FLAG_SIZE_16;
    break;
  case 43:  // lhau
    update = true;
    [[fallthrough]];
  case 42:  // lha
    flags |= BackPatchInfo::FLAG_EXTEND | BackPatchInfo::FLAG_SIZE_16;
    break;
  }

  SafeLoadToReg(d, update ? a : (a ? a : -1), offsetReg, flags, offset, update);
}

void JitArm64::stX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(jo.memcheck);

  u32 a = inst.RA, b = inst.RB, s = inst.RS;
  s32 offset = inst.SIMM_16;
  s32 regOffset = -1;
  u32 flags = BackPatchInfo::FLAG_STORE;
  bool update = false;
  switch (inst.OPCD)
  {
  case 31:
    regOffset = b;
    switch (inst.SUBOP10)
    {
    case 183:  // stwux
      update = true;
      [[fallthrough]];
    case 151:  // stwx
      flags |= BackPatchInfo::FLAG_SIZE_32;
      break;
    case 247:  // stbux
      update = true;
      [[fallthrough]];
    case 215:  // stbx
      flags |= BackPatchInfo::FLAG_SIZE_8;
      break;
    case 439:  // sthux
      update = true;
      [[fallthrough]];
    case 407:  // sthx
      flags |= BackPatchInfo::FLAG_SIZE_16;
      break;
    case 662:  // stwbrx
      flags |= BackPatchInfo::FLAG_REVERSE | BackPatchInfo::FLAG_SIZE_32;
      break;
    case 918:  // sthbrx
      flags |= BackPatchInfo::FLAG_REVERSE | BackPatchInfo::FLAG_SIZE_16;
      break;
    }
    break;
  case 37:  // stwu
    update = true;
    [[fallthrough]];
  case 36:  // stw
    flags |= BackPatchInfo::FLAG_SIZE_32;
    break;
  case 39:  // stbu
    update = true;
    [[fallthrough]];
  case 38:  // stb
    flags |= BackPatchInfo::FLAG_SIZE_8;
    break;
  case 45:  // sthu
    update = true;
    [[fallthrough]];
  case 44:  // sth
    flags |= BackPatchInfo::FLAG_SIZE_16;
    break;
  }

  SafeStoreFromReg(update ? a : (a ? a : -1), s, regOffset, flags, offset);

  if (update)
  {
    gpr.BindToRegister(a, false);

    ARM64Reg WA = gpr.GetReg();
    ARM64Reg RB = {};
    ARM64Reg RA = gpr.R(a);
    if (regOffset != -1)
      RB = gpr.R(regOffset);
    if (regOffset == -1)
    {
      ADDI2R(RA, RA, offset, WA);
    }
    else
    {
      ADD(RA, RA, RB);
    }
    gpr.Unlock(WA);
  }
}

void JitArm64::lmw(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(!jo.fastmem || jo.memcheck);

  u32 a = inst.RA;

  ARM64Reg WA = gpr.GetReg();
  ARM64Reg XA = EncodeRegTo64(WA);
  if (a)
  {
    ADDI2R(WA, gpr.R(a), inst.SIMM_16, WA);
    ADD(XA, XA, MEM_REG);
  }
  else
  {
    ADDI2R(XA, MEM_REG, (u32)(s32)(s16)inst.SIMM_16, XA);
  }

  for (int i = inst.RD; i < 32; i++)
  {
    int remaining = 32 - i;
    if (remaining >= 4)
    {
      gpr.BindToRegister(i + 3, false);
      gpr.BindToRegister(i + 2, false);
      gpr.BindToRegister(i + 1, false);
      gpr.BindToRegister(i, false);
      ARM64Reg RX4 = gpr.R(i + 3);
      ARM64Reg RX3 = gpr.R(i + 2);
      ARM64Reg RX2 = gpr.R(i + 1);
      ARM64Reg RX1 = gpr.R(i);
      LDP(IndexType::Post, EncodeRegTo64(RX1), EncodeRegTo64(RX3), XA, 16);
      REV32(EncodeRegTo64(RX1), EncodeRegTo64(RX1));
      REV32(EncodeRegTo64(RX3), EncodeRegTo64(RX3));
      LSR(EncodeRegTo64(RX2), EncodeRegTo64(RX1), 32);
      LSR(EncodeRegTo64(RX4), EncodeRegTo64(RX3), 32);
      i += 3;
    }
    else if (remaining >= 2)
    {
      gpr.BindToRegister(i + 1, false);
      gpr.BindToRegister(i, false);
      ARM64Reg RX2 = gpr.R(i + 1);
      ARM64Reg RX1 = gpr.R(i);
      LDP(IndexType::Post, RX1, RX2, XA, 8);
      REV32(RX1, RX1);
      REV32(RX2, RX2);
      ++i;
    }
    else
    {
      gpr.BindToRegister(i, false);
      ARM64Reg RX = gpr.R(i);
      LDR(IndexType::Post, RX, XA, 4);
      REV32(RX, RX);
    }
  }

  gpr.Unlock(WA);
}

void JitArm64::stmw(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(!jo.fastmem || jo.memcheck);

  u32 a = inst.RA;

  ARM64Reg WA = gpr.GetReg();
  ARM64Reg XA = EncodeRegTo64(WA);
  ARM64Reg WB = gpr.GetReg();

  if (a)
  {
    ADDI2R(WA, gpr.R(a), inst.SIMM_16, WA);
    ADD(XA, XA, MEM_REG);
  }
  else
  {
    ADDI2R(XA, MEM_REG, (u32)(s32)(s16)inst.SIMM_16, XA);
  }

  for (int i = inst.RD; i < 32; i++)
  {
    ARM64Reg RX = gpr.R(i);
    REV32(WB, RX);
    STR(IndexType::Unsigned, WB, XA, (i - inst.RD) * 4);
  }

  gpr.Unlock(WA, WB);
}

void JitArm64::dcbx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  u32 a = inst.RA, b = inst.RB;

  // Check if the next instructions match a known looping pattern:
  // - dcbx rX
  // - addi rX,rX,32
  // - bdnz+ -8
  const bool make_loop = a == 0 && b != 0 && CanMergeNextInstructions(2) &&
                         (js.op[1].inst.hex & 0xfc00'ffff) == 0x38000020 &&
                         js.op[1].inst.RA_6 == b && js.op[1].inst.RD_2 == b &&
                         js.op[2].inst.hex == 0x4200fff8;

  gpr.Lock(ARM64Reg::W0);
  if (make_loop)
    gpr.Lock(ARM64Reg::W1);

  ARM64Reg WA = gpr.GetReg();

  if (make_loop)
    gpr.BindToRegister(b, true);

  ARM64Reg loop_counter = ARM64Reg::INVALID_REG;
  if (make_loop)
  {
    // We'll execute somewhere between one single cacheline invalidation and however many are needed
    // to reduce the downcount to zero, never exceeding the amount requested by the game.
    // To stay consistent with the rest of the code we adjust the involved registers (CTR and Rb)
    // by the amount of cache lines we invalidate minus one -- since we'll run the regular addi and
    // bdnz afterwards! So if we invalidate a single cache line, we don't adjust the registers at
    // all, if we invalidate 2 cachelines we adjust the registers by one step, and so on.

    ARM64Reg reg_cycle_count = gpr.GetReg();
    ARM64Reg reg_downcount = gpr.GetReg();
    loop_counter = ARM64Reg::W1;
    ARM64Reg WB = ARM64Reg::W0;

    // Figure out how many loops we want to do.
    const u8 cycle_count_per_loop =
        js.op[0].opinfo->numCycles + js.op[1].opinfo->numCycles + js.op[2].opinfo->numCycles;

    LDR(IndexType::Unsigned, reg_downcount, PPC_REG, PPCSTATE_OFF(downcount));
    MOVI2R(WA, 0);
    CMP(reg_downcount, 0);                                          // if (downcount <= 0)
    FixupBranch downcount_is_zero_or_negative = B(CCFlags::CC_LE);  // only do 1 invalidation; else:
    LDR(IndexType::Unsigned, loop_counter, PPC_REG, PPCSTATE_OFF_SPR(SPR_CTR));
    MOVI2R(reg_cycle_count, cycle_count_per_loop);
    SDIV(WB, reg_downcount, reg_cycle_count);  // WB = downcount / cycle_count
    SUB(WA, loop_counter, 1);                  // WA = CTR - 1
    // ^ Note that this CTR-1 implicitly handles the CTR == 0 case correctly.
    CMP(WB, WA);
    CSEL(WA, WB, WA, CCFlags::CC_LO);  // WA = min(WB, WA)

    // WA now holds the amount of loops to execute minus 1, which is the amount we need to adjust
    // downcount, CTR, and Rb by to exit the loop construct with the right values in those
    // registers.

    // CTR -= WA
    SUB(loop_counter, loop_counter, WA);
    STR(IndexType::Unsigned, loop_counter, PPC_REG, PPCSTATE_OFF_SPR(SPR_CTR));

    // downcount -= (WA * reg_cycle_count)
    MUL(WB, WA, reg_cycle_count);
    // ^ Note that this cannot overflow because it's limited by (downcount/cycle_count).
    SUB(reg_downcount, reg_downcount, WB);
    STR(IndexType::Unsigned, reg_downcount, PPC_REG, PPCSTATE_OFF(downcount));

    SetJumpTarget(downcount_is_zero_or_negative);

    // Load the loop_counter register with the amount of invalidations to execute.
    ADD(loop_counter, WA, 1);

    gpr.Unlock(reg_cycle_count, reg_downcount);
  }

  ARM64Reg effective_addr = ARM64Reg::W0;
  ARM64Reg physical_addr = gpr.GetReg();

  if (a)
    ADD(effective_addr, gpr.R(a), gpr.R(b));
  else
    MOV(effective_addr, gpr.R(b));

  if (make_loop)
  {
    // This is the best place to adjust Rb to what it should be since WA still has the
    // adjusted loop count and we're done reading from Rb.
    ADD(gpr.R(b), gpr.R(b), WA, ArithOption(WA, ShiftType::LSL, 5));  // Rb += (WA * 32)
  }

  // Translate effective address to physical address.
  const u8* loop_start = GetCodePtr();
  FixupBranch bat_lookup_failed;
  if (MSR.IR)
  {
    bat_lookup_failed =
        BATAddressLookup(physical_addr, effective_addr, WA, PowerPC::ibat_table.data());
    BFI(physical_addr, effective_addr, 0, PowerPC::BAT_INDEX_SHIFT);
  }

  // Check whether a JIT cache line needs to be invalidated.
  LSR(physical_addr, physical_addr, 5 + 5);  // >> 5 for cache line size, >> 5 for width of bitset
  MOVP2R(EncodeRegTo64(WA), GetBlockCache()->GetBlockBitSet());
  LDR(physical_addr, EncodeRegTo64(WA), ArithOption(EncodeRegTo64(physical_addr), true));

  LSR(WA, effective_addr, 5);  // mask sizeof cacheline, & 0x1f is the position within the bitset

  LSRV(physical_addr, physical_addr, WA);  // move current bit to bit 0

  FixupBranch bit_not_set = TBZ(physical_addr, 0);
  FixupBranch invalidate_needed = B();
  SetJumpTarget(bit_not_set);

  if (make_loop)
  {
    ADD(effective_addr, effective_addr, 32);
    SUBS(loop_counter, loop_counter, 1);
    B(CCFlags::CC_NEQ, loop_start);
  }

  SwitchToFarCode();
  SetJumpTarget(invalidate_needed);
  if (MSR.IR)
    SetJumpTarget(bat_lookup_failed);

  BitSet32 gprs_to_push = gpr.GetCallerSavedUsed();
  BitSet32 fprs_to_push = fpr.GetCallerSavedUsed();
  gprs_to_push[DecodeReg(effective_addr)] = false;
  gprs_to_push[DecodeReg(physical_addr)] = false;
  gprs_to_push[DecodeReg(WA)] = false;
  if (make_loop)
    gprs_to_push[DecodeReg(loop_counter)] = false;

  ABI_PushRegisters(gprs_to_push);
  m_float_emit.ABI_PushRegisters(fprs_to_push, WA);

  // The function call arguments are already in the correct registers
  if (make_loop)
    MOVP2R(ARM64Reg::X8, &JitInterface::InvalidateICacheLines);
  else
    MOVP2R(ARM64Reg::X8, &JitInterface::InvalidateICacheLine);
  BLR(ARM64Reg::X8);

  m_float_emit.ABI_PopRegisters(fprs_to_push, WA);
  ABI_PopRegisters(gprs_to_push);

  FixupBranch near_addr = B();
  SwitchToNearCode();
  SetJumpTarget(near_addr);

  gpr.Unlock(effective_addr, physical_addr, WA);
  if (make_loop)
    gpr.Unlock(loop_counter);
}

void JitArm64::dcbt(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  // Prefetch. Since we don't emulate the data cache, we don't need to do anything.

  // If a dcbst follows a dcbt, it probably isn't a case of dynamic code
  // modification, so don't bother invalidating the jit block cache.
  // This is important because invalidating the block cache when we don't
  // need to is terrible for performance.
  // (Invalidating the jit block cache on dcbst is a heuristic.)
  if (CanMergeNextInstructions(1) && js.op[1].inst.OPCD == 31 && js.op[1].inst.SUBOP10 == 54 &&
      js.op[1].inst.RA == inst.RA && js.op[1].inst.RB == inst.RB)
  {
    js.skipInstructions = 1;
  }
}

void JitArm64::dcbz(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(jo.memcheck || !jo.fastmem_arena);
  FALLBACK_IF(SConfig::GetInstance().bLowDCBZHack);

  int a = inst.RA, b = inst.RB;

  gpr.Lock(ARM64Reg::W0, ARM64Reg::W30);

  ARM64Reg addr_reg = ARM64Reg::W0;

  if (a)
  {
    bool is_imm_a, is_imm_b;
    is_imm_a = gpr.IsImm(a);
    is_imm_b = gpr.IsImm(b);
    if (is_imm_a && is_imm_b)
    {
      // full imm_addr
      u32 imm_addr = gpr.GetImm(b) + gpr.GetImm(a);
      MOVI2R(addr_reg, imm_addr & ~31);
    }
    else if (is_imm_a || is_imm_b)
    {
      // Only one register is an immediate
      ARM64Reg base = is_imm_a ? gpr.R(b) : gpr.R(a);
      u32 imm_offset = is_imm_a ? gpr.GetImm(a) : gpr.GetImm(b);
      ADDI2R(addr_reg, base, imm_offset, addr_reg);
      AND(addr_reg, addr_reg, LogicalImm(~31, 32));
    }
    else
    {
      // Both are registers
      ADD(addr_reg, gpr.R(a), gpr.R(b));
      AND(addr_reg, addr_reg, LogicalImm(~31, 32));
    }
  }
  else
  {
    // RA isn't used, only RB
    if (gpr.IsImm(b))
    {
      u32 imm_addr = gpr.GetImm(b);
      MOVI2R(addr_reg, imm_addr & ~31);
    }
    else
    {
      AND(addr_reg, gpr.R(b), LogicalImm(~31, 32));
    }
  }

  BitSet32 gprs_to_push = gpr.GetCallerSavedUsed();
  BitSet32 fprs_to_push = fpr.GetCallerSavedUsed();
  gprs_to_push[DecodeReg(ARM64Reg::W0)] = 0;

  EmitBackpatchRoutine(BackPatchInfo::FLAG_ZERO_256, true, true, ARM64Reg::W0,
                       EncodeRegTo64(addr_reg), gprs_to_push, fprs_to_push);

  gpr.Unlock(ARM64Reg::W0, ARM64Reg::W30);
}

void JitArm64::eieio(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  // optimizeGatherPipe generally postpones FIFO checks to the end of the JIT block,
  // which is generally safe. However postponing FIFO writes across eieio instructions
  // is incorrect (would crash NBA2K11 strap screen if we improve our FIFO detection).
  if (jo.optimizeGatherPipe && js.fifoBytesSinceCheck > 0)
    js.mustCheckFifo = true;
}
