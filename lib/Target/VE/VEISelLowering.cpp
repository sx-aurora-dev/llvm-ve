//===-- VEISelLowering.cpp - VE DAG Lowering Implementation ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that VE uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#include "VEISelLowering.h"
#include "MCTargetDesc/VEMCExpr.h"
#include "VEMachineFunctionInfo.h"
#include "VERegisterInfo.h"
#include "VETargetMachine.h"
// #include "VETargetObjectFile.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
using namespace llvm;

#define DEBUG_TYPE "ve-lower"

//===----------------------------------------------------------------------===//
// Calling Convention Implementation
//===----------------------------------------------------------------------===//

#include "VEGenCallingConv.inc"

SDValue
VETargetLowering::LowerBuildVector(SDValue Chain, SelectionDAG &DAG) const {
  auto & bvNode = *cast<BuildVectorSDNode>(Chain);

  SDLoc DL(Chain);

// VEC_BROADCAST
  bool allUndef = true;
  unsigned first_def = -1;
  for (unsigned i = 0; i < bvNode.getNumOperands(); ++i) {
    if (!bvNode.getOperand(i).isUndef()) {
      allUndef = false;
      first_def = i;
      break;
    }
  }

  if (allUndef) {
    return DAG.getNode(VEISD::VEC_BROADCAST, DL, Chain.getSimpleValueType(),
                                  bvNode.getOperand(0));
  }

  bool isBroadcast = true;
  for (unsigned i = first_def + 1; i < bvNode.getNumOperands(); ++i) {
    if (bvNode.getOperand(first_def) != bvNode.getOperand(i) && !bvNode.getOperand(i).isUndef()) {
      isBroadcast = false;
      break;
    }
  }

  if (isBroadcast) {
    return DAG.getNode(VEISD::VEC_BROADCAST, DL, Chain.getSimpleValueType(),
                                  bvNode.getOperand(first_def));
  }


// VEC_SEQ(stride) patterns
  /*if (!bvNode.isConstant()) {
    return Chain; // FIXME actually we want to Expand in this case
  }*/

  // identify a constant stride vector
  bool hasConstantStride = true;
  bool firstStride = true;
  int64_t stride = 0;
  int64_t lastElemValue = 0;
  MVT elemTy;

  for (unsigned i = 0; i < bvNode.getNumOperands(); ++i) {
    if (bvNode.getOperand(i).isUndef()) {
      lastElemValue += stride;
      continue;
    }

    // is this an immediate constant value?
    auto * constNumElem = dyn_cast<ConstantSDNode>(bvNode.getOperand(i));
    if (!constNumElem) {
      hasConstantStride = false;
      break;
    }

    // read value
    int64_t elemValue = constNumElem->getSExtValue();
    elemTy = constNumElem->getSimpleValueType(0);

    if (i > first_def && firstStride) {
      // first stride
      stride = (elemValue - lastElemValue) / (i - first_def);
      firstStride = false;
    } else if (i > first_def) {
      // later stride
      int64_t thisStride = elemValue - lastElemValue;
      if (thisStride != stride) {
        hasConstantStride = false;
        break;
      }
    }

    // track last elem value
    lastElemValue = elemValue;
  }

  // detected a proper stride pattern
  if (hasConstantStride) {
    return DAG.getNode(VEISD::VEC_SEQ, DL, Chain.getSimpleValueType(),
                                  DAG.getConstant(stride, DL, elemTy)); // TODO draw strideTy from elements
  }

  SDValue newVector = DAG.getNode(VEISD::VEC_BROADCAST, DL, Chain.getSimpleValueType(),
                                  bvNode.getOperand(0));

  for (unsigned i = 0; i < bvNode.getNumOperands(); ++i) {
    newVector = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, Chain.getSimpleValueType(),
        newVector,
        bvNode.getOperand(i),
        DAG.getConstant(i, DL, EVT::getIntegerVT(*DAG.getContext(), 64))
    );
  }

  return newVector;
}

SDValue
VETargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                              bool IsVarArg,
                              const SmallVectorImpl<ISD::OutputArg> &Outs,
                              const SmallVectorImpl<SDValue> &OutVals,
                              const SDLoc &DL, SelectionDAG &DAG) const {
  return LowerReturn_64(Chain, CallConv, IsVarArg, Outs, OutVals, DL, DAG);
}

#if 0
SDValue
VETargetLowering::LowerReturn_32(SDValue Chain, CallingConv::ID CallConv,
                                    bool IsVarArg,
                                    const SmallVectorImpl<ISD::OutputArg> &Outs,
                                    const SmallVectorImpl<SDValue> &OutVals,
                                    const SDLoc &DL, SelectionDAG &DAG) const {
#if 0
  MachineFunction &MF = DAG.getMachineFunction();
#endif

  // CCValAssign - represent the assignment of the return value to locations.
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analyze return values.
  CCInfo.AnalyzeReturn(Outs, RetCC_VE);

  SDValue Flag;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0, realRVLocIdx = 0;
       i != RVLocs.size();
       ++i, ++realRVLocIdx) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");

    SDValue Arg = OutVals[realRVLocIdx];

    if (VA.needsCustom()) {
      assert(VA.getLocVT() == MVT::v2i32);
      // Legalize ret v2i32 -> ret 2 x i32 (Basically: do what would
      // happen by default if this wasn't a legal type)

      SDValue Part0 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::i32,
                                  Arg,
                                  DAG.getConstant(0, DL, getVectorIdxTy(DAG.getDataLayout())));
      SDValue Part1 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::i32,
                                  Arg,
                                  DAG.getConstant(1, DL, getVectorIdxTy(DAG.getDataLayout())));

      Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), Part0, Flag);
      Flag = Chain.getValue(1);
      RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
      VA = RVLocs[++i]; // skip ahead to next loc
      Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), Part1,
                               Flag);
    } else
      Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), Arg, Flag);

    // Guarantee that all emitted copies are stuck together with flags.
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;  // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(VEISD::RET_FLAG, DL, MVT::Other, RetOps);
}
#endif

// Lower return values for the 64-bit ABI.
// Return values are passed the exactly the same way as function arguments.
SDValue
VETargetLowering::LowerReturn_64(SDValue Chain, CallingConv::ID CallConv,
                                    bool IsVarArg,
                                    const SmallVectorImpl<ISD::OutputArg> &Outs,
                                    const SmallVectorImpl<SDValue> &OutVals,
                                    const SDLoc &DL, SelectionDAG &DAG) const {
  // CCValAssign - represent the assignment of the return value to locations.
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analyze return values.
  CCInfo.AnalyzeReturn(Outs, RetCC_VE);

  SDValue Flag;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");
    SDValue OutVal = OutVals[i];

    // Integer return values must be sign or zero extended by the callee.
    switch (VA.getLocInfo()) {
    case CCValAssign::Full: break;
    case CCValAssign::SExt:
      OutVal = DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), OutVal);
      break;
    case CCValAssign::ZExt:
      OutVal = DAG.getNode(ISD::ZERO_EXTEND, DL, VA.getLocVT(), OutVal);
      break;
    case CCValAssign::AExt:
      OutVal = DAG.getNode(ISD::ANY_EXTEND, DL, VA.getLocVT(), OutVal);
      break;
    default:
      llvm_unreachable("Unknown loc info!");
    }

    // The custom bit on an i32 return value indicates that it should be passed
    // in the high bits of the register.
    if (VA.getValVT() == MVT::i32 && VA.needsCustom()) {
      OutVal = DAG.getNode(ISD::SHL, DL, MVT::i64, OutVal,
                           DAG.getConstant(32, DL, MVT::i32));

      // The next value may go in the low bits of the same register.
      // Handle both at once.
      if (i+1 < RVLocs.size() && RVLocs[i+1].getLocReg() == VA.getLocReg()) {
        SDValue NV = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i64, OutVals[i+1]);
        OutVal = DAG.getNode(ISD::OR, DL, MVT::i64, OutVal, NV);
        // Skip the next value, it's already done.
        ++i;
      }
    }

    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVal, Flag);

    // Guarantee that all emitted copies are stuck together with flags.
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;  // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(VEISD::RET_FLAG, DL, MVT::Other, RetOps);
}

SDValue VETargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  return LowerFormalArguments_64(Chain, CallConv, IsVarArg, Ins,
                                 DL, DAG, InVals);
}

#if 0
/// LowerFormalArguments32 - V8 uses a very simple ABI, where all values are
/// passed in either one or two GPRs, including FP values.  TODO: we should
/// pass FP values in FP registers for fastcc functions.
SDValue VETargetLowering::LowerFormalArguments_32(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  VEMachineFunctionInfo *FuncInfo = MF.getInfo<VEMachineFunctionInfo>();

  // Assign locations to all of the incoming arguments.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_VE);

  const unsigned StackOffset = 92;
  bool IsLittleEndian = DAG.getDataLayout().isLittleEndian();

  unsigned InIdx = 0;
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i, ++InIdx) {
    CCValAssign &VA = ArgLocs[i];

    if (Ins[InIdx].Flags.isSRet()) {
      if (InIdx != 0)
        report_fatal_error("sparc only supports sret on the first parameter");
      // Get SRet from [%fp+64].
      int FrameIdx = MF.getFrameInfo().CreateFixedObject(4, 64, true);
      SDValue FIPtr = DAG.getFrameIndex(FrameIdx, MVT::i32);
      SDValue Arg =
          DAG.getLoad(MVT::i32, dl, Chain, FIPtr, MachinePointerInfo());
      InVals.push_back(Arg);
      continue;
    }

    if (VA.isRegLoc()) {
      if (VA.needsCustom()) {
        assert(VA.getLocVT() == MVT::f64 || VA.getLocVT() == MVT::v2i32);

        unsigned VRegHi = RegInfo.createVirtualRegister(&VE::I32RegClass);
        MF.getRegInfo().addLiveIn(VA.getLocReg(), VRegHi);
        SDValue HiVal = DAG.getCopyFromReg(Chain, dl, VRegHi, MVT::i32);

        assert(i+1 < e);
        CCValAssign &NextVA = ArgLocs[++i];

        SDValue LoVal;
        if (NextVA.isMemLoc()) {
          int FrameIdx = MF.getFrameInfo().
            CreateFixedObject(4, StackOffset+NextVA.getLocMemOffset(),true);
          SDValue FIPtr = DAG.getFrameIndex(FrameIdx, MVT::i32);
          LoVal = DAG.getLoad(MVT::i32, dl, Chain, FIPtr, MachinePointerInfo());
        } else {
          unsigned loReg = MF.addLiveIn(NextVA.getLocReg(),
                                        &VE::I32RegClass);
          LoVal = DAG.getCopyFromReg(Chain, dl, loReg, MVT::i32);
        }

        if (IsLittleEndian)
          std::swap(LoVal, HiVal);

        SDValue WholeValue =
          DAG.getNode(ISD::BUILD_PAIR, dl, MVT::i64, LoVal, HiVal);
        WholeValue = DAG.getNode(ISD::BITCAST, dl, VA.getLocVT(), WholeValue);
        InVals.push_back(WholeValue);
        continue;
      }
      unsigned VReg = RegInfo.createVirtualRegister(&VE::I32RegClass);
      MF.getRegInfo().addLiveIn(VA.getLocReg(), VReg);
      SDValue Arg = DAG.getCopyFromReg(Chain, dl, VReg, MVT::i32);
      if (VA.getLocVT() == MVT::f32)
        Arg = DAG.getNode(ISD::BITCAST, dl, MVT::f32, Arg);
      else if (VA.getLocVT() != MVT::i32) {
        Arg = DAG.getNode(ISD::AssertSext, dl, MVT::i32, Arg,
                          DAG.getValueType(VA.getLocVT()));
        Arg = DAG.getNode(ISD::TRUNCATE, dl, VA.getLocVT(), Arg);
      }
      InVals.push_back(Arg);
      continue;
    }

    assert(VA.isMemLoc());

    unsigned Offset = VA.getLocMemOffset()+StackOffset;
    auto PtrVT = getPointerTy(DAG.getDataLayout());

    if (VA.needsCustom()) {
      assert(VA.getValVT() == MVT::f64 || VA.getValVT() == MVT::v2i32);
      // If it is double-word aligned, just load.
      if (Offset % 8 == 0) {
        int FI = MF.getFrameInfo().CreateFixedObject(8,
                                                     Offset,
                                                     true);
        SDValue FIPtr = DAG.getFrameIndex(FI, PtrVT);
        SDValue Load =
            DAG.getLoad(VA.getValVT(), dl, Chain, FIPtr, MachinePointerInfo());
        InVals.push_back(Load);
        continue;
      }

      int FI = MF.getFrameInfo().CreateFixedObject(4,
                                                   Offset,
                                                   true);
      SDValue FIPtr = DAG.getFrameIndex(FI, PtrVT);
      SDValue HiVal =
          DAG.getLoad(MVT::i32, dl, Chain, FIPtr, MachinePointerInfo());
      int FI2 = MF.getFrameInfo().CreateFixedObject(4,
                                                    Offset+4,
                                                    true);
      SDValue FIPtr2 = DAG.getFrameIndex(FI2, PtrVT);

      SDValue LoVal =
          DAG.getLoad(MVT::i32, dl, Chain, FIPtr2, MachinePointerInfo());

      if (IsLittleEndian)
        std::swap(LoVal, HiVal);

      SDValue WholeValue =
        DAG.getNode(ISD::BUILD_PAIR, dl, MVT::i64, LoVal, HiVal);
      WholeValue = DAG.getNode(ISD::BITCAST, dl, VA.getValVT(), WholeValue);
      InVals.push_back(WholeValue);
      continue;
    }

    int FI = MF.getFrameInfo().CreateFixedObject(4,
                                                 Offset,
                                                 true);
    SDValue FIPtr = DAG.getFrameIndex(FI, PtrVT);
    SDValue Load ;
    if (VA.getValVT() == MVT::i32 || VA.getValVT() == MVT::f32) {
      Load = DAG.getLoad(VA.getValVT(), dl, Chain, FIPtr, MachinePointerInfo());
    } else if (VA.getValVT() == MVT::f128) {
      report_fatal_error("SPARCv8 does not handle f128 in calls; "
                         "pass indirectly");
    } else {
      // We shouldn't see any other value types here.
      llvm_unreachable("Unexpected ValVT encountered in frame lowering.");
    }
    InVals.push_back(Load);
  }

  if (MF.getFunction().hasStructRetAttr()) {
    // Copy the SRet Argument to SRetReturnReg.
    VEMachineFunctionInfo *SFI = MF.getInfo<VEMachineFunctionInfo>();
    unsigned Reg = SFI->getSRetReturnReg();
    if (!Reg) {
      Reg = MF.getRegInfo().createVirtualRegister(&VE::I64RegClass);
      SFI->setSRetReturnReg(Reg);
    }
    SDValue Copy = DAG.getCopyToReg(DAG.getEntryNode(), dl, Reg, InVals[0]);
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Copy, Chain);
  }

  // Store remaining ArgRegs to the stack if this is a varargs function.
  if (isVarArg) {
    static const MCPhysReg ArgRegs[] = {
      VE::SX0, VE::SX1, VE::SX2, VE::SX3, VE::SX4, VE::SX5, VE::SX6, VE::SX7,
    };
    unsigned NumAllocated = CCInfo.getFirstUnallocated(ArgRegs);
    const MCPhysReg *CurArgReg = ArgRegs+NumAllocated, *ArgRegEnd = ArgRegs+6;
    unsigned ArgOffset = CCInfo.getNextStackOffset();
    if (NumAllocated == 6)
      ArgOffset += StackOffset;
    else {
      assert(!ArgOffset);
      ArgOffset = 68+4*NumAllocated;
    }

    // Remember the vararg offset for the va_start implementation.
    FuncInfo->setVarArgsFrameOffset(ArgOffset);

    std::vector<SDValue> OutChains;

    for (; CurArgReg != ArgRegEnd; ++CurArgReg) {
      unsigned VReg = RegInfo.createVirtualRegister(&VE::I32RegClass);
      MF.getRegInfo().addLiveIn(*CurArgReg, VReg);
      SDValue Arg = DAG.getCopyFromReg(DAG.getRoot(), dl, VReg, MVT::i32);

      int FrameIdx = MF.getFrameInfo().CreateFixedObject(4, ArgOffset,
                                                         true);
      SDValue FIPtr = DAG.getFrameIndex(FrameIdx, MVT::i32);

      OutChains.push_back(
          DAG.getStore(DAG.getRoot(), dl, Arg, FIPtr, MachinePointerInfo()));
      ArgOffset += 4;
    }

    if (!OutChains.empty()) {
      OutChains.push_back(Chain);
      Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
    }
  }

  return Chain;
}
#endif

// Lower formal arguments for the 64 bit ABI.
SDValue VETargetLowering::LowerFormalArguments_64(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();

  // Get the base offset of the incoming arguments stack space.
  unsigned ArgsBaseOffset = 176;
  // Get the size of the preserved arguments area
  unsigned ArgsPreserved = 64;

  // Analyze arguments according to CC_VE.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  // Allocate the preserved area first.
  CCInfo.AllocateStack(ArgsPreserved, 8);
  // We already allocated the preserved area, so the stack offset computed
  // by CC_VE would be correct now.
  CCInfo.AnalyzeFormalArguments(Ins, CC_VE);

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    if (VA.isRegLoc()) {
      // This argument is passed in a register.
      // All integer register arguments are promoted by the caller to i64.

      // Create a virtual register for the promoted live-in value.
      unsigned VReg = MF.addLiveIn(VA.getLocReg(),
                                   getRegClassFor(VA.getLocVT()));
      SDValue Arg = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());

      // Get the high bits for i32 struct elements.
      if (VA.getValVT() == MVT::i32 && VA.needsCustom())
        Arg = DAG.getNode(ISD::SRL, DL, VA.getLocVT(), Arg,
                          DAG.getConstant(32, DL, MVT::i32));

      // The caller promoted the argument, so insert an Assert?ext SDNode so we
      // won't promote the value again in this function.
      switch (VA.getLocInfo()) {
      case CCValAssign::SExt:
        Arg = DAG.getNode(ISD::AssertSext, DL, VA.getLocVT(), Arg,
                          DAG.getValueType(VA.getValVT()));
        break;
      case CCValAssign::ZExt:
        Arg = DAG.getNode(ISD::AssertZext, DL, VA.getLocVT(), Arg,
                          DAG.getValueType(VA.getValVT()));
        break;
      default:
        break;
      }

      // Truncate the register down to the argument type.
      if (VA.isExtInLoc())
        Arg = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), Arg);

      InVals.push_back(Arg);
      continue;
    }

    // The registers are exhausted. This argument was passed on the stack.
    assert(VA.isMemLoc());
    // The CC_VE_Full/Half functions compute stack offsets relative to the
    // beginning of the arguments area at %fp+176.
    unsigned Offset = VA.getLocMemOffset() + ArgsBaseOffset;
    unsigned ValSize = VA.getValVT().getSizeInBits() / 8;
    // Adjust offset for extended arguments, SPARC is big-endian.
    // The caller will have written the full slot with extended bytes, but we
    // prefer our own extending loads.
    if (VA.isExtInLoc())
      Offset += 8 - ValSize;
    int FI = MF.getFrameInfo().CreateFixedObject(ValSize, Offset, true);
    InVals.push_back(
        DAG.getLoad(VA.getValVT(), DL, Chain,
                    DAG.getFrameIndex(FI, getPointerTy(MF.getDataLayout())),
                    MachinePointerInfo::getFixedStack(MF, FI)));
  }

  if (!IsVarArg)
    return Chain;

  // This function takes variable arguments, some of which may have been passed
  // in registers %s0-%s8.
  //
  // The va_start intrinsic needs to know the offset to the first variable
  // argument.
  // TODO: need to calculate offset correctly once we support f128.
  unsigned ArgOffset = ArgLocs.size() * 8;
  VEMachineFunctionInfo *FuncInfo = MF.getInfo<VEMachineFunctionInfo>();
  // Skip the 176 bytes of register save area.
  FuncInfo->setVarArgsFrameOffset(ArgOffset + ArgsBaseOffset);

#if 0
// VE ABI requires to store values in stack by caller side.
// So no need to store varargs here.
  // Save the variable arguments that were passed in registers.
  // The caller is required to reserve stack space for 8 arguments regardless
  // of how many arguments were actually passed.
  SmallVector<SDValue, 8> OutChains;
  for (; ArgOffset < 8*8; ArgOffset += 8) {
    unsigned VReg = MF.addLiveIn(VE::SX0 + ArgOffset/8, &VE::I64RegClass);
    SDValue VArg = DAG.getCopyFromReg(Chain, DL, VReg, MVT::i64);
    int FI = MF.getFrameInfo().CreateFixedObject(8, ArgOffset + ArgsBaseOffset, true);
    auto PtrVT = getPointerTy(MF.getDataLayout());
    OutChains.push_back(
        DAG.getStore(Chain, DL, VArg, DAG.getFrameIndex(FI, PtrVT),
                     MachinePointerInfo::getFixedStack(MF, FI)));
  }

  if (!OutChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, OutChains);
#endif

  return Chain;
}

SDValue
VETargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                               SmallVectorImpl<SDValue> &InVals) const {
  return LowerCall_64(CLI, InVals);
}

#if 0
// Lower a call for the 32-bit ABI.
SDValue
VETargetLowering::LowerCall_32(TargetLowering::CallLoweringInfo &CLI,
                                  SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG                     = CLI.DAG;
  SDLoc &dl                             = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals     = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins   = CLI.Ins;
  SDValue Chain                         = CLI.Chain;
  SDValue Callee                        = CLI.Callee;
  bool &isTailCall                      = CLI.IsTailCall;
  CallingConv::ID CallConv              = CLI.CallConv;
  bool isVarArg                         = CLI.IsVarArg;

  // VE target does not yet support tail call optimization.
  isTailCall = false;

  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_VE);

  // Get the size of the outgoing arguments stack space requirement.
  unsigned ArgsSize = CCInfo.getNextStackOffset();

  // Keep stack frames 8-byte aligned.
  ArgsSize = (ArgsSize+7) & ~7;

  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();

  // Create local copies for byval args.
  SmallVector<SDValue, 8> ByValArgs;
  for (unsigned i = 0,  e = Outs.size(); i != e; ++i) {
    ISD::ArgFlagsTy Flags = Outs[i].Flags;
    if (!Flags.isByVal())
      continue;

    SDValue Arg = OutVals[i];
    unsigned Size = Flags.getByValSize();
    unsigned Align = Flags.getByValAlign();

    if (Size > 0U) {
      int FI = MFI.CreateStackObject(Size, Align, false);
      SDValue FIPtr = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      SDValue SizeNode = DAG.getConstant(Size, dl, MVT::i32);

      Chain = DAG.getMemcpy(Chain, dl, FIPtr, Arg, SizeNode, Align,
                            false,        // isVolatile,
                            (Size <= 32), // AlwaysInline if size <= 32,
                            false,        // isTailCall
                            MachinePointerInfo(), MachinePointerInfo());
      ByValArgs.push_back(FIPtr);
    }
    else {
      SDValue nullVal;
      ByValArgs.push_back(nullVal);
    }
  }

  Chain = DAG.getCALLSEQ_START(Chain, ArgsSize, 0, dl);

  SmallVector<std::pair<unsigned, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;

  const unsigned StackOffset = 92;
  bool hasStructRetAttr = false;
  // Walk the register/memloc assignments, inserting copies/loads.
  for (unsigned i = 0, realArgIdx = 0, byvalArgIdx = 0, e = ArgLocs.size();
       i != e;
       ++i, ++realArgIdx) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[realArgIdx];

    ISD::ArgFlagsTy Flags = Outs[realArgIdx].Flags;

    // Use local copy if it is a byval arg.
    if (Flags.isByVal()) {
      Arg = ByValArgs[byvalArgIdx++];
      if (!Arg) {
        continue;
      }
    }

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
    default: llvm_unreachable("Unknown loc info!");
    case CCValAssign::Full: break;
    case CCValAssign::SExt:
      Arg = DAG.getNode(ISD::SIGN_EXTEND, dl, VA.getLocVT(), Arg);
      break;
    case CCValAssign::ZExt:
      Arg = DAG.getNode(ISD::ZERO_EXTEND, dl, VA.getLocVT(), Arg);
      break;
    case CCValAssign::AExt:
      Arg = DAG.getNode(ISD::ANY_EXTEND, dl, VA.getLocVT(), Arg);
      break;
    case CCValAssign::BCvt:
      Arg = DAG.getNode(ISD::BITCAST, dl, VA.getLocVT(), Arg);
      break;
    }

    if (Flags.isSRet()) {
      assert(VA.needsCustom());
      // store SRet argument in %sp+64
      SDValue StackPtr = DAG.getRegister(VE::SX11, MVT::i32);
      SDValue PtrOff = DAG.getIntPtrConstant(64, dl);
      PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
      MemOpChains.push_back(
          DAG.getStore(Chain, dl, Arg, PtrOff, MachinePointerInfo()));
      hasStructRetAttr = true;
      continue;
    }

    if (VA.needsCustom()) {
      assert(VA.getLocVT() == MVT::f64 || VA.getLocVT() == MVT::v2i32);

      if (VA.isMemLoc()) {
        unsigned Offset = VA.getLocMemOffset() + StackOffset;
        // if it is double-word aligned, just store.
        if (Offset % 8 == 0) {
          SDValue StackPtr = DAG.getRegister(VE::SX11, MVT::i32);
          SDValue PtrOff = DAG.getIntPtrConstant(Offset, dl);
          PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
          MemOpChains.push_back(
              DAG.getStore(Chain, dl, Arg, PtrOff, MachinePointerInfo()));
          continue;
        }
      }

      if (VA.getLocVT() == MVT::f64) {
        // Move from the float value from float registers into the
        // integer registers.

        // TODO: The f64 -> v2i32 conversion is super-inefficient for
        // constants: it sticks them in the constant pool, then loads
        // to a fp register, then stores to temp memory, then loads to
        // integer registers.
        Arg = DAG.getNode(ISD::BITCAST, dl, MVT::v2i32, Arg);
      }

      SDValue Part0 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i32,
                                  Arg,
                                  DAG.getConstant(0, dl, getVectorIdxTy(DAG.getDataLayout())));
      SDValue Part1 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i32,
                                  Arg,
                                  DAG.getConstant(1, dl, getVectorIdxTy(DAG.getDataLayout())));

      if (VA.isRegLoc()) {
        RegsToPass.push_back(std::make_pair(VA.getLocReg(), Part0));
        assert(i+1 != e);
        CCValAssign &NextVA = ArgLocs[++i];
        if (NextVA.isRegLoc()) {
          RegsToPass.push_back(std::make_pair(NextVA.getLocReg(), Part1));
        } else {
          // Store the second part in stack.
          unsigned Offset = NextVA.getLocMemOffset() + StackOffset;
          SDValue StackPtr = DAG.getRegister(VE::SX11, MVT::i32);
          SDValue PtrOff = DAG.getIntPtrConstant(Offset, dl);
          PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
          MemOpChains.push_back(
              DAG.getStore(Chain, dl, Part1, PtrOff, MachinePointerInfo()));
        }
      } else {
        unsigned Offset = VA.getLocMemOffset() + StackOffset;
        // Store the first part.
        SDValue StackPtr = DAG.getRegister(VE::SX11, MVT::i32);
        SDValue PtrOff = DAG.getIntPtrConstant(Offset, dl);
        PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
        MemOpChains.push_back(
            DAG.getStore(Chain, dl, Part0, PtrOff, MachinePointerInfo()));
        // Store the second part.
        PtrOff = DAG.getIntPtrConstant(Offset + 4, dl);
        PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
        MemOpChains.push_back(
            DAG.getStore(Chain, dl, Part1, PtrOff, MachinePointerInfo()));
      }
      continue;
    }

    // Arguments that can be passed on register must be kept at
    // RegsToPass vector
    if (VA.isRegLoc()) {
      if (VA.getLocVT() != MVT::f32) {
        RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
        continue;
      }
      Arg = DAG.getNode(ISD::BITCAST, dl, MVT::i32, Arg);
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      continue;
    }

    assert(VA.isMemLoc());

    // Create a store off the stack pointer for this argument.
    SDValue StackPtr = DAG.getRegister(VE::SX11, MVT::i32);
    SDValue PtrOff = DAG.getIntPtrConstant(VA.getLocMemOffset() + StackOffset,
                                           dl);
    PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);
    MemOpChains.push_back(
        DAG.getStore(Chain, dl, Arg, PtrOff, MachinePointerInfo()));
  }


  // Emit all stores, make sure the occur before any copies into physregs.
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);

  // Build a sequence of copy-to-reg nodes chained together with token
  // chain and flag operands which copy the outgoing args into registers.
  // The InFlag in necessary since all emitted instructions must be
  // stuck together.
  SDValue InFlag;
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    unsigned Reg = RegsToPass[i].first;
    Chain = DAG.getCopyToReg(Chain, dl, Reg, RegsToPass[i].second, InFlag);
    InFlag = Chain.getValue(1);
  }

  unsigned SRetArgSize = (hasStructRetAttr)? getSRetArgSize(DAG, Callee):0;

  // If the callee is a GlobalAddress node (quite common, every direct call is)
  // turn it into a TargetGlobalAddress node so that legalize doesn't hack it.
  // Likewise ExternalSymbol -> TargetExternalSymbol.
  unsigned TF = isPositionIndependent() ? VEMCExpr::VK_VE_WPLT30 : 0;
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, MVT::i32, 0, TF);
  else if (ExternalSymbolSDNode *E = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(E->getSymbol(), MVT::i32, TF);

  // Returns a chain & a flag for retval copy to use
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  if (hasStructRetAttr)
    Ops.push_back(DAG.getTargetConstant(SRetArgSize, dl, MVT::i32));
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i)
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));

  // Add a register mask operand representing the call-preserved registers.
  const VERegisterInfo *TRI = Subtarget->getRegisterInfo();
  const uint32_t *Mask =
           TRI->getCallPreservedMask(DAG.getMachineFunction(), CallConv);
  assert(Mask && "Missing call preserved mask for calling convention");
  Ops.push_back(DAG.getRegisterMask(Mask));

  if (InFlag.getNode())
    Ops.push_back(InFlag);

  Chain = DAG.getNode(VEISD::CALL, dl, NodeTys, Ops);
  InFlag = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(ArgsSize, dl, true),
                             DAG.getIntPtrConstant(0, dl, true), InFlag, dl);
  InFlag = Chain.getValue(1);

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;
  CCState RVInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  RVInfo.AnalyzeCallResult(Ins, RetCC_VE);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    if (RVLocs[i].getLocVT() == MVT::v2i32) {
      SDValue Vec = DAG.getNode(ISD::UNDEF, dl, MVT::v2i32);
      SDValue Lo = DAG.getCopyFromReg(
          Chain, dl, RVLocs[i++].getLocReg(), MVT::i32, InFlag);
      Chain = Lo.getValue(1);
      InFlag = Lo.getValue(2);
      Vec = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v2i32, Vec, Lo,
                        DAG.getConstant(0, dl, MVT::i32));
      SDValue Hi = DAG.getCopyFromReg(
          Chain, dl, RVLocs[i].getLocReg(), MVT::i32, InFlag);
      Chain = Hi.getValue(1);
      InFlag = Hi.getValue(2);
      Vec = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v2i32, Vec, Hi,
                        DAG.getConstant(1, dl, MVT::i32));
      InVals.push_back(Vec);
    } else {
      Chain =
          DAG.getCopyFromReg(Chain, dl, RVLocs[i].getLocReg(),
                             RVLocs[i].getValVT(), InFlag)
              .getValue(1);
      InFlag = Chain.getValue(2);
      InVals.push_back(Chain.getValue(0));
    }
  }

  return Chain;
}
#endif

// FIXME? Maybe this could be a TableGen attribute on some registers and
// this table could be generated automatically from RegInfo.
unsigned VETargetLowering::getRegisterByName(const char* RegName, EVT VT,
                                               SelectionDAG &DAG) const {
  unsigned Reg = StringSwitch<unsigned>(RegName)
    .Case("sp", VE::SX11)        // Stack pointer
    .Case("fp", VE::SX9)         // Frame pointer
    .Case("sl", VE::SX8)         // Stack limit
    .Case("lr", VE::SX10)        // Link regsiter
    .Case("tp", VE::SX14)        // Thread pointer
    .Case("outer", VE::SX12)     // Outer regiser
    .Case("info", VE::SX17)      // Info area register
    .Case("got", VE::SX15)       // Global offset table register
    .Case("plt", VE::SX16)       // Procedure linkage table register
    .Case("usrcc", VE::UCC)     // User clock counter
    .Default(0);

  if (Reg)
    return Reg;

  report_fatal_error("Invalid register name global variable");
}

// This functions returns true if CalleeName is a ABI function that returns
// a long double (fp128).
static bool isFP128ABICall(const char *CalleeName)
{
  static const char *const ABICalls[] =
    {  "_Q_add", "_Q_sub", "_Q_mul", "_Q_div",
       "_Q_sqrt", "_Q_neg",
       "_Q_itoq", "_Q_stoq", "_Q_dtoq", "_Q_utoq",
       "_Q_lltoq", "_Q_ulltoq",
       nullptr
    };
  for (const char * const *I = ABICalls; *I != nullptr; ++I)
    if (strcmp(CalleeName, *I) == 0)
      return true;
  return false;
}

unsigned
VETargetLowering::getSRetArgSize(SelectionDAG &DAG, SDValue Callee) const
{
  const Function *CalleeFn = nullptr;
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    CalleeFn = dyn_cast<Function>(G->getGlobal());
  } else if (ExternalSymbolSDNode *E =
             dyn_cast<ExternalSymbolSDNode>(Callee)) {
    const Function &F = DAG.getMachineFunction().getFunction();
    const Module *M = F.getParent();
    const char *CalleeName = E->getSymbol();
    CalleeFn = M->getFunction(CalleeName);
    if (!CalleeFn && isFP128ABICall(CalleeName))
      return 16; // Return sizeof(fp128)
  }

  if (!CalleeFn)
    return 0;

  // It would be nice to check for the sret attribute on CalleeFn here,
  // but since it is not part of the function type, any check will misfire.

  PointerType *Ty = cast<PointerType>(CalleeFn->arg_begin()->getType());
  Type *ElementTy = Ty->getElementType();
  return DAG.getDataLayout().getTypeAllocSize(ElementTy);
}

// Lower a call for the 64-bit ABI.
SDValue
VETargetLowering::LowerCall_64(TargetLowering::CallLoweringInfo &CLI,
                               SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc DL = CLI.DL;
  SDValue Chain = CLI.Chain;
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  // VE target does not yet support tail call optimization.
  CLI.IsTailCall = false;

  // Get the base offset of the outgoing arguments stack space.
  unsigned ArgsBaseOffset = 176;
  // Get the size of the preserved arguments area
  unsigned ArgsPreserved = 8*8u;

  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CLI.CallConv, CLI.IsVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  // Allocate the preserved area first.
  CCInfo.AllocateStack(ArgsPreserved, 8);
  // We already allocated the preserved area, so the stack offset computed
  // by CC_VE would be correct now.
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_VE);

  // VE requires to use both register and stack for varargs or no-prototyped
  // functions.  FIXME: How to check prototype here?
  bool UseBoth = CLI.IsVarArg /* || CLI.NoProtoType */;

  // Analyze operands again if it is required to store BOTH.
  SmallVector<CCValAssign, 16> ArgLocs2;
  CCState CCInfo2(CLI.CallConv, CLI.IsVarArg, DAG.getMachineFunction(),
                  ArgLocs2, *DAG.getContext());
  if (UseBoth)
    CCInfo2.AnalyzeCallOperands(CLI.Outs, CC_VE2);

  // Get the size of the outgoing arguments stack space requirement.
  unsigned ArgsSize = CCInfo.getNextStackOffset();

  // Keep stack frames 16-byte aligned.
  ArgsSize = alignTo(ArgsSize, 16);

  // Adjust the stack pointer to make room for the arguments.
  // FIXME: Use hasReservedCallFrame to avoid %sp adjustments around all calls
  // with more than 6 arguments.
  Chain = DAG.getCALLSEQ_START(Chain, ArgsSize, 0, DL);

  // Collect the set of registers to pass to the function and their values.
  // This will be emitted as a sequence of CopyToReg nodes glued to the call
  // instruction.
  SmallVector<std::pair<unsigned, SDValue>, 8> RegsToPass;

  // Collect chains from all the memory opeations that copy arguments to the
  // stack. They must follow the stack pointer adjustment above and precede the
  // call instruction itself.
  SmallVector<SDValue, 8> MemOpChains;

  // VE needs to get address of callee function in a register
  // So, prepare to copy it to SX12 here.

  // If the callee is a GlobalAddress node (quite common, every direct call is)
  // turn it into a TargetGlobalAddress node so that legalize doesn't hack it.
  // Likewise ExternalSymbol -> TargetExternalSymbol.
  SDValue Callee = CLI.Callee;

  bool IsPICCall = isPositionIndependent();

  // Turn GlobalAddress/ExternalSymbol node into a value node
  // contining the address of them here.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    if (IsPICCall) {
      Callee = DAG.getTargetGlobalAddress(G->getGlobal(), DL, PtrVT, 0, 0);
      Callee = DAG.getNode(VEISD::GETFUNPLT, DL, PtrVT, Callee);
    } else {
      Callee =  makeHiLoPair(Callee, VEMCExpr::VK_VE_HI,
                             VEMCExpr::VK_VE_LO, DAG);
    }
  } else if (ExternalSymbolSDNode *E = dyn_cast<ExternalSymbolSDNode>(Callee)) {
    if (IsPICCall) {
      Callee = DAG.getTargetExternalSymbol(E->getSymbol(), PtrVT, 0);
      Callee = DAG.getNode(VEISD::GETFUNPLT, DL, PtrVT, Callee);
    } else {
      Callee =  makeHiLoPair(Callee, VEMCExpr::VK_VE_HI,
                             VEMCExpr::VK_VE_LO, DAG);
    }
  }

  RegsToPass.push_back(std::make_pair(VE::SX12, Callee));

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = CLI.OutVals[i];

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
    default:
      llvm_unreachable("Unknown location info!");
    case CCValAssign::Full:
      break;
    case CCValAssign::SExt:
      Arg = DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), Arg);
      break;
    case CCValAssign::ZExt:
      Arg = DAG.getNode(ISD::ZERO_EXTEND, DL, VA.getLocVT(), Arg);
      break;
    case CCValAssign::AExt:
      Arg = DAG.getNode(ISD::ANY_EXTEND, DL, VA.getLocVT(), Arg);
      break;
    }

    if (VA.isRegLoc()) {
      // The custom bit on an i32 return value indicates that it should be
      // passed in the high bits of the register.
      if (VA.getValVT() == MVT::i32 && VA.needsCustom()) {
#if 1 // ishizaka
      llvm_unreachable("what's this?\n");
#else
        Arg = DAG.getNode(ISD::SHL, DL, MVT::i64, Arg,
                          DAG.getConstant(32, DL, MVT::i32));

        // The next value may go in the low bits of the same register.
        // Handle both at once.
        if (i+1 < ArgLocs.size() && ArgLocs[i+1].isRegLoc() &&
            ArgLocs[i+1].getLocReg() == VA.getLocReg()) {
          SDValue NV = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i64,
                                   CLI.OutVals[i+1]);
          Arg = DAG.getNode(ISD::OR, DL, MVT::i64, Arg, NV);
          // Skip the next value, it's already done.
          ++i;
        }
#endif
      }
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      if (!UseBoth)
        continue;
      VA = ArgLocs2[i];
    }

    assert(VA.isMemLoc());

    // Create a store off the stack pointer for this argument.
    SDValue StackPtr = DAG.getRegister(VE::SX11, PtrVT);
    // The argument area starts at %fp+176 in the callee frame,
    // %sp+176 in ours.
    SDValue PtrOff = DAG.getIntPtrConstant(VA.getLocMemOffset() +
                                           ArgsBaseOffset, DL);
    PtrOff = DAG.getNode(ISD::ADD, DL, PtrVT, StackPtr, PtrOff);
    MemOpChains.push_back(
        DAG.getStore(Chain, DL, Arg, PtrOff, MachinePointerInfo()));
  }

  // Emit all stores, make sure they occur before the call.
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  // Build a sequence of CopyToReg nodes glued together with token chain and
  // glue operands which copy the outgoing args into registers. The InGlue is
  // necessary since all emitted instructions must be stuck together in order
  // to pass the live physical registers.
  SDValue InGlue;
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Chain = DAG.getCopyToReg(Chain, DL,
                             RegsToPass[i].first, RegsToPass[i].second, InGlue);
    InGlue = Chain.getValue(1);
  }

  // Build the operands for the call instruction itself.
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i)
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));

  // Add a register mask operand representing the call-preserved registers.
  const VERegisterInfo *TRI = Subtarget->getRegisterInfo();
  const uint32_t *Mask =
    TRI->getCallPreservedMask(DAG.getMachineFunction(), CLI.CallConv);
  assert(Mask && "Missing call preserved mask for calling convention");
  Ops.push_back(DAG.getRegisterMask(Mask));

  // Make sure the CopyToReg nodes are glued to the call instruction which
  // consumes the registers.
  if (InGlue.getNode())
    Ops.push_back(InGlue);

  // Now the call itself.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  Chain = DAG.getNode(VEISD::CALL, DL, NodeTys, Ops);
  InGlue = Chain.getValue(1);

  // Revert the stack pointer immediately after the call.
  Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(ArgsSize, DL, true),
                             DAG.getIntPtrConstant(0, DL, true), InGlue, DL);
  InGlue = Chain.getValue(1);

  // Now extract the return values. This is more or less the same as
  // LowerFormalArguments_64.

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;
  CCState RVInfo(CLI.CallConv, CLI.IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Set inreg flag manually for codegen generated library calls that
  // return float.
  if (CLI.Ins.size() == 1 && CLI.Ins[0].VT == MVT::f32 && !CLI.CS)
    CLI.Ins[0].Flags.setInReg();

  RVInfo.AnalyzeCallResult(CLI.Ins, RetCC_VE);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    unsigned Reg = VA.getLocReg();

    // When returning 'inreg {i32, i32 }', two consecutive i32 arguments can
    // reside in the same register in the high and low bits. Reuse the
    // CopyFromReg previous node to avoid duplicate copies.
    SDValue RV;
    if (RegisterSDNode *SrcReg = dyn_cast<RegisterSDNode>(Chain.getOperand(1)))
      if (SrcReg->getReg() == Reg && Chain->getOpcode() == ISD::CopyFromReg)
        RV = Chain.getValue(0);

    // But usually we'll create a new CopyFromReg for a different register.
    if (!RV.getNode()) {
      RV = DAG.getCopyFromReg(Chain, DL, Reg, RVLocs[i].getLocVT(), InGlue);
      Chain = RV.getValue(1);
      InGlue = Chain.getValue(2);
    }

    // Get the high bits for i32 struct elements.
    if (VA.getValVT() == MVT::i32 && VA.needsCustom())
      RV = DAG.getNode(ISD::SRL, DL, VA.getLocVT(), RV,
                       DAG.getConstant(32, DL, MVT::i32));

    // The callee promoted the return value, so insert an Assert?ext SDNode so
    // we won't promote the value again in this function.
    switch (VA.getLocInfo()) {
    case CCValAssign::SExt:
      RV = DAG.getNode(ISD::AssertSext, DL, VA.getLocVT(), RV,
                       DAG.getValueType(VA.getValVT()));
      break;
    case CCValAssign::ZExt:
      RV = DAG.getNode(ISD::AssertZext, DL, VA.getLocVT(), RV,
                       DAG.getValueType(VA.getValVT()));
      break;
    default:
      break;
    }

    // Truncate the register down to the return value type.
    if (VA.isExtInLoc())
      RV = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), RV);

    InVals.push_back(RV);
  }

  return Chain;
}

//===----------------------------------------------------------------------===//
// TargetLowering Implementation
//===----------------------------------------------------------------------===//

TargetLowering::AtomicExpansionKind VETargetLowering::shouldExpandAtomicRMWInIR(AtomicRMWInst *AI) const {
  if (AI->getOperation() == AtomicRMWInst::Xchg &&
      AI->getType()->getPrimitiveSizeInBits() == 32)
    return AtomicExpansionKind::None; // Uses xchg instruction

  return AtomicExpansionKind::CmpXChg;
}

VETargetLowering::VETargetLowering(const TargetMachine &TM,
                                   const VESubtarget &STI)
    : TargetLowering(TM), Subtarget(&STI) {
  MVT PtrVT = MVT::getIntegerVT(8 * TM.getPointerSize(0));

  // Instructions which use registers as conditionals examine all the
  // bits (as does the pseudo SELECT_CC expansion). I don't think it
  // matters much whether it's ZeroOrOneBooleanContent, or
  // ZeroOrNegativeOneBooleanContent, so, arbitrarily choose the
  // former.
  setBooleanContents(ZeroOrOneBooleanContent);
  setBooleanVectorContents(ZeroOrOneBooleanContent);

  // Set up the register classes.
  addRegisterClass(MVT::i32, &VE::I32RegClass);
  addRegisterClass(MVT::i64, &VE::I64RegClass);
  addRegisterClass(MVT::f32, &VE::F32RegClass);
  addRegisterClass(MVT::f64, &VE::I64RegClass);
  addRegisterClass(MVT::f128, &VE::F128RegClass);
  addRegisterClass(MVT::v256i32, &VE::V64RegClass);
  addRegisterClass(MVT::v256i64, &VE::V64RegClass);
  addRegisterClass(MVT::v512i32, &VE::V64RegClass);
  addRegisterClass(MVT::v256f32, &VE::V64RegClass);
  addRegisterClass(MVT::v256f64, &VE::V64RegClass);
  addRegisterClass(MVT::v512f32, &VE::V64RegClass);
  addRegisterClass(MVT::v4i64, &VE::VMRegClass);
  addRegisterClass(MVT::v256i1, &VE::VMRegClass);
  addRegisterClass(MVT::v8i64, &VE::VM512RegClass);
  addRegisterClass(MVT::v512i1, &VE::VM512RegClass);

  // Turn FP extload into load/fpextend
  for (MVT VT : MVT::fp_valuetypes()) {
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::f32, Expand);
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::f64, Expand);
  }

  // VE doesn't have i1 sign extending load
  for (MVT VT : MVT::integer_valuetypes()) {
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i1, Promote);
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i1, Promote);
    setLoadExtAction(ISD::EXTLOAD,  VT, MVT::i1, Promote);
    setTruncStoreAction(MVT::i64, MVT::i1, Expand);
  }

  // Turn FP truncstore into trunc + store.
  setTruncStoreAction(MVT::f64, MVT::f32, Expand);
  setTruncStoreAction(MVT::f128, MVT::f32, Expand);
  setTruncStoreAction(MVT::f128, MVT::f64, Expand);

  // custom splat handling
  for (MVT VT : MVT::vector_valuetypes()) {
    setOperationAction(ISD::BUILD_VECTOR, VT, Custom);
  }

  // Custom legalize GlobalAddress nodes into LO/HI parts.
  setOperationAction(ISD::GlobalAddress, PtrVT, Custom);
  setOperationAction(ISD::GlobalTLSAddress, PtrVT, Custom);
  setOperationAction(ISD::ConstantPool, PtrVT, Custom);
  setOperationAction(ISD::BlockAddress, PtrVT, Custom);

  // VE has no REM or DIVREM operations.
  for (MVT VT : MVT::integer_valuetypes()) {
    setOperationAction(ISD::UREM, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::SDIVREM, VT, Expand);
    setOperationAction(ISD::UDIVREM, VT, Expand);
  }

  // VE has instructions for fp<->sint, so use them.

  // VE doesn't have instructions for fp<->uint, so expand them by llvm
  setOperationAction(ISD::FP_TO_UINT, MVT::i32, Promote); // use i64
  setOperationAction(ISD::UINT_TO_FP, MVT::i32, Promote); // use i64
  setOperationAction(ISD::FP_TO_UINT, MVT::i64, Expand);
  setOperationAction(ISD::UINT_TO_FP, MVT::i64, Expand);

  // VE doesn't have BRCOND
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);

  // BRIND/BR_JT are not implemented yet.
  //   FIXME: BRIND instruction is implemented, but JumpTable is not yet.
  setOperationAction(ISD::BRIND,  MVT::Other, Expand);
  setOperationAction(ISD::BR_JT,  MVT::Other, Expand);

#if 0
  // FIXME: VE's SETJMP is not investigated yet.
  setOperationAction(ISD::EH_SJLJ_SETJMP, MVT::i32, Custom);
  setOperationAction(ISD::EH_SJLJ_LONGJMP, MVT::Other, Custom);
#endif

  setTargetDAGCombine(ISD::FADD);
  //setTargetDAGCombine(ISD::FMA);

  // ATOMICs.
  // Atomics are supported on VE. 32-bit atomics are also
  // supported by some Leon VE variants. Otherwise, atomics
  // are unsupported.
  setMaxAtomicSizeInBitsSupported(64);

  setMinCmpXchgSizeInBits(32);

  // FIXME: VE's atomic instructions are not investivated yet.
  setOperationAction(ISD::ATOMIC_FENCE, MVT::Other, Legal);

  setOperationAction(ISD::ATOMIC_CMP_SWAP, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_ADD, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_SWAP, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_SUB, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_AND, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_CLR, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_OR, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_XOR, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_NAND, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_MIN, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_MAX, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_UMIN, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_UMAX, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_STORE, MVT::i32, Expand);

  if (1) {
    setOperationAction(ISD::ATOMIC_CMP_SWAP, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_ADD, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_SWAP, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_SUB, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_AND, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_CLR, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_OR, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_XOR, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_NAND, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_MIN, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_MAX, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_UMIN, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD_UMAX, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_LOAD, MVT::i64, Expand);
    setOperationAction(ISD::ATOMIC_STORE, MVT::i64, Expand);
  }

  // FIXME: VE's I128 stuff is not investivated yet
  if (!1) {
    // These libcalls are not available in 32-bit.
    setLibcallName(RTLIB::SHL_I128, nullptr);
    setLibcallName(RTLIB::SRL_I128, nullptr);
    setLibcallName(RTLIB::SRA_I128, nullptr);
  }

  for (MVT VT : MVT::fp_valuetypes()) {
    // VE has no sclar FMA instruction
    setOperationAction(ISD::FMA, VT, Expand);
    setOperationAction(ISD::FMAD, VT, Expand);
    setOperationAction(ISD::FREM, VT, Expand);
    setOperationAction(ISD::FNEG, VT, Expand);
    setOperationAction(ISD::FABS, VT, Expand);
    setOperationAction(ISD::FSQRT, VT, Expand);
    setOperationAction(ISD::FSIN, VT, Expand);
    setOperationAction(ISD::FCOS, VT, Expand);
    setOperationAction(ISD::FPOWI, VT, Expand);
    setOperationAction(ISD::FPOW, VT, Expand);
    setOperationAction(ISD::FLOG, VT, Expand);
    setOperationAction(ISD::FLOG2, VT, Expand);
    setOperationAction(ISD::FLOG10, VT, Expand);
    setOperationAction(ISD::FEXP, VT, Expand);
    setOperationAction(ISD::FEXP2, VT, Expand);
    setOperationAction(ISD::FCEIL, VT, Expand);
    setOperationAction(ISD::FTRUNC, VT, Expand);
    setOperationAction(ISD::FRINT, VT, Expand);
    setOperationAction(ISD::FNEARBYINT, VT, Expand);
    setOperationAction(ISD::FROUND, VT, Expand);
    setOperationAction(ISD::FFLOOR, VT, Expand);
    setOperationAction(ISD::FMINNUM, VT, Expand);
    setOperationAction(ISD::FMAXNUM, VT, Expand);
    setOperationAction(ISD::FMINNAN, VT, Expand);
    setOperationAction(ISD::FMAXNAN, VT, Expand);
    setOperationAction(ISD::FSINCOS, VT, Expand);
  }

  // FIXME: VE's FCOPYSIGN is not investivated yet
  setOperationAction(ISD::FCOPYSIGN, MVT::f128, Expand);
  setOperationAction(ISD::FCOPYSIGN, MVT::f64, Expand);
  setOperationAction(ISD::FCOPYSIGN, MVT::f32, Expand);

  // FIXME: VE's SHL_PARTS and others are not investigated yet.
  setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);
  if (1) {
    setOperationAction(ISD::SHL_PARTS, MVT::i64, Expand);
    setOperationAction(ISD::SRA_PARTS, MVT::i64, Expand);
    setOperationAction(ISD::SRL_PARTS, MVT::i64, Expand);
  }

  // Expands to [SU]MUL_LOHI.
  setOperationAction(ISD::MULHU,     MVT::i32, Expand);
  setOperationAction(ISD::MULHS,     MVT::i32, Expand);
  //setOperationAction(ISD::MUL,       MVT::i32, Expand);

  // FIXME: VE's i64 MUL stuff is not investigated yet.
#if 0
  if (Subtarget->useSoftMulDiv()) {
    // .umul works for both signed and unsigned
    setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
    setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
    setLibcallName(RTLIB::MUL_I32, ".umul");

    setOperationAction(ISD::SDIV, MVT::i32, Expand);
    setLibcallName(RTLIB::SDIV_I32, ".div");

    setOperationAction(ISD::UDIV, MVT::i32, Expand);
    setLibcallName(RTLIB::UDIV_I32, ".udiv");
  }
#endif

  if (1) {
    setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::MULHU,     MVT::i64, Expand);
    setOperationAction(ISD::MULHS,     MVT::i64, Expand);

    setOperationAction(ISD::UMULO,     MVT::i64, Expand);
    setOperationAction(ISD::SMULO,     MVT::i64, Expand);
  }

  // Bits operations
  setOperationAction(ISD::BITREVERSE, MVT::i32, Legal);
  setOperationAction(ISD::BITREVERSE, MVT::i64, Legal);
  setOperationAction(ISD::BSWAP, MVT::i32, Legal);
  setOperationAction(ISD::BSWAP, MVT::i64, Legal);
  setOperationAction(ISD::CTPOP, MVT::i32, Legal);
  setOperationAction(ISD::CTPOP, MVT::i64, Legal);
  // FIXME: VE has CTLZ, but not sure how to use it correctly atm.
  setOperationAction(ISD::CTLZ , MVT::i32, Legal);
  setOperationAction(ISD::CTLZ , MVT::i64, Legal);
  setOperationAction(ISD::CTTZ , MVT::i32, Expand);
  setOperationAction(ISD::CTTZ , MVT::i64, Expand);
  setOperationAction(ISD::ROTL , MVT::i32, Expand);
  setOperationAction(ISD::ROTL , MVT::i64, Expand);
  setOperationAction(ISD::ROTR , MVT::i32, Expand);
  setOperationAction(ISD::ROTR , MVT::i64, Expand);

  // VASTART needs to be custom lowered to use the VarArgsFrameIndex.
  setOperationAction(ISD::VASTART           , MVT::Other, Custom);
  // VAARG needs to be lowered to access with 8 bytes alignment.
  setOperationAction(ISD::VAARG             , MVT::Other, Custom);

  // Use the default implementation.
  setOperationAction(ISD::VACOPY            , MVT::Other, Expand);
  setOperationAction(ISD::VAEND             , MVT::Other, Expand);
  setOperationAction(ISD::STACKSAVE         , MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE      , MVT::Other, Expand);

  // Expand DYNAMIC_STACKALLOC
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Expand);

#if 0
  // VE natively supports vector loads
  for (MVT VT : MVT::vector_valuetypes()) {
    setOperationAction(ISD::LOAD,  VT, Expand);
    setOperationAction(ISD::STORE, VT, Expand);
  }
#endif

  // VE has no load/store for f128, but llvm doesn't expand them
  // automatically, so we need to use Custom here.
  setOperationAction(ISD::LOAD, MVT::f128, Custom);
  setOperationAction(ISD::STORE, MVT::f128, Custom);

  // VE has FAQ, FSQ, FMQ, and FCQ
  setOperationAction(ISD::FADD,  MVT::f128, Legal);
  setOperationAction(ISD::FSUB,  MVT::f128, Legal);
  setOperationAction(ISD::FMUL,  MVT::f128, Legal);
  setOperationAction(ISD::FDIV,  MVT::f128, Expand);
  setOperationAction(ISD::FSQRT, MVT::f128, Expand);
  setOperationAction(ISD::FP_EXTEND, MVT::f128, Legal);
  setOperationAction(ISD::FP_ROUND,  MVT::f128, Legal);

  // Other configurations related to f128.
  setOperationAction(ISD::SELECT,    MVT::f128, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::f128, Expand);
  setOperationAction(ISD::SETCC,     MVT::f128, Legal);
  setOperationAction(ISD::BR_CC,     MVT::f128, Legal);

  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);

  // TRAP to expand (which turns it into abort).
  setOperationAction(ISD::TRAP, MVT::Other, Expand);

  // On most systems, DEBUGTRAP and TRAP have no difference. The "Expand"
  // here is to inform DAG Legalizer to replace DEBUGTRAP with TRAP.
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Expand);

// vector fma // TESTING
  for (MVT VT : MVT::vector_valuetypes()) {
    setOperationAction(ISD::FMA, VT, Legal);
    setOperationAction(ISD::FNEG, VT, Legal);
    //setOperationAction(ISD::FMAD, VT, Legal);
  }

  setStackPointerRegisterToSaveRestore(VE::SX11);

  // Set function alignment to 16 bytes (4 bits)
  setMinFunctionAlignment(4);

  // VE stores all argument by 8 bytes alignment
  setMinStackArgumentAlignment(8);

  computeRegisterProperties(Subtarget->getRegisterInfo());
}

const char *VETargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch ((VEISD::NodeType)Opcode) {
  case VEISD::FIRST_NUMBER:    break;
  case VEISD::CMPICC:          return "VEISD::CMPICC";
  case VEISD::CMPFCC:          return "VEISD::CMPFCC";
  case VEISD::BRICC:           return "VEISD::BRICC";
  case VEISD::BRXCC:           return "VEISD::BRXCC";
  case VEISD::BRFCC:           return "VEISD::BRFCC";
  case VEISD::SELECT:          return "VEISD::SELECT";
  case VEISD::SELECT_ICC:      return "VEISD::SELECT_ICC";
  case VEISD::SELECT_XCC:      return "VEISD::SELECT_XCC";
  case VEISD::SELECT_FCC:      return "VEISD::SELECT_FCC";
  case VEISD::EH_SJLJ_SETJMP:  return "VEISD::EH_SJLJ_SETJMP";
  case VEISD::EH_SJLJ_LONGJMP: return "VEISD::EH_SJLJ_LONGJMP";
  case VEISD::Hi:              return "VEISD::Hi";
  case VEISD::Lo:              return "VEISD::Lo";
  case VEISD::FTOI:            return "VEISD::FTOI";
  case VEISD::ITOF:            return "VEISD::ITOF";
  case VEISD::FTOX:            return "VEISD::FTOX";
  case VEISD::XTOF:            return "VEISD::XTOF";
  case VEISD::MAX:             return "VEISD::MAX";
  case VEISD::MIN:             return "VEISD::MIN";
  case VEISD::FMAX:            return "VEISD::FMAX";
  case VEISD::FMIN:            return "VEISD::FMIN";
  case VEISD::GETFUNPLT:       return "VEISD::GETFUNPLT";
  case VEISD::CALL:            return "VEISD::CALL";
  case VEISD::RET_FLAG:        return "VEISD::RET_FLAG";
  case VEISD::GLOBAL_BASE_REG: return "VEISD::GLOBAL_BASE_REG";
  case VEISD::FLUSHW:          return "VEISD::FLUSHW";
  case VEISD::TLS_ADD:         return "VEISD::TLS_ADD";
  case VEISD::TLS_LD:          return "VEISD::TLS_LD";
  case VEISD::TLS_CALL:        return "VEISD::TLS_CALL";
  case VEISD::VEC_BROADCAST:   return "VEISD::VEC_BROADCAST";
  case VEISD::VEC_SEQ:         return "VEISD::VEC_SEQ";
  }
  return nullptr;
}

EVT VETargetLowering::getSetCCResultType(const DataLayout &, LLVMContext &,
                                            EVT VT) const {
  if (!VT.isVector())
    return MVT::i32;
  return VT.changeVectorElementTypeToInteger();
}

/// isMaskedValueZeroForTargetNode - Return true if 'Op & Mask' is known to
/// be zero. Op is expected to be a target specific node. Used by DAG
/// combiner.
void VETargetLowering::computeKnownBitsForTargetNode
                                (const SDValue Op,
                                 KnownBits &Known,
                                 const APInt &DemandedElts,
                                 const SelectionDAG &DAG,
                                 unsigned Depth) const {
  KnownBits Known2;
  Known.resetAll();

  switch (Op.getOpcode()) {
  default: break;
  case VEISD::SELECT_ICC:
  case VEISD::SELECT_XCC:
  case VEISD::SELECT_FCC:
    DAG.computeKnownBits(Op.getOperand(1), Known, Depth+1);
    DAG.computeKnownBits(Op.getOperand(0), Known2, Depth+1);

    // Only known if known in both the LHS and RHS.
    Known.One &= Known2.One;
    Known.Zero &= Known2.Zero;
    break;
  }
}

#if 0
// Look at LHS/RHS/CC and see if they are a lowered setcc instruction.  If so
// set LHS/RHS and VECC to the LHS/RHS of the setcc and VECC to the condition.
static void LookThroughSetCC(SDValue &LHS, SDValue &RHS,
                             ISD::CondCode CC, unsigned &VECC) {
  if (isNullConstant(RHS) &&
      CC == ISD::SETNE &&
      (((LHS.getOpcode() == VEISD::SELECT_ICC ||
         LHS.getOpcode() == VEISD::SELECT_XCC) &&
        LHS.getOperand(3).getOpcode() == VEISD::CMPICC) ||
       (LHS.getOpcode() == VEISD::SELECT_FCC &&
        LHS.getOperand(3).getOpcode() == VEISD::CMPFCC)) &&
      isOneConstant(LHS.getOperand(0)) &&
      isNullConstant(LHS.getOperand(1))) {
    SDValue CMPCC = LHS.getOperand(3);
    VECC = cast<ConstantSDNode>(LHS.getOperand(2))->getZExtValue();
    LHS = CMPCC.getOperand(0);
    RHS = CMPCC.getOperand(1);
  }
}
#endif

// Convert to a target node and set target flags.
SDValue VETargetLowering::withTargetFlags(SDValue Op, unsigned TF,
                                             SelectionDAG &DAG) const {
  if (const GlobalAddressSDNode *GA = dyn_cast<GlobalAddressSDNode>(Op))
    return DAG.getTargetGlobalAddress(GA->getGlobal(),
                                      SDLoc(GA),
                                      GA->getValueType(0),
                                      GA->getOffset(), TF);

  if (const ConstantPoolSDNode *CP = dyn_cast<ConstantPoolSDNode>(Op))
    return DAG.getTargetConstantPool(CP->getConstVal(),
                                     CP->getValueType(0),
                                     CP->getAlignment(),
                                     CP->getOffset(), TF);

  if (const BlockAddressSDNode *BA = dyn_cast<BlockAddressSDNode>(Op))
    return DAG.getTargetBlockAddress(BA->getBlockAddress(),
                                     Op.getValueType(),
                                     0,
                                     TF);

  if (const ExternalSymbolSDNode *ES = dyn_cast<ExternalSymbolSDNode>(Op))
    return DAG.getTargetExternalSymbol(ES->getSymbol(),
                                       ES->getValueType(0), TF);

  llvm_unreachable("Unhandled address SDNode");
}

// Split Op into high and low parts according to HiTF and LoTF.
// Return an ADD node combining the parts.
SDValue VETargetLowering::makeHiLoPair(SDValue Op,
                                          unsigned HiTF, unsigned LoTF,
                                          SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  SDValue Hi = DAG.getNode(VEISD::Hi, DL, VT, withTargetFlags(Op, HiTF, DAG));
  SDValue Lo = DAG.getNode(VEISD::Lo, DL, VT, withTargetFlags(Op, LoTF, DAG));
  return DAG.getNode(ISD::ADD, DL, VT, Hi, Lo);
}

// Build SDNodes for producing an address from a GlobalAddress, ConstantPool,
// or ExternalSymbol SDNode.
SDValue VETargetLowering::makeAddress(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = getPointerTy(DAG.getDataLayout());

  // Handle PIC mode first. SPARC needs a got load for every variable!
  if (isPositionIndependent()) {
    // GLOBAL_BASE_REG codegen'ed with call. Inform MFI that this
    // function has calls.
    MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
    MFI.setHasCalls(true);

    if (dyn_cast<GlobalAddressSDNode>(Op) != nullptr &&
        dyn_cast<GlobalAddressSDNode>(Op)->getGlobal()->hasLocalLinkage()) {
      SDValue HiLo = makeHiLoPair(Op, VEMCExpr::VK_VE_GOTOFFHI,
                                  VEMCExpr::VK_VE_GOTOFFLO, DAG);
      SDValue GlobalBase = DAG.getNode(VEISD::GLOBAL_BASE_REG, DL, VT);
      return DAG.getNode(ISD::ADD, DL, VT, GlobalBase, HiLo);
    } else {
      SDValue HiLo = makeHiLoPair(Op, VEMCExpr::VK_VE_GOTHI,
                                  VEMCExpr::VK_VE_GOTLO, DAG);
      SDValue GlobalBase = DAG.getNode(VEISD::GLOBAL_BASE_REG, DL, VT);
      SDValue AbsAddr = DAG.getNode(ISD::ADD, DL, VT, GlobalBase, HiLo);
      return DAG.getLoad(VT, DL, DAG.getEntryNode(), AbsAddr,
                         MachinePointerInfo::getGOT(DAG.getMachineFunction()));
    }
  }

  // This is one of the absolute code models.
  switch(getTargetMachine().getCodeModel()) {
  default:
    llvm_unreachable("Unsupported absolute code model");
  case CodeModel::Small:
    // abs32.
    return makeHiLoPair(Op, VEMCExpr::VK_VE_HI,
                        VEMCExpr::VK_VE_LO, DAG);
  case CodeModel::Medium: {
    // abs44.
    SDValue H44 = makeHiLoPair(Op, VEMCExpr::VK_VE_H44,
                               VEMCExpr::VK_VE_M44, DAG);
    H44 = DAG.getNode(ISD::SHL, DL, VT, H44, DAG.getConstant(12, DL, MVT::i32));
    SDValue L44 = withTargetFlags(Op, VEMCExpr::VK_VE_L44, DAG);
    L44 = DAG.getNode(VEISD::Lo, DL, VT, L44);
    return DAG.getNode(ISD::ADD, DL, VT, H44, L44);
  }
  case CodeModel::Large: {
    // abs64.
    SDValue Hi = makeHiLoPair(Op, VEMCExpr::VK_VE_HH,
                              VEMCExpr::VK_VE_HM, DAG);
    Hi = DAG.getNode(ISD::SHL, DL, VT, Hi, DAG.getConstant(32, DL, MVT::i32));
    SDValue Lo = makeHiLoPair(Op, VEMCExpr::VK_VE_HI,
                              VEMCExpr::VK_VE_LO, DAG);
    return DAG.getNode(ISD::ADD, DL, VT, Hi, Lo);
  }
  }
}

SDValue VETargetLowering::LowerGlobalAddress(SDValue Op,
                                                SelectionDAG &DAG) const {
  return makeAddress(Op, DAG);
}

SDValue VETargetLowering::LowerConstantPool(SDValue Op,
                                               SelectionDAG &DAG) const {
  return makeAddress(Op, DAG);
}

SDValue VETargetLowering::LowerBlockAddress(SDValue Op,
                                               SelectionDAG &DAG) const {
  return makeAddress(Op, DAG);
}

#if 0
SDValue VETargetLowering::LowerGlobalTLSAddress(SDValue Op,
                                                   SelectionDAG &DAG) const {
  GlobalAddressSDNode *GA = cast<GlobalAddressSDNode>(Op);
  if (DAG.getTarget().Options.EmulatedTLS)
    return LowerToTLSEmulatedModel(GA, DAG);

  SDLoc DL(GA);
  const GlobalValue *GV = GA->getGlobal();
  EVT PtrVT = getPointerTy(DAG.getDataLayout());

  TLSModel::Model model = getTargetMachine().getTLSModel(GV);

  if (model == TLSModel::GeneralDynamic || model == TLSModel::LocalDynamic) {
    unsigned HiTF = ((model == TLSModel::GeneralDynamic)
                     ? VEMCExpr::VK_VE_TLS_GD_HI22
                     : VEMCExpr::VK_VE_TLS_LDM_HI22);
    unsigned LoTF = ((model == TLSModel::GeneralDynamic)
                     ? VEMCExpr::VK_VE_TLS_GD_LO10
                     : VEMCExpr::VK_VE_TLS_LDM_LO10);
    unsigned addTF = ((model == TLSModel::GeneralDynamic)
                      ? VEMCExpr::VK_VE_TLS_GD_ADD
                      : VEMCExpr::VK_VE_TLS_LDM_ADD);
    unsigned callTF = ((model == TLSModel::GeneralDynamic)
                       ? VEMCExpr::VK_VE_TLS_GD_CALL
                       : VEMCExpr::VK_VE_TLS_LDM_CALL);

    SDValue HiLo = makeHiLoPair(Op, HiTF, LoTF, DAG);
    SDValue Base = DAG.getNode(VEISD::GLOBAL_BASE_REG, DL, PtrVT);
    SDValue Argument = DAG.getNode(VEISD::TLS_ADD, DL, PtrVT, Base, HiLo,
                               withTargetFlags(Op, addTF, DAG));

    SDValue Chain = DAG.getEntryNode();
    SDValue InFlag;

    Chain = DAG.getCALLSEQ_START(Chain, 1, 0, DL);
    Chain = DAG.getCopyToReg(Chain, DL, SP::O0, Argument, InFlag);
    InFlag = Chain.getValue(1);
    SDValue Callee = DAG.getTargetExternalSymbol("__tls_get_addr", PtrVT);
    SDValue Symbol = withTargetFlags(Op, callTF, DAG);

    SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
    const uint32_t *Mask = Subtarget->getRegisterInfo()->getCallPreservedMask(
        DAG.getMachineFunction(), CallingConv::C);
    assert(Mask && "Missing call preserved mask for calling convention");
    SDValue Ops[] = {Chain,
                     Callee,
                     Symbol,
                     DAG.getRegister(SP::O0, PtrVT),
                     DAG.getRegisterMask(Mask),
                     InFlag};
    Chain = DAG.getNode(VEISD::TLS_CALL, DL, NodeTys, Ops);
    InFlag = Chain.getValue(1);
    Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(1, DL, true),
                               DAG.getIntPtrConstant(0, DL, true), InFlag, DL);
    InFlag = Chain.getValue(1);
    SDValue Ret = DAG.getCopyFromReg(Chain, DL, SP::O0, PtrVT, InFlag);

    if (model != TLSModel::LocalDynamic)
      return Ret;

    SDValue Hi = DAG.getNode(VEISD::Hi, DL, PtrVT,
                 withTargetFlags(Op, VEMCExpr::VK_VE_TLS_LDO_HIX22, DAG));
    SDValue Lo = DAG.getNode(VEISD::Lo, DL, PtrVT,
                 withTargetFlags(Op, VEMCExpr::VK_VE_TLS_LDO_LOX10, DAG));
    HiLo =  DAG.getNode(ISD::XOR, DL, PtrVT, Hi, Lo);
    return DAG.getNode(VEISD::TLS_ADD, DL, PtrVT, Ret, HiLo,
                   withTargetFlags(Op, VEMCExpr::VK_VE_TLS_LDO_ADD, DAG));
  }

  if (model == TLSModel::InitialExec) {
    unsigned ldTF     = ((PtrVT == MVT::i64)? VEMCExpr::VK_VE_TLS_IE_LDX
                         : VEMCExpr::VK_VE_TLS_IE_LD);

    SDValue Base = DAG.getNode(VEISD::GLOBAL_BASE_REG, DL, PtrVT);

    // GLOBAL_BASE_REG codegen'ed with call. Inform MFI that this
    // function has calls.
    MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
    MFI.setHasCalls(true);

    SDValue TGA = makeHiLoPair(Op,
                               VEMCExpr::VK_VE_TLS_IE_HI22,
                               VEMCExpr::VK_VE_TLS_IE_LO10, DAG);
    SDValue Ptr = DAG.getNode(ISD::ADD, DL, PtrVT, Base, TGA);
    SDValue Offset = DAG.getNode(VEISD::TLS_LD,
                                 DL, PtrVT, Ptr,
                                 withTargetFlags(Op, ldTF, DAG));
    return DAG.getNode(VEISD::TLS_ADD, DL, PtrVT,
                       DAG.getRegister(SP::G7, PtrVT), Offset,
                       withTargetFlags(Op,
                                       VEMCExpr::VK_VE_TLS_IE_ADD, DAG));
  }

  assert(model == TLSModel::LocalExec);
  SDValue Hi = DAG.getNode(VEISD::Hi, DL, PtrVT,
                  withTargetFlags(Op, VEMCExpr::VK_VE_TLS_LE_HIX22, DAG));
  SDValue Lo = DAG.getNode(VEISD::Lo, DL, PtrVT,
                  withTargetFlags(Op, VEMCExpr::VK_VE_TLS_LE_LOX10, DAG));
  SDValue Offset =  DAG.getNode(ISD::XOR, DL, PtrVT, Hi, Lo);

  return DAG.getNode(ISD::ADD, DL, PtrVT,
                     DAG.getRegister(SP::G7, PtrVT), Offset);
}
#endif

#if 0
SDValue VETargetLowering::LowerF128_LibCallArg(SDValue Chain,
                                                  ArgListTy &Args, SDValue Arg,
                                                  const SDLoc &DL,
                                                  SelectionDAG &DAG) const {
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  EVT ArgVT = Arg.getValueType();
  Type *ArgTy = ArgVT.getTypeForEVT(*DAG.getContext());

  ArgListEntry Entry;
  Entry.Node = Arg;
  Entry.Ty   = ArgTy;

  if (ArgTy->isFP128Ty()) {
    // Create a stack object and pass the pointer to the library function.
    int FI = MFI.CreateStackObject(16, 8, false);
    SDValue FIPtr = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
    Chain = DAG.getStore(Chain, DL, Entry.Node, FIPtr, MachinePointerInfo(),
                         /* Alignment = */ 8);

    Entry.Node = FIPtr;
    Entry.Ty   = PointerType::getUnqual(ArgTy);
  }
  Args.push_back(Entry);
  return Chain;
}

SDValue
VETargetLowering::LowerF128Op(SDValue Op, SelectionDAG &DAG,
                                 const char *LibFuncName,
                                 unsigned numArgs) const {

  ArgListTy Args;

  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  SDValue Callee = DAG.getExternalSymbol(LibFuncName, PtrVT);
  Type *RetTy = Op.getValueType().getTypeForEVT(*DAG.getContext());
  Type *RetTyABI = RetTy;
  SDValue Chain = DAG.getEntryNode();
  SDValue RetPtr;

  if (RetTy->isFP128Ty()) {
    // Create a Stack Object to receive the return value of type f128.
    ArgListEntry Entry;
    int RetFI = MFI.CreateStackObject(16, 8, false);
    RetPtr = DAG.getFrameIndex(RetFI, PtrVT);
    Entry.Node = RetPtr;
    Entry.Ty   = PointerType::getUnqual(RetTy);
    if (!1)
      Entry.IsSRet = true;
    Entry.IsReturned = false;
    Args.push_back(Entry);
    RetTyABI = Type::getVoidTy(*DAG.getContext());
  }

  assert(Op->getNumOperands() >= numArgs && "Not enough operands!");
  for (unsigned i = 0, e = numArgs; i != e; ++i) {
    Chain = LowerF128_LibCallArg(Chain, Args, Op.getOperand(i), SDLoc(Op), DAG);
  }
  TargetLowering::CallLoweringInfo CLI(DAG);
  CLI.setDebugLoc(SDLoc(Op)).setChain(Chain)
    .setCallee(CallingConv::C, RetTyABI, Callee, std::move(Args));

  std::pair<SDValue, SDValue> CallInfo = LowerCallTo(CLI);

  // chain is in second result.
  if (RetTyABI == RetTy)
    return CallInfo.first;

  assert (RetTy->isFP128Ty() && "Unexpected return type!");

  Chain = CallInfo.second;

  // Load RetPtr to get the return value.
  return DAG.getLoad(Op.getValueType(), SDLoc(Op), Chain, RetPtr,
                     MachinePointerInfo(), /* Alignment = */ 8);
}
#endif

#if 0
SDValue VETargetLowering::LowerEH_SJLJ_SETJMP(SDValue Op, SelectionDAG &DAG,
    const VETargetLowering &TLI) const {
  SDLoc DL(Op);
  return DAG.getNode(VEISD::EH_SJLJ_SETJMP, DL,
      DAG.getVTList(MVT::i32, MVT::Other), Op.getOperand(0), Op.getOperand(1));

}

SDValue VETargetLowering::LowerEH_SJLJ_LONGJMP(SDValue Op, SelectionDAG &DAG,
    const VETargetLowering &TLI) const {
  SDLoc DL(Op);
  return DAG.getNode(VEISD::EH_SJLJ_LONGJMP, DL, MVT::Other, Op.getOperand(0), Op.getOperand(1));
}
#endif

static SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG,
                            const VETargetLowering &TLI) {
  MachineFunction &MF = DAG.getMachineFunction();
  VEMachineFunctionInfo *FuncInfo = MF.getInfo<VEMachineFunctionInfo>();
  auto PtrVT = TLI.getPointerTy(DAG.getDataLayout());

  // Need frame address to find the address of VarArgsFrameIndex.
  MF.getFrameInfo().setFrameAddressIsTaken(true);

  // vastart just stores the address of the VarArgsFrameIndex slot into the
  // memory location argument.
  SDLoc DL(Op);
  SDValue Offset =
      DAG.getNode(ISD::ADD, DL, PtrVT, DAG.getRegister(VE::SX9, PtrVT),
                  DAG.getIntPtrConstant(FuncInfo->getVarArgsFrameOffset(), DL));
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), DL, Offset, Op.getOperand(1),
                      MachinePointerInfo(SV));
}

static SDValue LowerVAARG(SDValue Op, SelectionDAG &DAG) {
  SDNode *Node = Op.getNode();
  EVT VT = Node->getValueType(0);
  SDValue InChain = Node->getOperand(0);
  SDValue VAListPtr = Node->getOperand(1);
  EVT PtrVT = VAListPtr.getValueType();
  const Value *SV = cast<SrcValueSDNode>(Node->getOperand(2))->getValue();
  SDLoc DL(Node);
  SDValue VAList =
      DAG.getLoad(PtrVT, DL, InChain, VAListPtr, MachinePointerInfo(SV));
  // Increment the pointer, VAList, by 8 to the next vaarg.
  SDValue NextPtr = DAG.getNode(ISD::ADD, DL, PtrVT, VAList,
                                DAG.getIntPtrConstant(8,
                                                      DL));
  // Store the incremented VAList to the legalized pointer.
  InChain = DAG.getStore(VAList.getValue(1), DL, NextPtr, VAListPtr,
                         MachinePointerInfo(SV));
  // Load the actual argument out of the pointer VAList.
  // We can't count on greater alignment than the word size.
  return DAG.getLoad(VT, DL, InChain, VAList, MachinePointerInfo(),
                     std::min(PtrVT.getSizeInBits(), VT.getSizeInBits()) / 8);
}

#if 0
static SDValue LowerDYNAMIC_STACKALLOC(SDValue Op, SelectionDAG &DAG,
                                       const VESubtarget *Subtarget) {
  SDValue Chain = Op.getOperand(0);  // Legalize the chain.
  SDValue Size  = Op.getOperand(1);  // Legalize the size.
  unsigned Align = cast<ConstantSDNode>(Op.getOperand(2))->getZExtValue();
  unsigned StackAlign = Subtarget->getFrameLowering()->getStackAlignment();
  EVT VT = Size->getValueType(0);
  SDLoc dl(Op);

  // TODO: implement over-aligned alloca. (Note: also implies
  // supporting support for overaligned function frames + dynamic
  // allocations, at all, which currently isn't supported)
  if (Align > StackAlign) {
    const MachineFunction &MF = DAG.getMachineFunction();
    report_fatal_error("Function \"" + Twine(MF.getName()) + "\": "
                       "over-aligned dynamic alloca not supported.");
  }

  // The resultant pointer needs to be above the register spill area
  // at the bottom of the stack.  The format is described in VESubtarget.cpp.
  unsigned regSpillArea = 176;

  unsigned SPReg = VE::SX11;
  SDValue SP = DAG.getCopyFromReg(Chain, dl, SPReg, VT);
  SDValue NewSP = DAG.getNode(ISD::SUB, dl, VT, SP, Size); // Value
  Chain = DAG.getCopyToReg(SP.getValue(1), dl, SPReg, NewSP);    // Output chain

  SDValue NewVal = DAG.getNode(ISD::ADD, dl, VT, NewSP,
                               DAG.getConstant(regSpillArea, dl, VT));
  SDValue Ops[2] = { NewVal, Chain };
  return DAG.getMergeValues(Ops, dl);
}
#endif

static SDValue LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG,
                              const VETargetLowering &TLI,
                              const VESubtarget *Subtarget) {
  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();

  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MFI.setFrameAddressIsTaken(true);

  EVT PtrVT = TLI.getPointerTy(MF.getDataLayout());

  // Naked functions never have a frame pointer, and so we use r1. For all
  // other functions, this decision must be delayed until during PEI.
  const VERegisterInfo *RegInfo = Subtarget->getRegisterInfo();
  unsigned FrameReg = RegInfo->getFrameRegister(MF);

  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl, FrameReg,
                                         PtrVT);
  while (Depth--)
    FrameAddr = DAG.getLoad(Op.getValueType(), dl, DAG.getEntryNode(),
                            FrameAddr, MachinePointerInfo());
  return FrameAddr;
}

static SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG,
                               const VETargetLowering &TLI,
                               const VESubtarget *Subtarget) {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MFI.setReturnAddressIsTaken(true);

  if (TLI.verifyReturnAddressArgumentIsConstant(Op, DAG))
    return SDValue();

#if 0
  SDValue RetAddr;
  if (depth == 0) {
    auto PtrVT = TLI.getPointerTy(DAG.getDataLayout());
    unsigned RetReg = MF.addLiveIn(VE::SX10, TLI.getRegClassFor(PtrVT));
    RetAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl, RetReg, VT);
    return RetAddr;
  }

  // Need frame address to find return address of the caller.
  SDValue FrameAddr = getFRAMEADDR(depth - 1, Op, DAG, Subtarget);

  unsigned Offset = (Subtarget->is64Bit()) ? 120 : 60;
  SDValue Ptr = DAG.getNode(ISD::ADD,
                            dl, VT,
                            FrameAddr,
                            DAG.getIntPtrConstant(Offset, dl));
  RetAddr = DAG.getLoad(VT, dl, DAG.getEntryNode(), Ptr, MachinePointerInfo());

  return RetAddr;
#endif

  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();

  auto PtrVT = TLI.getPointerTy(MF.getDataLayout());

  if (Depth > 0) {
    SDValue FrameAddr = LowerFRAMEADDR(Op, DAG, TLI, Subtarget);
    SDValue Offset =
        DAG.getConstant(8, dl, MVT::i64);
    return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(),
                       DAG.getNode(ISD::ADD, dl, PtrVT, FrameAddr, Offset),
                       MachinePointerInfo());
  }

  // Just load the return address off the stack.
  SDValue RetAddrFI = DAG.getFrameIndex(1, PtrVT);
  return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), RetAddrFI,
                     MachinePointerInfo());
}

// Lower a f128 load into two f64 loads.
static SDValue LowerF128Load(SDValue Op, SelectionDAG &DAG)
{
  SDLoc dl(Op);
  LoadSDNode *LdNode = dyn_cast<LoadSDNode>(Op.getNode());
  assert(LdNode && LdNode->getOffset().isUndef()
         && "Unexpected node type");

  unsigned alignment = LdNode->getAlignment();
  if (alignment > 8)
    alignment = 8;

  SDValue Hi64 =
      DAG.getLoad(MVT::i64, dl, LdNode->getChain(), LdNode->getBasePtr(),
                  LdNode->getPointerInfo(), alignment,
                  LdNode->isVolatile() ? MachineMemOperand::MOVolatile :
                                         MachineMemOperand::MONone);
  EVT addrVT = LdNode->getBasePtr().getValueType();
  SDValue LoPtr = DAG.getNode(ISD::ADD, dl, addrVT,
                              LdNode->getBasePtr(),
                              DAG.getConstant(8, dl, addrVT));
  SDValue Lo64 =
      DAG.getLoad(MVT::i64, dl, LdNode->getChain(), LoPtr,
                  LdNode->getPointerInfo(), alignment,
                  LdNode->isVolatile() ? MachineMemOperand::MOVolatile :
                                         MachineMemOperand::MONone);

  SDValue SubRegEven = DAG.getTargetConstant(VE::sub_even, dl, MVT::i32);
  SDValue SubRegOdd  = DAG.getTargetConstant(VE::sub_odd, dl, MVT::i32);

  // VE stores LowReg to 8(addr) and HiReg to 0(addr)
  SDNode *InFP128 = DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF,
                                       dl, MVT::f128);
  InFP128 = DAG.getMachineNode(TargetOpcode::INSERT_SUBREG, dl,
                               MVT::f128,
                               SDValue(InFP128, 0),
                               Lo64,
                               SubRegEven);
  InFP128 = DAG.getMachineNode(TargetOpcode::INSERT_SUBREG, dl,
                               MVT::f128,
                               SDValue(InFP128, 0),
                               Hi64,
                               SubRegOdd);
  SDValue OutChains[2] = { SDValue(Hi64.getNode(), 1),
                           SDValue(Lo64.getNode(), 1) };
  SDValue OutChain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
  SDValue Ops[2] = {SDValue(InFP128,0), OutChain};
  return DAG.getMergeValues(Ops, dl);
}

static SDValue LowerLOAD(SDValue Op, SelectionDAG &DAG)
{
  LoadSDNode *LdNode = cast<LoadSDNode>(Op.getNode());

  EVT MemVT = LdNode->getMemoryVT();
  if (MemVT == MVT::f128)
    return LowerF128Load(Op, DAG);

  return Op;
}

// Lower a f128 store into two f64 stores.
static SDValue LowerF128Store(SDValue Op, SelectionDAG &DAG) {
  SDLoc dl(Op);
  StoreSDNode *StNode = dyn_cast<StoreSDNode>(Op.getNode());
  assert(StNode && StNode->getOffset().isUndef()
         && "Unexpected node type");
  SDValue SubRegEven = DAG.getTargetConstant(VE::sub_even, dl, MVT::i32);
  SDValue SubRegOdd  = DAG.getTargetConstant(VE::sub_odd, dl, MVT::i32);

  SDNode *Hi64 = DAG.getMachineNode(TargetOpcode::EXTRACT_SUBREG,
                                    dl,
                                    MVT::i64,
                                    StNode->getValue(),
                                    SubRegEven);
  SDNode *Lo64 = DAG.getMachineNode(TargetOpcode::EXTRACT_SUBREG,
                                    dl,
                                    MVT::i64,
                                    StNode->getValue(),
                                    SubRegOdd);

  unsigned alignment = StNode->getAlignment();
  if (alignment > 8)
    alignment = 8;

  // VE stores LowReg to 8(addr) and HiReg to 0(addr)
  SDValue OutChains[2];
  OutChains[0] =
      DAG.getStore(StNode->getChain(), dl, SDValue(Lo64, 0),
                   StNode->getBasePtr(), MachinePointerInfo(), alignment,
                   StNode->isVolatile() ? MachineMemOperand::MOVolatile :
                                          MachineMemOperand::MONone);
  EVT addrVT = StNode->getBasePtr().getValueType();
  SDValue LoPtr = DAG.getNode(ISD::ADD, dl, addrVT,
                              StNode->getBasePtr(),
                              DAG.getConstant(8, dl, addrVT));
  OutChains[1] =
      DAG.getStore(StNode->getChain(), dl, SDValue(Hi64, 0), LoPtr,
                   MachinePointerInfo(), alignment,
                   StNode->isVolatile() ? MachineMemOperand::MOVolatile :
                                          MachineMemOperand::MONone);
  return DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
}

static SDValue LowerSTORE(SDValue Op, SelectionDAG &DAG)
{
  SDLoc dl(Op);
  StoreSDNode *St = cast<StoreSDNode>(Op.getNode());

  EVT MemVT = St->getMemoryVT();
  if (MemVT == MVT::f128)
    return LowerF128Store(Op, DAG);

  return SDValue();
}

#if 0
// Custom lower UMULO/SMULO for SPARC. This code is similar to ExpandNode()
// in LegalizeDAG.cpp except the order of arguments to the library function.
static SDValue LowerUMULO_SMULO(SDValue Op, SelectionDAG &DAG,
                                const VETargetLowering &TLI)
{
  unsigned opcode = Op.getOpcode();
  assert((opcode == ISD::UMULO || opcode == ISD::SMULO) && "Invalid Opcode.");

  bool isSigned = (opcode == ISD::SMULO);
  EVT VT = MVT::i64;
  EVT WideVT = MVT::i128;
  SDLoc dl(Op);
  SDValue LHS = Op.getOperand(0);

  if (LHS.getValueType() != VT)
    return Op;

  SDValue ShiftAmt = DAG.getConstant(63, dl, VT);

  SDValue RHS = Op.getOperand(1);
  SDValue HiLHS = DAG.getNode(ISD::SRA, dl, VT, LHS, ShiftAmt);
  SDValue HiRHS = DAG.getNode(ISD::SRA, dl, MVT::i64, RHS, ShiftAmt);
  SDValue Args[] = { HiLHS, LHS, HiRHS, RHS };

  SDValue MulResult = TLI.makeLibCall(DAG,
                                      RTLIB::MUL_I128, WideVT,
                                      Args, isSigned, dl).first;
  SDValue BottomHalf = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, VT,
                                   MulResult, DAG.getIntPtrConstant(0, dl));
  SDValue TopHalf = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, VT,
                                MulResult, DAG.getIntPtrConstant(1, dl));
  if (isSigned) {
    SDValue Tmp1 = DAG.getNode(ISD::SRA, dl, VT, BottomHalf, ShiftAmt);
    TopHalf = DAG.getSetCC(dl, MVT::i32, TopHalf, Tmp1, ISD::SETNE);
  } else {
    TopHalf = DAG.getSetCC(dl, MVT::i32, TopHalf, DAG.getConstant(0, dl, VT),
                           ISD::SETNE);
  }
  // MulResult is a node with an illegal type. Because such things are not
  // generally permitted during this phase of legalization, ensure that
  // nothing is left using the node. The above EXTRACT_ELEMENT nodes should have
  // been folded.
  assert(MulResult->use_empty() && "Illegally typed node still in use!");

  SDValue Ops[2] = { BottomHalf, TopHalf } ;
  return DAG.getMergeValues(Ops, dl);
}

static SDValue LowerATOMIC_LOAD_STORE(SDValue Op, SelectionDAG &DAG) {
  if (isStrongerThanMonotonic(cast<AtomicSDNode>(Op)->getOrdering()))
  // Expand with a fence.
  return SDValue();

  // Monotonic load/stores are legal.
  return Op;
}
#endif

SDValue VETargetLowering::LowerINTRINSIC_WO_CHAIN(SDValue Op,
                                                     SelectionDAG &DAG) const {
  unsigned IntNo = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  SDLoc dl(Op);
  switch (IntNo) {
  default: return SDValue();    // Don't custom lower most intrinsics.
  case Intrinsic::thread_pointer: {
    report_fatal_error("Intrinsic::thread_point is not implemented yet");
#if 0
    EVT PtrVT = getPointerTy(DAG.getDataLayout());
    return DAG.getRegister(SP::G7, PtrVT);
#endif
  }
  case Intrinsic::ve_vfdivsA_vvv: {
/*
    600000000b98:       00 00 01 05     vrcp.s          %v5,%v1,%vm1
    600000000ba0:       00 00 80 3f     lea.sl          %s0,0x3f800000(0,0)
    600000000ba8:       05 01 00 04     vfnmsb.s        %v4,%s0,%v1,%v5,%vm1
    600000000bb0:       04 05 05 03     vfmad.s         %v3,%v5,%v5,%v4,%vm1
    600000000bb8:       00 03 00 02     vfmul.s         %v2,%v0,%v3,%vm1
    600000000bc0:       01 02 00 04     vfnmsb.s        %v4,%v0,%v2,%v1,%vm1
    600000000bc8:       04 05 02 02     vfmad.s         %v2,%v2,%v5,%v4,%vm1
    600000000bd0:       01 02 00 00     vfnmsb.s        %v0,%v0,%v2,%v1,%vm1
    600000000bd8:       00 03 02 00     vfmad.s         %v0,%v2,%v3,%v0,%vm1
    600000000be0:       00 00 00 00     b.l.t           0x0(,%s10)
 */
    // Op0: function id, Op1: V64, Op1: V64
    SDLoc dl(Op);

    EVT VT = Op.getValueType();
    SDValue S0;
    SDValue V0, V1, V2, V3, V4, V5;
    SDValue SubRegF32 = DAG.getTargetConstant(VE::sub_f32, dl, MVT::i32);

    V0 = Op.getOperand(1);
    V1 = Op.getOperand(2);

    V5 = SDValue(DAG.getMachineNode(VE::VRCPsv, dl, VT, V1), 0);  // V5 = 1.0f / V1
    S0 = SDValue(DAG.getMachineNode(VE::LEASLzzi, dl, MVT::i64,
                                    DAG.getTargetConstant(0x3f800000, dl, MVT::i64)), 0); // S0 = 1.0f
    S0 = SDValue(DAG.getMachineNode(TargetOpcode::EXTRACT_SUBREG, dl, MVT::f32,
                                    S0, SubRegF32), 0);
    V4 = SDValue(DAG.getMachineNode(VE::VFNMSBsr, dl, VT, S0, V1, V5), 0); // V4 = -(V1*V5-S0)
    V3 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V5, V5, V4), 0); // V3 = V5*V4+V5
    V2 = SDValue(DAG.getMachineNode(VE::VFMPsv,   dl, VT, V0, V3), 0);     // V1 = V0*V3
    V4 = SDValue(DAG.getMachineNode(VE::VFNMSBsv, dl, VT, V0, V2, V1), 0); // V4 = -(V2*V1-V0)
    V2 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V2, V5, V4), 0); // V2 = V5*V4+V2
    V0 = SDValue(DAG.getMachineNode(VE::VFNMSBsv, dl, VT, V0, V2, V1), 0); // v3 = -(V2*V1-S0)
    V0 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V2, V3, V0), 0); // V0 = V3*V0+V2
    return V0;
  }
#if 1
  case Intrinsic::ve_pvfdivA_vvv: {
    // Op0: function id, Op1: V64, Op1: V64
    SDLoc dl(Op);

    EVT VT = Op.getValueType();
    SDValue S0, S1;
    SDValue V0, V1, V2, V3, V4, V5;

    V0 = Op.getOperand(1);
    V1 = Op.getOperand(2);

    V5 = SDValue(DAG.getMachineNode(VE::VRCPpv, dl, VT, V1), 0);  // V5 = 1.0f / V1
    // S0 = 1.0f|1.0f
    S1 = SDValue(DAG.getMachineNode(VE::LEAzzi, dl, MVT::i64,
                                    DAG.getTargetConstant(0x3f800000, dl, MVT::i64)), 0);
    S0 = SDValue(DAG.getMachineNode(VE::LEASLrzi, dl, MVT::i64,
                                    S1,
                                    DAG.getTargetConstant(0x3f800000, dl, MVT::i64)), 0);
    V4 = SDValue(DAG.getMachineNode(VE::VFNMSBpr, dl, VT, S0, V1, V5), 0); // V4 = -(V1*V5-S0)
    V3 = SDValue(DAG.getMachineNode(VE::VFMADpv,  dl, VT, V5, V5, V4), 0); // V3 = V5*V4+V5
    V2 = SDValue(DAG.getMachineNode(VE::VFMPpv,   dl, VT, V0, V3), 0);     // V1 = V0*V3
    V4 = SDValue(DAG.getMachineNode(VE::VFNMSBpv, dl, VT, V0, V2, V1), 0); // V4 = -(V2*V1-V0)
    V2 = SDValue(DAG.getMachineNode(VE::VFMADpv,  dl, VT, V2, V5, V4), 0); // V2 = V5*V4+V2
    V0 = SDValue(DAG.getMachineNode(VE::VFNMSBpv, dl, VT, V0, V2, V1), 0); // v3 = -(V2*V1-S0)
    V0 = SDValue(DAG.getMachineNode(VE::VFMADpv,  dl, VT, V2, V3, V0), 0); // V0 = V3*V0+V2
    return V0;
  }
#endif
  case Intrinsic::ve_vfdivsA_vsv: {
/*
600000000c68:       00 00 00 04     vrcp.s  %v4,%v0,%vm1
600000000c70:       00 00 80 3f     lea.sl  %s1,0x3f800000(0,0)
600000000c78:       04 00 00 02     vfnmsb.s        %v2,%s1,%v0,%v4,%vm1
600000000c80:       02 04 04 02     vfmad.s %v2,%v4,%v4,%v2,%vm1
600000000c88:       00 02 00 01     vfmul.s %v1,%s0,%v2,%vm1
600000000c90:       00 01 00 03     vfnmsb.s        %v3,%s0,%v1,%v0,%vm1
600000000c98:       03 04 01 01     vfmad.s %v1,%v1,%v4,%v3,%vm1
600000000ca0:       00 01 00 03     vfnmsb.s        %v3,%s0,%v1,%v0,%vm1
600000000ca8:       03 02 01 00     vfmad.s %v0,%v1,%v2,%v3,%vm1
600000000cb0:       00 00 00 00     b.l.t   0x0(,%s10)
 */
    // Op0: function id, Op1: f32, Op1: V64  (f32/V64)
    SDLoc dl(Op);

    EVT VT = Op.getValueType();
    SDValue S0, S1;
    SDValue V0, V1, V2, V3, V4;
    SDValue SubRegF32 = DAG.getTargetConstant(VE::sub_f32, dl, MVT::i32);

    S0 = Op.getOperand(1);
    V0 = Op.getOperand(2);

    V4 = SDValue(DAG.getMachineNode(VE::VRCPsv, dl, VT, V0), 0);  // V4 = 1.0f / V0
    S1 = SDValue(DAG.getMachineNode(VE::LEASLzzi, dl, MVT::i64,
                                    DAG.getTargetConstant(0x3f800000, dl, MVT::i64)), 0); // S1 = 1.0f
    S1 = SDValue(DAG.getMachineNode(TargetOpcode::EXTRACT_SUBREG, dl, MVT::f32,
                                    S1, SubRegF32), 0);
    V2 = SDValue(DAG.getMachineNode(VE::VFNMSBsr, dl, VT, S1, V0, V4), 0); // V2 = -(V0*V4-S1)
    V2 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V4, V4, V2), 0); // V2 = V4*V2+V4
    V1 = SDValue(DAG.getMachineNode(VE::VFMPsr,   dl, VT, S0, V2), 0);     // V1 = S0*V2
    V3 = SDValue(DAG.getMachineNode(VE::VFNMSBsr, dl, VT, S0, V1, V0), 0); // V3 = -(V1*V0-S0)
    V1 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V1, V4, V3), 0); // V1 = V4*V3+V1
    V3 = SDValue(DAG.getMachineNode(VE::VFNMSBsr, dl, VT, S0, V1, V0), 0); // v3 = -(V1*V0-S0)
    V0 = SDValue(DAG.getMachineNode(VE::VFMADsv,  dl, VT, V1, V2, V3), 0); // V0 = V2*V3+V1
    return V0;
  }
  case Intrinsic::ve_vec_call: {
    // Op0: function id, Op1: input V64, Op2: address
    SDLoc dl(Op);
    SDValue Chain = DAG.getEntryNode();
    SDValue InGlue;

    // create copy from input to V0
    Chain = DAG.getCopyToReg(Chain, dl, VE::V0, Op.getOperand(1), InGlue);
    InGlue = Chain.getValue(1);

    // create CALL node
    SmallVector<SDValue, 8> Ops;
    Ops.push_back(Chain);
    Ops.push_back(Op.getOperand(2));
    Ops.push_back(DAG.getRegister(VE::V0, MVT::v256f64));

    // preserved registers
    const VERegisterInfo *TRI = Subtarget->getRegisterInfo();
    const uint32_t *Mask = TRI->getCallPreservedMask(DAG.getMachineFunction(),
        CallingConv::VE_VEC_EXPF);
    assert(Mask && "Missing call preserved mask for calling convention");
    Ops.push_back(DAG.getRegisterMask(Mask));

    if (InGlue.getNode())
        Ops.push_back(InGlue);
    SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
    Chain = DAG.getNode(VEISD::CALL, dl, NodeTys, Ops);
    InGlue = Chain.getValue(1);

    // create copy from V0 to output
    SDValue RV = DAG.getCopyFromReg(Chain, dl, VE::V0, MVT::v256f64, InGlue);
    return RV;
  }
  }
}

SDValue VETargetLowering::
LowerOperation(SDValue Op, SelectionDAG &DAG) const {

#if 0
  bool hasHardQuad = Subtarget->hasHardQuad();
  bool isV9        = Subtarget->isV9();
#endif

  switch (Op.getOpcode()) {
  default: llvm_unreachable("Should not custom lower this!");

  case ISD::RETURNADDR:         return LowerRETURNADDR(Op, DAG, *this,
                                                       Subtarget);
  case ISD::FRAMEADDR:          return LowerFRAMEADDR(Op, DAG, *this,
                                                      Subtarget);
  case ISD::GlobalTLSAddress:   // return LowerGlobalTLSAddress(Op, DAG);
    report_fatal_error("GlobalTLSAddress expansion is not implemented yet");
  case ISD::GlobalAddress:      return LowerGlobalAddress(Op, DAG);
  case ISD::BlockAddress:       return LowerBlockAddress(Op, DAG);
  case ISD::ConstantPool:       return LowerConstantPool(Op, DAG);
  case ISD::EH_SJLJ_SETJMP:     // return LowerEH_SJLJ_SETJMP(Op, DAG, *this);
    report_fatal_error("EH_SJLJ_SETJMP expansion is not implemented yet");
  case ISD::EH_SJLJ_LONGJMP:    // return LowerEH_SJLJ_LONGJMP(Op, DAG, *this);
    report_fatal_error("EH_SJLJ_LONGJMP expansion is not implemented yet");
  case ISD::VASTART:            return LowerVASTART(Op, DAG, *this);
  case ISD::VAARG:              return LowerVAARG(Op, DAG);
  case ISD::DYNAMIC_STACKALLOC: // return LowerDYNAMIC_STACKALLOC(Op, DAG,
                                //                                Subtarget);
    report_fatal_error("DYNAMIC_STACKALLOC expansion is not implemented yet");

  case ISD::LOAD:               return LowerLOAD(Op, DAG);
  case ISD::STORE:              return LowerSTORE(Op, DAG);
  case ISD::FDIV:               // return LowerF128Op(Op, DAG,
                                //        getLibcallName(RTLIB::DIV_F128), 2);
    report_fatal_error("FDIV expansion is not implemented yet");
  case ISD::FSQRT:              // return LowerF128Op(Op, DAG,
                                //        getLibcallName(RTLIB::SQRT_F128),1);
    report_fatal_error("FSQRT expansion is not implemented yet");
  case ISD::UMULO:
  case ISD::SMULO:              // return LowerUMULO_SMULO(Op, DAG, *this);
    report_fatal_error("UMULO or SMULO expansion is not implemented yet");
  case ISD::ATOMIC_LOAD:
  case ISD::ATOMIC_STORE:       // return LowerATOMIC_LOAD_STORE(Op, DAG);
    report_fatal_error("ATOMIC_LOAD or ATOMIC_STORE expansion is not implemented yet");
  case ISD::INTRINSIC_WO_CHAIN: return LowerINTRINSIC_WO_CHAIN(Op, DAG);
    //report_fatal_error("INTRINSIC_WO_CHAIN expansion is not implemented yet");
  case ISD::BUILD_VECTOR:      return LowerBuildVector(Op, DAG);
  }
}

MachineBasicBlock *
VETargetLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                 MachineBasicBlock *BB) const {
  switch (MI.getOpcode()) {
  default: llvm_unreachable("Unknown Custom Instruction!");
#if 0
  case SP::EH_SJLJ_SETJMP32ri:
  case SP::EH_SJLJ_SETJMP32rr:
    return emitEHSjLjSetJmp(MI, BB);
  case SP::EH_SJLJ_LONGJMP32rr:
  case SP::EH_SJLJ_LONGJMP32ri:
    return emitEHSjLjLongJmp(MI, BB);
#endif
  }
}

#if 0
MachineBasicBlock *
VETargetLowering::emitEHSjLjLongJmp(MachineInstr &MI,
                                       MachineBasicBlock *MBB) const {
  DebugLoc DL = MI.getDebugLoc();
  const TargetInstrInfo *TII = Subtarget->getInstrInfo();

  MachineFunction *MF = MBB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  MachineInstrBuilder MIB;

  MVT PVT = getPointerTy(MF->getDataLayout());
  unsigned RegSize = PVT.getStoreSize();
  assert(PVT == MVT::i32 && "Invalid Pointer Size!");

  unsigned Buf = MI.getOperand(0).getReg();
  unsigned JmpLoc = MRI.createVirtualRegister(&SP::I64RegClass);

  // TO DO: If we do 64-bit handling, this perhaps should be FLUSHW, not TA 3
  MIB = BuildMI(*MBB, MI, DL, TII->get(SP::TRAPri), SP::G0).addImm(3).addImm(SPCC::ICC_A);

  // Instruction to restore FP
  const unsigned FP  = SP::I6;
  MIB = BuildMI(*MBB, MI, DL, TII->get(SP::LDri))
            .addReg(FP)
            .addReg(Buf)
            .addImm(0);

  // Instruction to load jmp location
  MIB = BuildMI(*MBB, MI, DL, TII->get(SP::LDri))
            .addReg(JmpLoc, RegState::Define)
            .addReg(Buf)
            .addImm(RegSize);

  // Instruction to restore SP
  const unsigned SP  = VE::SX11;
  MIB = BuildMI(*MBB, MI, DL, TII->get(SP::LDri))
            .addReg(SP)
            .addReg(Buf)
            .addImm(2 * RegSize);

  // Instruction to restore I7
  MIB = BuildMI(*MBB, MI, DL, TII->get(SP::LDri))
            .addReg(SP::I7)
            .addReg(Buf, RegState::Kill)
            .addImm(3 * RegSize);

  // Jump to JmpLoc
  BuildMI(*MBB, MI, DL, TII->get(SP::JMPLrr)).addReg(SP::G0).addReg(JmpLoc, RegState::Kill).addReg(SP::G0);

  MI.eraseFromParent();
  return MBB;
}

MachineBasicBlock *
VETargetLowering::emitEHSjLjSetJmp(MachineInstr &MI,
                                      MachineBasicBlock *MBB) const {
  DebugLoc DL = MI.getDebugLoc();
  const TargetInstrInfo *TII = Subtarget->getInstrInfo();
  const TargetRegisterInfo *TRI = Subtarget->getRegisterInfo();

  MachineFunction *MF = MBB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  MachineInstrBuilder MIB;

  MVT PVT = getPointerTy(MF->getDataLayout());
  unsigned RegSize = PVT.getStoreSize();
  assert(PVT == MVT::i32 && "Invalid Pointer Size!");

  unsigned DstReg = MI.getOperand(0).getReg();
  const TargetRegisterClass *RC = MRI.getRegClass(DstReg);
  assert(TRI->isTypeLegalForClass(*RC, MVT::i32) && "Invalid destination!");
  (void)TRI;
  unsigned mainDstReg = MRI.createVirtualRegister(RC);
  unsigned restoreDstReg = MRI.createVirtualRegister(RC);

  // For v = setjmp(buf), we generate
  //
  // thisMBB:
  //  buf[0] = FP
  //  buf[RegSize] = restoreMBB <-- takes address of restoreMBB
  //  buf[RegSize * 2] = O6
  //  buf[RegSize * 3] = I7
  //  Ensure restoreMBB remains in the relocations list (done using a bn instruction)
  //  b mainMBB
  //
  // mainMBB:
  //  v_main = 0
  //  b sinkMBB
  //
  // restoreMBB:
  //  v_restore = 1
  //  --fall through--
  //
  // sinkMBB:
  //  v = phi(main, restore)

  const BasicBlock *BB = MBB->getBasicBlock();
  MachineFunction::iterator It = ++MBB->getIterator();
  MachineBasicBlock *thisMBB = MBB;
  MachineBasicBlock *mainMBB = MF->CreateMachineBasicBlock(BB);
  MachineBasicBlock *restoreMBB = MF->CreateMachineBasicBlock(BB);
  MachineBasicBlock *sinkMBB = MF->CreateMachineBasicBlock(BB);

  MF->insert(It, mainMBB);
  MF->insert(It, restoreMBB);
  MF->insert(It, sinkMBB);
  restoreMBB->setHasAddressTaken();

  // Transfer the remainder of BB and its successor edges to sinkMBB.
  sinkMBB->splice(sinkMBB->begin(), MBB,
                  std::next(MachineBasicBlock::iterator(MI)),
                  MBB->end());
  sinkMBB->transferSuccessorsAndUpdatePHIs(MBB);

  unsigned LabelReg = MRI.createVirtualRegister(&SP::I64RegClass);
  unsigned LabelReg2 = MRI.createVirtualRegister(&SP::I64RegClass);
  unsigned BufReg = MI.getOperand(1).getReg();

  // Instruction to store FP
  const unsigned FP  = SP::I6;
  MIB = BuildMI(thisMBB, DL, TII->get(SP::STri))
            .addReg(BufReg)
            .addImm(0)
            .addReg(FP);

  // Instructions to store jmp location
  MIB = BuildMI(thisMBB, DL, TII->get(SP::SETHIi))
            .addReg(LabelReg, RegState::Define)
            .addMBB(restoreMBB, VEMCExpr::VK_VE_HI);

  MIB = BuildMI(thisMBB, DL, TII->get(SP::ORri))
            .addReg(LabelReg2, RegState::Define)
            .addReg(LabelReg, RegState::Kill)
            .addMBB(restoreMBB, VEMCExpr::VK_VE_LO);

  MIB = BuildMI(thisMBB, DL, TII->get(SP::STri))
            .addReg(BufReg)
            .addImm(RegSize)
            .addReg(LabelReg2, RegState::Kill);

  // Instruction to store SP
  const unsigned SP  = VE::SX11;
  MIB = BuildMI(thisMBB, DL, TII->get(SP::STri))
            .addReg(BufReg)
            .addImm(2 * RegSize)
            .addReg(SP);

  // Instruction to store I7
  MIB = BuildMI(thisMBB, DL, TII->get(SP::STri))
            .addReg(BufReg)
            .addImm(3 * RegSize)
            .addReg(SP::I7);


  // FIX ME: This next instruction ensures that the restoreMBB block address remains
  // valid through optimization passes and serves no other purpose. The ICC_N ensures
  // that the branch is never taken. This commented-out code here was an alternative
  // attempt to achieve this which brought myriad problems.
  //MIB = BuildMI(thisMBB, DL, TII->get(SP::EH_SjLj_Setup)).addMBB(restoreMBB, VEMCExpr::VK_VE_None);
  MIB = BuildMI(thisMBB, DL, TII->get(SP::BCOND))
              .addMBB(restoreMBB)
              .addImm(SPCC::ICC_N);

  MIB = BuildMI(thisMBB, DL, TII->get(SP::BCOND))
              .addMBB(mainMBB)
              .addImm(SPCC::ICC_A);

  thisMBB->addSuccessor(mainMBB);
  thisMBB->addSuccessor(restoreMBB);


  // mainMBB:
  MIB = BuildMI(mainMBB, DL, TII->get(SP::ORrr))
             .addReg(mainDstReg, RegState::Define)
             .addReg(SP::G0)
             .addReg(SP::G0);
  MIB = BuildMI(mainMBB, DL, TII->get(SP::BCOND)).addMBB(sinkMBB).addImm(SPCC::ICC_A);

  mainMBB->addSuccessor(sinkMBB);


  // restoreMBB:
  MIB = BuildMI(restoreMBB, DL, TII->get(SP::ORri))
              .addReg(restoreDstReg, RegState::Define)
              .addReg(SP::G0)
              .addImm(1);
  //MIB = BuildMI(restoreMBB, DL, TII->get(SP::BCOND)).addMBB(sinkMBB).addImm(SPCC::ICC_A);
  restoreMBB->addSuccessor(sinkMBB);

  // sinkMBB:
  MIB = BuildMI(*sinkMBB, sinkMBB->begin(), DL,
                TII->get(SP::PHI), DstReg)
             .addReg(mainDstReg).addMBB(mainMBB)
             .addReg(restoreDstReg).addMBB(restoreMBB);

  MI.eraseFromParent();
  return sinkMBB;
}
#endif

//===----------------------------------------------------------------------===//
//                         VE Inline Assembly Support
//===----------------------------------------------------------------------===//

/// getConstraintType - Given a constraint letter, return the type of
/// constraint it is for this target.
VETargetLowering::ConstraintType
VETargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    default:  break;
    case 'r':
    case 'f':
    case 'e':
      return C_RegisterClass;
    case 'I': // SIMM13
      return C_Other;
    }
  }

  return TargetLowering::getConstraintType(Constraint);
}

TargetLowering::ConstraintWeight VETargetLowering::
getSingleConstraintMatchWeight(AsmOperandInfo &info,
                               const char *constraint) const {
  ConstraintWeight weight = CW_Invalid;
  Value *CallOperandVal = info.CallOperandVal;
  // If we don't have a value, we can't do a match,
  // but allow it at the lowest weight.
  if (!CallOperandVal)
    return CW_Default;

  // Look at the constraint type.
  switch (*constraint) {
  default:
    weight = TargetLowering::getSingleConstraintMatchWeight(info, constraint);
    break;
  case 'I': // SIMM13
    if (ConstantInt *C = dyn_cast<ConstantInt>(info.CallOperandVal)) {
      if (isInt<13>(C->getSExtValue()))
        weight = CW_Constant;
    }
    break;
  }
  return weight;
}

/// LowerAsmOperandForConstraint - Lower the specified operand into the Ops
/// vector.  If it is invalid, don't add anything to Ops.
void VETargetLowering::
LowerAsmOperandForConstraint(SDValue Op,
                             std::string &Constraint,
                             std::vector<SDValue> &Ops,
                             SelectionDAG &DAG) const {
  SDValue Result(nullptr, 0);

  // Only support length 1 constraints for now.
  if (Constraint.length() > 1)
    return;

  char ConstraintLetter = Constraint[0];
  switch (ConstraintLetter) {
  default: break;
  case 'I':
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (isInt<13>(C->getSExtValue())) {
        Result = DAG.getTargetConstant(C->getSExtValue(), SDLoc(Op),
                                       Op.getValueType());
        break;
      }
      return;
    }
  }

  if (Result.getNode()) {
    Ops.push_back(Result);
    return;
  }
  TargetLowering::LowerAsmOperandForConstraint(Op, Constraint, Ops, DAG);
}

std::pair<unsigned, const TargetRegisterClass *>
VETargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                  StringRef Constraint,
                                                  MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return std::make_pair(0U, &VE::I64RegClass);
    case 'f':
      if (VT == MVT::f32 || VT == MVT::f64)
        return std::make_pair(0U, &VE::I64RegClass);
      else if (VT == MVT::f128)
        return std::make_pair(0U, &VE::F128RegClass);
      llvm_unreachable("Unknown ValueType for f-register-type!");
      break;
    case 'e':
      if (VT == MVT::f32 || VT == MVT::f64)
        return std::make_pair(0U, &VE::I64RegClass);
      else if (VT == MVT::f128)
        return std::make_pair(0U, &VE::F128RegClass);
      llvm_unreachable("Unknown ValueType for e-register-type!");
      break;
    }
  } else if (!Constraint.empty() && Constraint.size() <= 5
              && Constraint[0] == '{' && *(Constraint.end()-1) == '}') {
    // constraint = '{r<d>}'
    // Remove the braces from around the name.
    StringRef name(Constraint.data()+1, Constraint.size()-2);
    // Handle register aliases:
    //       r0-r7   -> g0-g7
    //       r8-r15  -> o0-o7
    //       r16-r23 -> l0-l7
    //       r24-r31 -> i0-i7
    uint64_t intVal = 0;
    if (name.substr(0, 1).equals("r")
        && !name.substr(1).getAsInteger(10, intVal) && intVal <= 31) {
      const char regTypes[] = { 'g', 'o', 'l', 'i' };
      char regType = regTypes[intVal/8];
      char regIdx = '0' + (intVal % 8);
      char tmp[] = { '{', regType, regIdx, '}', 0 };
      std::string newConstraint = std::string(tmp);
      return TargetLowering::getRegForInlineAsmConstraint(TRI, newConstraint,
                                                          VT);
    }
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

bool
VETargetLowering::isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const {
  // The VE target isn't yet aware of offsets.
  return false;
}

void VETargetLowering::ReplaceNodeResults(SDNode *N,
                                             SmallVectorImpl<SDValue>& Results,
                                             SelectionDAG &DAG) const {

  SDLoc dl(N);

  switch (N->getOpcode()) {
  default:
    llvm_unreachable("Do not know how to custom type legalize this operation!");
  }
}

// Override to enable LOAD_STACK_GUARD lowering on Linux.
bool VETargetLowering::useLoadStackGuardNode() const {
  if (!Subtarget->isTargetLinux())
    return TargetLowering::useLoadStackGuardNode();
  return true;
}

// Override to disable global variable loading on Linux.
void VETargetLowering::insertSSPDeclarations(Module &M) const {
  if (!Subtarget->isTargetLinux())
    return TargetLowering::insertSSPDeclarations(M);
}
