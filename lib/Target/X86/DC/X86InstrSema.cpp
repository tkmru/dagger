#include "X86InstrSema.h"
#include "X86RegisterSema.h"
#include "InstPrinter/X86ATTInstPrinter.h"
#include "InstPrinter/X86IntelInstPrinter.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86ISelLowering.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypeBuilder.h"

#include "X86GenSema.inc"

using namespace llvm;

X86InstrSema::X86InstrSema(DCRegisterSema &DRS)
    : DCInstrSema(X86::OpcodeToSemaIdx, X86::InstSemantics, X86::ConstantArray,
                  DRS),
      X86DRS((X86RegisterSema &)DRS), LastPrefix(0) {}

bool X86InstrSema::translateTargetInst() {
  unsigned Opcode = CurrentInst->Inst.getOpcode();

  if (LastPrefix) {
    unsigned Prefix = LastPrefix;
    // Reset the prefix.
    LastPrefix = 0;

    if (Prefix == X86::LOCK_PREFIX) {
      unsigned XADDMemOpType = X86::OpTypes::OPERAND_TYPE_INVALID;
      bool isINCDEC = false;
      AtomicRMWInst::BinOp AtomicOpc = AtomicRMWInst::BAD_BINOP;
      Instruction::BinaryOps Opc = Instruction::FAdd; // Invalid initializer
      switch (Opcode) {
      default: break;
      case X86::XADD8rm:
        XADDMemOpType = X86::OpTypes::i8mem; break;
      case X86::XADD16rm:
        XADDMemOpType = X86::OpTypes::i16mem; break;
      case X86::XADD32rm:
        XADDMemOpType = X86::OpTypes::i32mem; break;
      case X86::XADD64rm:
        XADDMemOpType = X86::OpTypes::i64mem; break;

      case X86::INC8m: case X86::INC16m: case X86::INC32m: case X86::INC64m:
        isINCDEC = true; // fallthrough
      case X86::ADD8mr: case X86::ADD16mr: case X86::ADD32mr: case X86::ADD64mr:
      case X86::ADD8mi: case X86::ADD16mi: case X86::ADD32mi:
      case X86::ADD16mi8: case X86::ADD32mi8:
      case X86::ADD64mi8: case X86::ADD64mi32:
        AtomicOpc = AtomicRMWInst::Add; Opc = Instruction::Add; break;
      case X86::DEC8m: case X86::DEC16m: case X86::DEC32m: case X86::DEC64m:
        isINCDEC = true; // fallthrough
      case X86::SUB8mr: case X86::SUB16mr: case X86::SUB32mr: case X86::SUB64mr:
      case X86::SUB8mi: case X86::SUB16mi: case X86::SUB32mi:
      case X86::SUB16mi8: case X86::SUB32mi8:
      case X86::SUB64mi8: case X86::SUB64mi32:
        AtomicOpc = AtomicRMWInst::Sub; Opc = Instruction::Sub; break;
      case X86::OR8mr: case X86::OR16mr: case X86::OR32mr: case X86::OR64mr:
      case X86::OR8mi: case X86::OR16mi: case X86::OR32mi:
      case X86::OR16mi8: case X86::OR32mi8:
      case X86::OR64mi8: case X86::OR64mi32:
        AtomicOpc = AtomicRMWInst::Or; Opc = Instruction::Or; break;
      case X86::XOR8mr: case X86::XOR16mr: case X86::XOR32mr: case X86::XOR64mr:
      case X86::XOR8mi: case X86::XOR16mi: case X86::XOR32mi:
      case X86::XOR16mi8: case X86::XOR32mi8:
      case X86::XOR64mi8: case X86::XOR64mi32:
        AtomicOpc = AtomicRMWInst::Xor; Opc = Instruction::Xor; break;
      case X86::AND8mr: case X86::AND16mr: case X86::AND32mr: case X86::AND64mr:
      case X86::AND8mi: case X86::AND16mi: case X86::AND32mi:
      case X86::AND16mi8: case X86::AND32mi8:
      case X86::AND64mi8: case X86::AND64mi32:
        AtomicOpc = AtomicRMWInst::And; Opc = Instruction::And; break;
      }

      Value *PointerOperand = nullptr, *Operand2 = nullptr, *Result = nullptr;

      // Either to a manual translation for XADD, or reuse the opcodes for
      // normal prefixed instructions.
      if (XADDMemOpType != X86::OpTypes::OPERAND_TYPE_INVALID) {
        AtomicOpc = AtomicRMWInst::Add;
        Opc = Instruction::Add;
        translateCustomOperand(XADDMemOpType, 0);
        PointerOperand = Vals.back();
        Operand2 = getReg(getRegOp(5));
      } else {
        if (AtomicOpc == AtomicRMWInst::BAD_BINOP)
          llvm_unreachable("Unknown LOCK-prefixed instruction");

        // First, translate the memory operand.
        unsigned NextOpc = Next();
        assert(NextOpc == DCINS::CUSTOM_OP &&
               "Expected X86 memory operand for LOCK-prefixed instruction");
        translateOpcode(NextOpc);
        PointerOperand = Vals.back();

        // Then, ignore the LOAD from that operand
        NextOpc = Next();
        assert(NextOpc == ISD::LOAD &&
               "Expected to load operand for X86 LOCK-prefixed instruction");
        /*VT=*/Next(); /*PointerOp=*/Next();

        // Finally, translate the second operand, if there is one.
        if (isINCDEC) {
          Operand2 = ConstantInt::get(
              PointerOperand->getType()->getPointerElementType(), 1);
        } else {
          NextOpc = Next();
          translateOpcode(NextOpc);
          Operand2 = Vals.back();
        }
      }

      // Translate LOCK-prefix into monotonic ordering.
      Value *Old = Builder->CreateAtomicRMW(AtomicOpc, PointerOperand, Operand2,
                                            AtomicOrdering::Monotonic);

      // If this was a XADD instruction, set the register to the old value.
      if (XADDMemOpType != X86::OpTypes::OPERAND_TYPE_INVALID)
        setReg(getRegOp(5), Old);

      // Finally, update EFLAGS.
      // FIXME: add support to X86DRS::updateEFLAGS for atomicrmw.
      Result = Builder->CreateBinOp(Opc, Old, Operand2);
      X86DRS.updateEFLAGS(Result, /*DontUpdateCF=*/isINCDEC);
      return true;
    } else if (Prefix == X86::REP_PREFIX) {
      unsigned SizeInBits = 0;
      switch (Opcode) {
      default:
        llvm_unreachable("Unknown rep-prefixed instruction");
      case X86::MOVSQ: SizeInBits = 64; break;
      case X86::MOVSL: SizeInBits = 32; break;
      case X86::MOVSW: SizeInBits = 16; break;
      case X86::MOVSB: SizeInBits = 8;  break;
      }
      Type *MemTy = Type::getIntNPtrTy(Builder->getContext(), SizeInBits);
      Value *Dst = Builder->CreateIntToPtr(getReg(X86::RDI), MemTy);
      Value *Src = Builder->CreateIntToPtr(getReg(X86::RSI), MemTy);
      Value *Len = getReg(X86::RCX);
      // FIXME: Add support for reverse copying, depending on Direction Flag.
      // We don't support CLD/STD yet anyway, so this isn't a big deal for now.
      Type *MemcpyArgTys[] = { Dst->getType(), Src->getType(), Len->getType() };
      Builder->CreateCall5(
          Intrinsic::getDeclaration(TheModule, Intrinsic::memcpy, MemcpyArgTys),
          Dst, Src, Len,
          /*Align=*/Builder->getInt32(1),
          /*isVolatile=*/Builder->getInt1(false));
      Builder->GetInsertBlock()->getParent()->dump();

      return true;
    }
    llvm_unreachable("Unable to translate prefixed instruction");
    return false;
  }

  switch (Opcode) {
  default:
    break;
  case X86::XCHG8rr:
  case X86::XCHG16rr:
  case X86::XCHG32rr:
  case X86::XCHG64rr: {
    unsigned R1 = getRegOp(0);
    unsigned R2 = getRegOp(1);
    Value *V1 = getReg(R1);
    Value *V2 = getReg(R2);
    setReg(R2, V1);
    setReg(R1, V2);
    return true;
  }

  case X86::NOOP:
  case X86::NOOPW:
  case X86::NOOPL:
    return true;

  case X86::CPUID: {
    // FIXME: Also generate the function.
    Type *I32Ty = Builder->getInt32Ty();
    Type *ArgTys[] = { I32Ty, I32Ty };
    Type *RetTy = StructType::get(I32Ty, I32Ty, I32Ty, I32Ty, nullptr);
    Function *CPUIDFn = cast<Function>(TheModule->getOrInsertFunction(
        "__llvm_dc_x86_cpuid", FunctionType::get(RetTy, ArgTys, false)));

    Value *Args[] = { getReg(X86::EAX), getReg(X86::ECX) };

    Value *CPUIDCall = Builder->CreateCall(CPUIDFn, Args);
    setReg(X86::EAX, Builder->CreateExtractValue(CPUIDCall, 0));
    setReg(X86::EBX, Builder->CreateExtractValue(CPUIDCall, 1));
    setReg(X86::ECX, Builder->CreateExtractValue(CPUIDCall, 2));
    setReg(X86::EDX, Builder->CreateExtractValue(CPUIDCall, 3));
    return true;
  }
  case X86::XGETBV: {
    // FIXME: Also generate the function.
    Type *I32Ty = Builder->getInt32Ty();
    Type *ArgTys[] = { I32Ty };
    Type *RetTy = StructType::get(I32Ty, I32Ty, nullptr);
    Function *ECRFn = cast<Function>(TheModule->getOrInsertFunction(
        "__llvm_dc_x86_xgetbv", FunctionType::get(RetTy, ArgTys, false)));

    Value *Args[] = { getReg(X86::ECX) };

    Value *ECRCall = Builder->CreateCall(ECRFn, Args);
    setReg(X86::EAX, Builder->CreateExtractValue(ECRCall, 0));
    setReg(X86::EDX, Builder->CreateExtractValue(ECRCall, 1));
    return true;
  }

  case X86::REP_PREFIX:
  case X86::LOCK_PREFIX: {
    LastPrefix = Opcode;
    return true;
  }
  }
  return false;
}

void X86InstrSema::translateTargetOpcode() {
  switch(Opcode) {
  default:
    llvm_unreachable(
        ("Unknown X86 opcode found in semantics: " + utostr(Opcode)).c_str());
  case X86ISD::CMOV: {
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand(),
          *Op3 = getNextOperand(), *Op4 = getNextOperand();
    assert(Op4 == getReg(X86::EFLAGS) &&
           "Conditional mov predicate register isn't EFLAGS!");
    (void)Op4;
    unsigned CC = cast<ConstantInt>(Op3)->getValue().getZExtValue();
    Value *Pred = X86DRS.testCondCode(CC);
    registerResult(Builder->CreateSelect(Pred, Op2, Op1));
    break;
  }
  case X86ISD::RET_FLAG: {
    // FIXME: Handle ret arg.
    /* unsigned Op1 = */ Next();
    setReg(X86::RIP, translatePop(8));
    Builder->CreateBr(ExitBB);
    break;
  }
  case X86ISD::CMP: {
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand();
    registerResult(X86DRS.getEFLAGSforCMP(Op1, Op2));
    break;
  }
  case X86ISD::BRCOND: {
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand(),
          *Op3 = getNextOperand();
    assert(Op3 == getReg(X86::EFLAGS) &&
           "Conditional branch predicate register isn't EFLAGS!");
    (void)Op3;
    uint64_t Target = cast<ConstantInt>(Op1)->getValue().getZExtValue();
    unsigned CC = cast<ConstantInt>(Op2)->getValue().getZExtValue();
    setReg(X86::RIP, Op1);
    Builder->CreateCondBr(X86DRS.testCondCode(CC),
                          getOrCreateBasicBlock(Target),
                          getOrCreateBasicBlock(getBasicBlockEndAddress()));
    break;
  }
  case X86ISD::CALL: {
    Value *Op1 = getNextOperand();
    translatePush(Builder->getInt64(CurrentInst->Address + CurrentInst->Size));
    insertCall(Op1);
    break;
  }
  case X86ISD::SETCC: {
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand();
    assert(Op2 == getReg(X86::EFLAGS) &&
           "SetCC predicate register isn't EFLAGS!");
    (void)Op2;
    unsigned CC = cast<ConstantInt>(Op1)->getValue().getZExtValue();
    Value *Pred = X86DRS.testCondCode(CC);
    registerResult(Builder->CreateZExt(Pred, Builder->getInt8Ty()));
    break;
  }
  case X86ISD::SBB: {
    (void)NextVT();
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand(),
          *Op3 = getNextOperand();
    assert(Op3 == getReg(X86::EFLAGS) &&
           "SBB borrow register isn't EFLAGS!");
    (void)Op3;
    Value *Borrow =
        Builder->CreateZExt(X86DRS.testCondCode(X86::CF), Op1->getType());
    Value *Res = Builder->CreateSub(Op1, Op2);
    registerResult(Builder->CreateSub(Res, Borrow));
    registerResult(getReg(X86::EFLAGS));
    break;
  }
  case X86ISD::BT: {
    Value *Base = getNextOperand();
    Value *Op2 = getNextOperand();
    Value *Offset = Builder->CreateZExtOrBitCast(Op2, Base->getType());
    Value *Bit = Builder->CreateTrunc(Builder->CreateShl(Base, Offset),
                                      Builder->getInt1Ty());

    Value *OldEFLAGS = getReg(X86::EFLAGS);
    Type *EFLAGSTy = OldEFLAGS->getType();
    APInt Mask = APInt::getAllOnesValue(EFLAGSTy->getPrimitiveSizeInBits());
    Mask.clearBit(X86::CF);
    OldEFLAGS = Builder->CreateAnd(OldEFLAGS, ConstantInt::get(*Ctx, Mask));

    Bit = Builder->CreateZExt(Bit, EFLAGSTy);
    Bit = Builder->CreateLShr(Bit, X86::CF);
    registerResult(Builder->CreateOr(OldEFLAGS, Bit));
    break;
  }

  case X86DCISD::DIV8:
    translateDivRem(/* isThreeOperand= */ false, /* isSigned= */ false);
    break;
  case X86DCISD::IDIV8:
    translateDivRem(/* isThreeOperand= */ false, /* isSigned= */ true);
    break;
  case X86DCISD::DIV:
    translateDivRem(/* isThreeOperand= */ true, /* isSigned= */ false);
    break;
  case X86DCISD::IDIV:
    translateDivRem(/* isThreeOperand= */ true, /* isSigned= */ true);
    break;

  case X86ISD::FMIN:
  case X86ISD::FMAX: {
    // FIXME: Ok this is an interesting one. The short version is: we don't
    // care about sNaN, since it's really missing from LLVM.
    // The result defaults to the second operand, so we do a backwards
    // fcmp+select.
    Value *Src1 = getNextOperand();
    Value *Src2 = getNextOperand();
    CmpInst::Predicate Pred;
    if (Opcode == X86ISD::FMAX)
      Pred = CmpInst::FCMP_ULE;
    else
      Pred = CmpInst::FCMP_UGE;
    registerResult(Builder->CreateSelect(Builder->CreateFCmp(Pred, Src1, Src2),
                                         Src2, Src1));
    break;
  }
  case X86ISD::MOVLHPS:
  case X86ISD::MOVLHPD:
  case X86ISD::MOVHLPS:
  case X86ISD::MOVLPS:
  case X86ISD::MOVLPD:
  case X86ISD::MOVSD:
  case X86ISD::MOVSS:
  case X86ISD::UNPCKH:
  case X86ISD::UNPCKL: {
    Value *Src1 = getNextOperand();
    Value *Src2 = getNextOperand();
    Type *VecTy = ResEVT.getTypeForEVT(*Ctx);
    assert(VecTy->isVectorTy() && VecTy == Src1->getType() &&
           VecTy == Src2->getType() &&
           "Operands to MOV/UNPCK shuffle aren't vectors!");
    unsigned NumElt = VecTy->getVectorNumElements();
    SmallVector<Constant *, 16> Mask;
    for (int i = 0, e = NumElt; i != e; ++i)
      Mask.push_back(Builder->getInt32(i));
    switch (Opcode) {
    case X86ISD::MOVLPD: // LPD and SD are equivalent.
    case X86ISD::MOVLPS: // LPS is just SS*2.
    case X86ISD::MOVSD:
    case X86ISD::MOVSS: {
      Mask[0] = Builder->getInt32(NumElt);
      if (Opcode == X86ISD::MOVLPS)
        Mask[1] = Builder->getInt32(NumElt + 1);
      break;
    }
    case X86ISD::MOVLHPS: {
      assert(NumElt == 4);
      Mask[2] = Builder->getInt32(NumElt);
      Mask[3] = Builder->getInt32(NumElt + 1);
      break;
    }
    case X86ISD::MOVHLPS: {
      assert(NumElt == 4);
      Mask[0] = Builder->getInt32(NumElt + 2);
      Mask[1] = Builder->getInt32(NumElt + 3);
      break;
    }
    case X86ISD::MOVLHPD: {
      assert(NumElt == 2);
      Mask[1] = Builder->getInt32(NumElt);
      break;
    }
    case X86ISD::UNPCKH:
    case X86ISD::UNPCKL: {
      int offset = (Opcode == X86ISD::UNPCKH ? NumElt / 2 : 0);
      for (int i = 0, e = NumElt / 2; i != e; ++i) {
        Mask[i] = Builder->getInt32(offset + i);
        Mask[i + 1] = Builder->getInt32(offset + i + NumElt);
      }
      break;
    }
    }
    registerResult(
        Builder->CreateShuffleVector(Src1, Src2, ConstantVector::get(Mask)));
    break;
  }
  case X86ISD::PSHUFD: {
    Value *Src = getNextOperand();
    uint64_t Order =
        cast<ConstantInt>(getNextOperand())->getValue().getZExtValue();
    Type *VecTy = ResEVT.getTypeForEVT(*Ctx);
    assert(VecTy->isVectorTy() && VecTy == Src->getType());
    unsigned NumElt = VecTy->getVectorNumElements();
    SmallVector<Constant *, 8> Mask;
    unsigned i;
    for (i = 0; i != 4; ++i)
      Mask.push_back(Builder->getInt32((Order >> (i * 2)) & 3));
    // If Src is bigger than v4, this is a VPSHUFD, so clear the upper bits.
    for (; i != NumElt; ++i)
      Mask.push_back(Builder->getInt32(NumElt + i));
    registerResult(Builder->CreateShuffleVector(Src,
                                                Constant::getNullValue(VecTy),
                                                ConstantVector::get(Mask)));
    break;
  }
  case X86ISD::HSUB:  translateHorizontalBinop(Instruction::Sub);  break;
  case X86ISD::HADD:  translateHorizontalBinop(Instruction::Add);  break;
  case X86ISD::FHSUB: translateHorizontalBinop(Instruction::FSub); break;
  case X86ISD::FHADD: translateHorizontalBinop(Instruction::FAdd); break;
  }
}

void X86InstrSema::translateCustomOperand(unsigned OperandType,
                                          unsigned MIOpNo) {
  switch(OperandType) {
  default:
    llvm_unreachable(("Unknown X86 operand type found in semantics: " +
                     utostr(OperandType)).c_str());

  case X86::OpTypes::i8mem : translateAddr(MIOpNo, MVT::i8); break;
  case X86::OpTypes::i16mem: translateAddr(MIOpNo, MVT::i16); break;
  case X86::OpTypes::i32mem: translateAddr(MIOpNo, MVT::i32); break;
  case X86::OpTypes::i64mem: translateAddr(MIOpNo, MVT::i64); break;
  case X86::OpTypes::f32mem: translateAddr(MIOpNo, MVT::f32); break;
  case X86::OpTypes::f64mem: translateAddr(MIOpNo, MVT::f64); break;
  case X86::OpTypes::f80mem: translateAddr(MIOpNo, MVT::f80); break;

  // Just fallback to an integer for the rest, let the user decide the type.
  case X86::OpTypes::i128mem :
  case X86::OpTypes::i256mem :
  case X86::OpTypes::f128mem :
  case X86::OpTypes::f256mem :
  case X86::OpTypes::lea64mem: translateAddr(MIOpNo); break;

  case X86::OpTypes::lea64_32mem: {
    translateAddr(MIOpNo);
    Value *&Ptr = Vals.back();
    Ptr = Builder->CreateTruncOrBitCast(Ptr, Builder->getInt32Ty());
    break;
  }

  case X86::OpTypes::i64i32imm_pcrel:
  case X86::OpTypes::brtarget:
  case X86::OpTypes::brtarget8: {
    // FIXME: use MCInstrAnalysis for this kind of thing?
    uint64_t Target = getImmOp(MIOpNo) +
      CurrentInst->Address + CurrentInst->Size;
    registerResult(Builder->getInt64(Target));
    break;
  }
  }
}

void X86InstrSema::translateImplicit(unsigned RegNo) {
  assert(RegNo == X86::EFLAGS);
  // FIXME: We need to understand instructions that define multiple values.
  Value *Def = 0;

  // Look for the last definition
  for (int i = Vals.size() - 1; i >= 0; --i) {
    if (Vals[i] != 0) {
      Def = Vals[i];
      break;
    }
  }
  assert(Def && "Nothing was defined in an instruction with implicit EFLAGS?");
  X86DRS.updateEFLAGS(Def);
}

void X86InstrSema::translateAddr(unsigned MIOperandNo,
                                 MVT::SimpleValueType VT) {
  // FIXME: We should switch to TargetRegisterInfo/InstrInfo instead of MC,
  // first because of all things 64 bit mode (ESP/RSP, size of iPTR, ..).
  // We already depend on codegen in lots of places, maybe completely
  // switching to using MachineFunctions and TargetMachine makes sense?

  Value *Base = getReg(getRegOp(MIOperandNo));
  ConstantInt *Scale = Builder->getInt64(getImmOp(MIOperandNo + 1));
  Value *Index = getReg(getRegOp(MIOperandNo + 2));
  ConstantInt *Offset = Builder->getInt64(getImmOp(MIOperandNo + 3));

  Value *Res = 0;
  if (!Offset->isZero())
    Res = Offset;
  if (Index) {
    if (!Scale->isOne())
      Index = Builder->CreateMul(Index, Scale);
    Res = (Res ? Builder->CreateAdd(Index, Res) : Index);
  }
  if (Base)
    Res = (Res ? Builder->CreateAdd(Base, Res) : Base);

  if (VT != MVT::iPTRAny) {
    Type *PtrTy = EVT(VT).getTypeForEVT(*Ctx)->getPointerTo();
    Res = Builder->CreateIntToPtr(Res, PtrTy);
  }

  registerResult(Res);
}

void X86InstrSema::translatePush(Value *Val) {
  unsigned OpSize = Val->getType()->getPrimitiveSizeInBits() / 8;

  // FIXME: again assumes that we are in 64bit mode.
  Value *OldSP = getReg(X86::RSP);

  Value *OpSizeVal = ConstantInt::get(
      IntegerType::get(*Ctx, OldSP->getType()->getIntegerBitWidth()), OpSize);
  Value *NewSP = Builder->CreateSub(OldSP, OpSizeVal);
  Value *SPPtr = Builder->CreateIntToPtr(NewSP, Val->getType()->getPointerTo());
  Builder->CreateStore(Val, SPPtr);

  setReg(X86::RSP, NewSP);
}

Value *X86InstrSema::translatePop(unsigned OpSize) {
  // FIXME: again assumes that we are in 64bit mode.
  Value *OldSP = getReg(X86::RSP);

  Type *OpTy = IntegerType::get(*Ctx, OpSize * 8);
  Value *OpSizeVal = ConstantInt::get(
      IntegerType::get(*Ctx, OldSP->getType()->getIntegerBitWidth()), OpSize);
  Value *NewSP = Builder->CreateAdd(OldSP, OpSizeVal);
  Value *SPPtr = Builder->CreateIntToPtr(OldSP, OpTy->getPointerTo());
  Value *Val = Builder->CreateLoad(SPPtr);

  setReg(X86::RSP, NewSP);
  return Val;
}

void X86InstrSema::translateHorizontalBinop(Instruction::BinaryOps BinOp) {
  Value *Src1 = getNextOperand(), *Src2 = getNextOperand();
  Type *VecTy = ResEVT.getTypeForEVT(*Ctx);
  assert(VecTy->isVectorTy());
  assert(VecTy == Src1->getType() && VecTy == Src2->getType());
  unsigned NumElt = VecTy->getVectorNumElements();
  Value *Res = UndefValue::get(VecTy);
  Value *Srcs[2] = { Src1, Src2 };
  for (int opi = 0, ope = 2; opi != ope; ++opi) {
    for (int i = 0, e = NumElt / 2; i != e; ++i) {
      Value *EltRes = Builder->CreateBinOp(
          BinOp, Builder->CreateExtractElement(Srcs[opi], Builder->getInt32(i)),
          Builder->CreateExtractElement(Srcs[opi], Builder->getInt32(i + 1)));
      Res = Builder->CreateInsertElement(Res, EltRes,
                                         Builder->getInt32(i + opi * NumElt));
    }
  }
  registerResult(Res);
}

void X86InstrSema::translateDivRem(bool isThreeOperand, bool isSigned) {
  EVT Re2EVT = NextVT();
  assert(Re2EVT == ResEVT && "X86 division result type mismatch!");
  (void)Re2EVT;
  Type *ResType = ResEVT.getTypeForEVT(*Ctx);

  Instruction::CastOps ExtOp;
  Instruction::BinaryOps DivOp, RemOp;
  if (isSigned) {
    ExtOp = Instruction::SExt;
    DivOp = Instruction::SDiv;
    RemOp = Instruction::SRem;
  } else {
    ExtOp = Instruction::ZExt;
    DivOp = Instruction::UDiv;
    RemOp = Instruction::URem;
  }

  Value *Divisor;
  if (isThreeOperand) {
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand();
    IntegerType *HalfType = cast<IntegerType>(ResType);
    unsigned HalfBits = HalfType->getPrimitiveSizeInBits();
    IntegerType *FullType = IntegerType::get(*Ctx, HalfBits * 2);
    Value *DivHi = Builder->CreateCast(ExtOp, Op1, FullType);
    Value *DivLo = Builder->CreateCast(ExtOp, Op2, FullType);
    Divisor = Builder->CreateOr(
        Builder->CreateShl(DivHi, ConstantInt::get(FullType, HalfBits)), DivLo);
  } else {
    Divisor = getNextOperand();
  }

  Value *Dividend = getNextOperand();
  Dividend = Builder->CreateCast(ExtOp, Dividend, Divisor->getType());

  registerResult(Builder->CreateTrunc(
                     Builder->CreateBinOp(DivOp, Divisor, Dividend), ResType));
  registerResult(Builder->CreateTrunc(
                     Builder->CreateBinOp(RemOp, Divisor, Dividend), ResType));
}
