//===-- VEAsmPrinter.cpp - VE LLVM assembly writer ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to GAS-format SPARC assembly language.
//
//===----------------------------------------------------------------------===//

#include "InstPrinter/VEInstPrinter.h"
#include "MCTargetDesc/VEMCExpr.h"
#include "VE.h"
#include "VEInstrInfo.h"
#include "VETargetMachine.h"
#include "MCTargetDesc/VETargetStreamer.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
  class VEAsmPrinter : public AsmPrinter {
    VETargetStreamer &getTargetStreamer() {
      return static_cast<VETargetStreamer &>(
          *OutStreamer->getTargetStreamer());
    }
  public:
    explicit VEAsmPrinter(TargetMachine &TM,
                             std::unique_ptr<MCStreamer> Streamer)
        : AsmPrinter(TM, std::move(Streamer)) {}

    StringRef getPassName() const override { return "VE Assembly Printer"; }

    void printOperand(const MachineInstr *MI, int opNum, raw_ostream &OS);
    void printMemOperand(const MachineInstr *MI, int opNum, raw_ostream &OS,
                         const char *Modifier = nullptr);
    void printMemHmOperand(const MachineInstr *MI, int opNum, raw_ostream &OS,
                           const char *Modifier = nullptr);

    void EmitFunctionBodyStart() override;
    void EmitInstruction(const MachineInstr *MI) override;

    static const char *getRegisterName(unsigned RegNo) {
      return VEInstPrinter::getRegisterName(RegNo);
    }

    bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                         unsigned AsmVariant, const char *ExtraCode,
                         raw_ostream &O) override;
    bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                               unsigned AsmVariant, const char *ExtraCode,
                               raw_ostream &O) override;

    void LowerGETGOTAndEmitMCInsts(const MachineInstr *MI,
                                   const MCSubtargetInfo &STI);
    void LowerGETFunPLTAndEmitMCInsts(const MachineInstr *MI,
                                      const MCSubtargetInfo &STI);

  };
} // end of anonymous namespace

static MCOperand createVEMCOperand(VEMCExpr::VariantKind Kind,
                                      MCSymbol *Sym, MCContext &OutContext) {
  const MCSymbolRefExpr *MCSym = MCSymbolRefExpr::create(Sym,
                                                         OutContext);
  const VEMCExpr *expr = VEMCExpr::create(Kind, MCSym, OutContext);
  return MCOperand::createExpr(expr);

}

static MCOperand createGOTRelExprOp(VEMCExpr::VariantKind Kind,
                                    MCSymbol *GOTLabel,
                                    MCContext &OutContext)
{
  const MCSymbolRefExpr *GOT = MCSymbolRefExpr::create(GOTLabel, OutContext);
  const VEMCExpr *expr = VEMCExpr::create(Kind, GOT, OutContext);
  return MCOperand::createExpr(expr);
}

static void EmitSIC(MCStreamer &OutStreamer,
                    MCOperand &RD, const MCSubtargetInfo &STI) {
  MCInst SICInst;
  SICInst.setOpcode(VE::SIC);
  SICInst.addOperand(RD);
  OutStreamer.EmitInstruction(SICInst, STI);
}

static void EmitLEAzzi(MCStreamer &OutStreamer,
                    MCOperand &Imm, MCOperand &RD,
                    const MCSubtargetInfo &STI)
{
  MCInst LEAInst;
  LEAInst.setOpcode(VE::LEAzzi);
  LEAInst.addOperand(RD);
  LEAInst.addOperand(Imm);
  OutStreamer.EmitInstruction(LEAInst, STI);
}

static void EmitLEASLzzi(MCStreamer &OutStreamer,
                      MCOperand &Imm, MCOperand &RD,
                      const MCSubtargetInfo &STI)
{
  MCInst LEASLInst;
  LEASLInst.setOpcode(VE::LEASLzzi);
  LEASLInst.addOperand(RD);
  LEASLInst.addOperand(Imm);
  OutStreamer.EmitInstruction(LEASLInst, STI);
}

static void EmitLEAzii(MCStreamer &OutStreamer,
                       MCOperand &RS1, MCOperand &Imm, MCOperand &RD,
                       const MCSubtargetInfo &STI)
{
  MCInst LEAInst;
  LEAInst.setOpcode(VE::LEAzii);
  LEAInst.addOperand(RD);
  LEAInst.addOperand(RS1);
  LEAInst.addOperand(Imm);
  OutStreamer.EmitInstruction(LEAInst, STI);
}

static void EmitLEASLrri(MCStreamer &OutStreamer,
                         MCOperand &RS1, MCOperand &RS2,
                         MCOperand &Imm, MCOperand &RD,
                         const MCSubtargetInfo &STI)
{
  MCInst LEASLInst;
  LEASLInst.setOpcode(VE::LEASLrri);
  LEASLInst.addOperand(RS1);
  LEASLInst.addOperand(RS2);
  LEASLInst.addOperand(RD);
  LEASLInst.addOperand(Imm);
  OutStreamer.EmitInstruction(LEASLInst, STI);
}

static void EmitBinary(MCStreamer &OutStreamer, unsigned Opcode,
                       MCOperand &RS1, MCOperand &Src2, MCOperand &RD,
                       const MCSubtargetInfo &STI)
{
  MCInst Inst;
  Inst.setOpcode(Opcode);
  Inst.addOperand(RD);
  Inst.addOperand(RS1);
  Inst.addOperand(Src2);
  OutStreamer.EmitInstruction(Inst, STI);
}

static void EmitANDrm0(MCStreamer &OutStreamer,
                       MCOperand &RS1, MCOperand &Imm, MCOperand &RD,
                       const MCSubtargetInfo &STI) {
  EmitBinary(OutStreamer, VE::ANDrm0, RS1, Imm, RD, STI);
}

static void EmitOR(MCStreamer &OutStreamer,
                   MCOperand &RS1, MCOperand &Imm, MCOperand &RD,
                   const MCSubtargetInfo &STI) {
  EmitBinary(OutStreamer, VE::ORri, RS1, Imm, RD, STI);
}

static void EmitADD(MCStreamer &OutStreamer,
                    MCOperand &RS1, MCOperand &RS2, MCOperand &RD,
                    const MCSubtargetInfo &STI) {
  EmitBinary(OutStreamer, VE::ADDrr, RS1, RS2, RD, STI);
}

static void EmitSHL(MCStreamer &OutStreamer,
                    MCOperand &RS1, MCOperand &Imm, MCOperand &RD,
                    const MCSubtargetInfo &STI) {
  EmitBinary(OutStreamer, VE::SLLri, RS1, Imm, RD, STI);
}

static void EmitHiLo(MCStreamer &OutStreamer,  MCSymbol *GOTSym,
                     VEMCExpr::VariantKind HiKind,
                     VEMCExpr::VariantKind LoKind,
                     MCOperand &RD,
                     MCContext &OutContext,
                     const MCSubtargetInfo &STI) {

  MCOperand hi = createVEMCOperand(HiKind, GOTSym, OutContext);
  MCOperand lo = createVEMCOperand(LoKind, GOTSym, OutContext);
  MCOperand ci32 = MCOperand::createImm(32);
  EmitLEAzzi(OutStreamer, lo, RD, STI);
  EmitANDrm0(OutStreamer, RD, ci32, RD, STI);
  EmitLEASLzzi(OutStreamer, hi, RD, STI);
}

void VEAsmPrinter::LowerGETGOTAndEmitMCInsts(const MachineInstr *MI,
                                             const MCSubtargetInfo &STI)
{
  MCSymbol *GOTLabel   =
    OutContext.getOrCreateSymbol(Twine("_GLOBAL_OFFSET_TABLE_"));

  const MachineOperand &MO = MI->getOperand(0);
  MCOperand MCRegOP = MCOperand::createReg(MO.getReg());


  if (!isPositionIndependent()) {
    // Just load the address of GOT to MCRegOP.
    switch(TM.getCodeModel()) {
    default:
      llvm_unreachable("Unsupported absolute code model");
    case CodeModel::Small:
      EmitHiLo(*OutStreamer, GOTLabel,
               VEMCExpr::VK_VE_HI, VEMCExpr::VK_VE_LO,
               MCRegOP, OutContext, STI);
      break;
    case CodeModel::Medium: {
      EmitHiLo(*OutStreamer, GOTLabel,
               VEMCExpr::VK_VE_H44, VEMCExpr::VK_VE_M44,
               MCRegOP, OutContext, STI);
      MCOperand imm = MCOperand::createExpr(MCConstantExpr::create(12,
                                                                   OutContext));
      EmitSHL(*OutStreamer, MCRegOP, imm, MCRegOP, STI);
      MCOperand lo = createVEMCOperand(VEMCExpr::VK_VE_L44,
                                          GOTLabel, OutContext);
      EmitOR(*OutStreamer, MCRegOP, lo, MCRegOP, STI);
      break;
    }
    case CodeModel::Large: {
      EmitHiLo(*OutStreamer, GOTLabel,
               VEMCExpr::VK_VE_HH, VEMCExpr::VK_VE_HM,
               MCRegOP, OutContext, STI);
      MCOperand imm = MCOperand::createExpr(MCConstantExpr::create(32,
                                                                   OutContext));
      EmitSHL(*OutStreamer, MCRegOP, imm, MCRegOP, STI);
      // Use register %s15 to load the lower 32 bits.
      MCOperand RegGOT = MCOperand::createReg(VE::SX15);
      EmitHiLo(*OutStreamer, GOTLabel,
               VEMCExpr::VK_VE_HI, VEMCExpr::VK_VE_LO,
               RegGOT, OutContext, STI);
      EmitADD(*OutStreamer, MCRegOP, RegGOT, MCRegOP, STI);
    }
    }
    return;
  }

  MCOperand RegGOT   = MCOperand::createReg(VE::SX15);  // GOT
  MCOperand RegPLT   = MCOperand::createReg(VE::SX16);  // PLT

  // lea %got, _GLOBAL_OFFSET_TABLE_@PC_LO(-24)
  // and %got, %got, (32)0
  // sic %plt
  // lea.sl %got, _GLOBAL_OFFSET_TABLE_@PC_HI(%got, %plt)
  MCOperand cim24 = MCOperand::createImm(-24);
  MCOperand loImm = createGOTRelExprOp(VEMCExpr::VK_VE_PCLO,
                                       GOTLabel,
                                       OutContext);
  EmitLEAzii(*OutStreamer, cim24, loImm, MCRegOP, STI);
  MCOperand ci32 = MCOperand::createImm(32);
  EmitANDrm0(*OutStreamer, MCRegOP, ci32, MCRegOP, STI);
  EmitSIC(*OutStreamer, RegPLT, STI);
  MCOperand hiImm = createGOTRelExprOp(VEMCExpr::VK_VE_PCHI,
                                       GOTLabel,
                                       OutContext);
  EmitLEASLrri(*OutStreamer, RegGOT, RegPLT, hiImm, MCRegOP, STI);
}

void VEAsmPrinter::LowerGETFunPLTAndEmitMCInsts(const MachineInstr *MI,
                                                const MCSubtargetInfo &STI)
{
  const MachineOperand &MO = MI->getOperand(0);
  MCOperand MCRegOP = MCOperand::createReg(MO.getReg());
  const MachineOperand &Addr = MI->getOperand(1);
  MCSymbol* AddrSym = nullptr;

  switch (Addr.getType()) {
  default:
    llvm_unreachable ("<unknown operand type>");
    return;
  case MachineOperand::MO_MachineBasicBlock:
    report_fatal_error("MBB is not supporeted yet");
    return;
  case MachineOperand::MO_ConstantPoolIndex:
    report_fatal_error("ConstantPool is not supporeted yet");
    return;
  case MachineOperand::MO_ExternalSymbol:
    AddrSym = GetExternalSymbolSymbol(Addr.getSymbolName());
    break;
  case MachineOperand::MO_GlobalAddress:
    AddrSym = getSymbol(Addr.getGlobal());
    break;
  }

  if (!isPositionIndependent()) {
    llvm_unreachable("Unsupported uses of %plt in not PIC code");
    return;
  }

  MCOperand RegPLT   = MCOperand::createReg(VE::SX16);  // PLT

  // lea %dst, %plt_lo(func)(-24)
  // and %dst, %dst, (32)0
  // sic %plt                            ; FIXME: is it safe to use %plt here?
  // lea.sl %dst, %plt_hi(func)(%dst, %plt)
  MCOperand cim24 = MCOperand::createImm(-24);
  MCOperand loImm = createGOTRelExprOp(VEMCExpr::VK_VE_PLTLO,
                                       AddrSym,
                                       OutContext);
  EmitLEAzii(*OutStreamer, cim24, loImm, MCRegOP, STI);
  MCOperand ci32 = MCOperand::createImm(32);
  EmitANDrm0(*OutStreamer, MCRegOP, ci32, MCRegOP, STI);
  EmitSIC(*OutStreamer, RegPLT, STI);
  MCOperand hiImm = createGOTRelExprOp(VEMCExpr::VK_VE_PLTHI,
                                       AddrSym,
                                       OutContext);
  EmitLEASLrri(*OutStreamer, MCRegOP, RegPLT, hiImm, MCRegOP, STI);
}

void VEAsmPrinter::EmitInstruction(const MachineInstr *MI)
{

  switch (MI->getOpcode()) {
  default: break;
  case TargetOpcode::DBG_VALUE:
    // FIXME: Debug Value.
    return;
  case VE::GETGOT:
    LowerGETGOTAndEmitMCInsts(MI, getSubtargetInfo());
    return;
  case VE::GETFUNPLT:
    LowerGETFunPLTAndEmitMCInsts(MI, getSubtargetInfo());
    return;
  }
  MachineBasicBlock::const_instr_iterator I = MI->getIterator();
  MachineBasicBlock::const_instr_iterator E = MI->getParent()->instr_end();
  do {
    MCInst TmpInst;
    LowerVEMachineInstrToMCInst(&*I, TmpInst, *this);
    EmitToStreamer(*OutStreamer, TmpInst);
  } while ((++I != E) && I->isInsideBundle()); // Delay slot check.
}

void VEAsmPrinter::EmitFunctionBodyStart() {
#if 0
  const MachineRegisterInfo &MRI = MF->getRegInfo();
  const unsigned globalRegs[] = { SP::G2, SP::G3, SP::G6, SP::G7, 0 };
  for (unsigned i = 0; globalRegs[i] != 0; ++i) {
    unsigned reg = globalRegs[i];
    if (MRI.use_empty(reg))
      continue;

    if  (reg == SP::G6 || reg == SP::G7)
      getTargetStreamer().emitVERegisterIgnore(reg);
    else
      getTargetStreamer().emitVERegisterScratch(reg);
  }
#endif
}

void VEAsmPrinter::printOperand(const MachineInstr *MI, int opNum,
                                   raw_ostream &O) {
  const DataLayout &DL = getDataLayout();
  const MachineOperand &MO = MI->getOperand (opNum);
  VEMCExpr::VariantKind TF = (VEMCExpr::VariantKind) MO.getTargetFlags();

#ifndef NDEBUG
  // Verify the target flags.
  if (MO.isGlobal() || MO.isSymbol() || MO.isCPI()) {
#if 0
    if (MI->getOpcode() == SP::CALL)
      assert(TF == VEMCExpr::VK_VE_None &&
             "Cannot handle target flags on call address");
    else if (MI->getOpcode() == VE::LEASL)
      assert((TF == VEMCExpr::VK_VE_HI
              || TF == VEMCExpr::VK_VE_H44
              || TF == VEMCExpr::VK_VE_HH
              || TF == VEMCExpr::VK_VE_TLS_GD_HI22
              || TF == VEMCExpr::VK_VE_TLS_LDM_HI22
              || TF == VEMCExpr::VK_VE_TLS_LDO_HIX22
              || TF == VEMCExpr::VK_VE_TLS_IE_HI22
              || TF == VEMCExpr::VK_VE_TLS_LE_HIX22) &&
             "Invalid target flags for address operand on sethi");
    else if (MI->getOpcode() == SP::TLS_CALL)
      assert((TF == VEMCExpr::VK_VE_None
              || TF == VEMCExpr::VK_VE_TLS_GD_CALL
              || TF == VEMCExpr::VK_VE_TLS_LDM_CALL) &&
             "Cannot handle target flags on tls call address");
    else if (MI->getOpcode() == SP::TLS_ADDrr)
      assert((TF == VEMCExpr::VK_VE_TLS_GD_ADD
              || TF == VEMCExpr::VK_VE_TLS_LDM_ADD
              || TF == VEMCExpr::VK_VE_TLS_LDO_ADD
              || TF == VEMCExpr::VK_VE_TLS_IE_ADD) &&
             "Cannot handle target flags on add for TLS");
    else if (MI->getOpcode() == SP::TLS_LDrr)
      assert(TF == VEMCExpr::VK_VE_TLS_IE_LD &&
             "Cannot handle target flags on ld for TLS");
    else if (MI->getOpcode() == SP::TLS_LDXrr)
      assert(TF == VEMCExpr::VK_VE_TLS_IE_LDX &&
             "Cannot handle target flags on ldx for TLS");
    else if (MI->getOpcode() == SP::XORri || MI->getOpcode() == SP::XORXri)
      assert((TF == VEMCExpr::VK_VE_TLS_LDO_LOX10
              || TF == VEMCExpr::VK_VE_TLS_LE_LOX10) &&
             "Cannot handle target flags on xor for TLS");
    else
      assert((TF == VEMCExpr::VK_VE_LO
              || TF == VEMCExpr::VK_VE_M44
              || TF == VEMCExpr::VK_VE_L44
              || TF == VEMCExpr::VK_VE_HM
              || TF == VEMCExpr::VK_VE_TLS_GD_LO10
              || TF == VEMCExpr::VK_VE_TLS_LDM_LO10
              || TF == VEMCExpr::VK_VE_TLS_IE_LO10 ) &&
             "Invalid target flags for small address operand");
#endif
  }
#endif


  bool CloseParen = VEMCExpr::printVariantKind(O, TF);

  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    O << "%" << StringRef(getRegisterName(MO.getReg())).lower();
    break;

  case MachineOperand::MO_Immediate:
    O << (int)MO.getImm();
    break;
  case MachineOperand::MO_MachineBasicBlock:
    MO.getMBB()->getSymbol()->print(O, MAI);
    return;
  case MachineOperand::MO_GlobalAddress:
    getSymbol(MO.getGlobal())->print(O, MAI);
    break;
  case MachineOperand::MO_BlockAddress:
    O <<  GetBlockAddressSymbol(MO.getBlockAddress())->getName();
    break;
  case MachineOperand::MO_ExternalSymbol:
    O << MO.getSymbolName();
    break;
  case MachineOperand::MO_ConstantPoolIndex:
    O << DL.getPrivateGlobalPrefix() << "CPI" << getFunctionNumber() << "_"
      << MO.getIndex();
    break;
  case MachineOperand::MO_Metadata:
    MO.getMetadata()->printAsOperand(O, MMI->getModule());
    break;
  default:
    llvm_unreachable("<unknown operand type>");
  }
  if (CloseParen) O << ")";
}

void VEAsmPrinter::printMemOperand(const MachineInstr *MI, int opNum,
                                      raw_ostream &O, const char *Modifier) {
  // If this is an ADD operand, emit it like normal operands.
  if (Modifier && !strcmp(Modifier, "arith")) {
    printOperand(MI, opNum, O);
    O << ", ";
    printOperand(MI, opNum+1, O);
    return;
  }

  if (MI->getOperand(opNum+1).isImm() &&
      MI->getOperand(opNum+1).getImm() == 0) {
    // don't print "+0"
  } else {
    printOperand(MI, opNum+1, O);
  }
  O << "(,";
  printOperand(MI, opNum, O);
  O << ")";
}

void VEAsmPrinter::printMemHmOperand(const MachineInstr *MI, int opNum,
                                      raw_ostream &O, const char *Modifier) {
  // If this is an ADD operand, emit it like normal operands.
  if (Modifier && !strcmp(Modifier, "arith")) {
    printOperand(MI, opNum, O);
    O << ", ";
    printOperand(MI, opNum+1, O);
    return;
  }

  if (MI->getOperand(opNum+1).isImm() &&
      MI->getOperand(opNum+1).getImm() == 0) {
    // don't print "+0"
  } else {
    printOperand(MI, opNum+1, O);
  }
  O << "(";
  printOperand(MI, opNum, O);
  O << ")";
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool VEAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                      unsigned AsmVariant,
                                      const char *ExtraCode,
                                      raw_ostream &O) {
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      // See if this is a generic print operand
      return AsmPrinter::PrintAsmOperand(MI, OpNo, AsmVariant, ExtraCode, O);
    case 'f':
    case 'r':
     break;
    }
  }

  printOperand(MI, OpNo, O);

  return false;
}

bool VEAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                            unsigned OpNo, unsigned AsmVariant,
                                            const char *ExtraCode,
                                            raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return true;  // Unknown modifier

  O << '[';
  printMemOperand(MI, OpNo, O);
  O << ']';

  return false;
}

// Force static initialization.
extern "C" void LLVMInitializeVEAsmPrinter() {
  RegisterAsmPrinter<VEAsmPrinter> X(getTheVETarget());
}
