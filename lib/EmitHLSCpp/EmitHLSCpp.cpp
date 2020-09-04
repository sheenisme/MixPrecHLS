//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Translation.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

#include "EmitHLSCpp.h"

using namespace mlir;
using namespace std;

//===----------------------------------------------------------------------===//
// Some Base Classes
//
// These classes should be factored out, and can be inherited by emitters
// targeting various backends (e.g., Xilinx Vivado HLS, Intel FPGAs, etc.).
//===----------------------------------------------------------------------===//

namespace {
/// This class maintains the mutable state that cross-cuts and is shared by the
/// various emitters.
class HLSCppEmitterState {
public:
  explicit HLSCppEmitterState(raw_ostream &os) : os(os) {}

  // The stream to emit to.
  raw_ostream &os;

  bool encounteredError = false;
  unsigned currentIndent = 0;

  // This table contains all declared values.
  DenseMap<Value, SmallString<8>> nameTable;

private:
  HLSCppEmitterState(const HLSCppEmitterState &) = delete;
  void operator=(const HLSCppEmitterState &) = delete;
};
} // namespace

namespace {
/// This is the base class for all of the HLSCpp Emitter components.
class HLSCppEmitterBase {
public:
  explicit HLSCppEmitterBase(HLSCppEmitterState &state)
      : state(state), os(state.os) {}

  InFlightDiagnostic emitError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitError(message);
  }

  raw_ostream &indent() { return os.indent(state.currentIndent); }

  void addIndent() { state.currentIndent += 2; }
  void reduceIndent() { state.currentIndent -= 2; }

  // All of the mutable state we are maintaining.
  HLSCppEmitterState &state;

  // The stream to emit to.
  raw_ostream &os;

  /// Value name management methods.
  SmallString<8> addName(Value val, bool isPtr);
  SmallString<8> getName(Value val);

private:
  HLSCppEmitterBase(const HLSCppEmitterBase &) = delete;
  void operator=(const HLSCppEmitterBase &) = delete;
};
} // namespace

SmallString<8> HLSCppEmitterBase::addName(Value val, bool isPtr = false) {
  assert(state.nameTable[val].empty() && "duplicate value declaration");

  // Temporary naming rule.
  SmallString<8> newName;
  if (isPtr)
    newName += "*";
  newName += StringRef("val" + to_string(state.nameTable.size()));

  state.nameTable[val] = newName;
  return newName;
}

SmallString<8> HLSCppEmitterBase::getName(Value val) {
  // For constant operations, the constant number will be returned rather than
  // the value name.
  if (val.getKind() != Value::Kind::BlockArgument) {
    if (auto constOp = dyn_cast<ConstantOp>(val.getDefiningOp())) {
      if (auto floatAttr = constOp.getValue().dyn_cast<FloatAttr>())
        return StringRef(to_string(floatAttr.getValueAsDouble()));
      else if (auto intAttr = constOp.getValue().dyn_cast<IntegerAttr>())
        return StringRef(to_string(intAttr.getInt()));
      else
        emitError(constOp, "has unsupported constant type.");
    }
  }

  return state.nameTable[val];
}

namespace {
/// This class is a visitor for SSACFG operation nodes.
template <typename ConcreteType, typename ResultType, typename... ExtraArgs>
class HLSCppVisitorBase {
public:
  ResultType dispatchVisitor(Operation *op, ExtraArgs... args) {
    auto *thisCast = static_cast<ConcreteType *>(this);
    return TypeSwitch<Operation *, ResultType>(op)
        .template Case<
            // Affine statements.
            AffineForOp, AffineIfOp, AffineParallelOp, AffineApplyOp,
            AffineMaxOp, AffineMinOp, AffineLoadOp, AffineStoreOp,
            AffineYieldOp,
            // Memref-related statements.
            AllocOp, LoadOp, StoreOp,
            // Unary expressions.
            AbsFOp, CeilFOp, NegFOp, CosOp, SinOp, TanhOp, SqrtOp, RsqrtOp,
            ExpOp, Exp2Op, LogOp, Log2Op, Log10Op,
            // Float binary expressions.
            CmpFOp, AddFOp, SubFOp, MulFOp, DivFOp, RemFOp,
            // Integer binary expressions.
            CmpIOp, AddIOp, SubIOp, MulIOp, SignedDivIOp, SignedRemIOp,
            UnsignedDivIOp, UnsignedRemIOp, XOrOp, AndOp, OrOp, ShiftLeftOp,
            SignedShiftRightOp, UnsignedShiftRightOp,
            // Special operations.
            ConstantOp, ReturnOp>([&](auto opNode) -> ResultType {
          return thisCast->visitOp(opNode, args...);
        })
        .Default([&](auto opNode) -> ResultType {
          return thisCast->visitInvalidOp(op, args...);
        });
  }

  /// This callback is invoked on any invalid operations.
  ResultType visitInvalidOp(Operation *op, ExtraArgs... args) {
    op->emitOpError("is unsupported operation.");
    abort();
  }

  /// This callback is invoked on any operations that are not handled by the
  /// concrete visitor.
  ResultType visitUnhandledOp(Operation *op, ExtraArgs... args) {
    return ResultType();
  }

#define HANDLE(OPTYPE)                                                         \
  ResultType visitOp(OPTYPE op, ExtraArgs... args) {                           \
    return static_cast<ConcreteType *>(this)->visitUnhandledOp(op, args...);   \
  }

  // Affine statements.
  HANDLE(AffineForOp);
  HANDLE(AffineIfOp);
  HANDLE(AffineParallelOp);
  HANDLE(AffineApplyOp);
  HANDLE(AffineMaxOp);
  HANDLE(AffineMinOp);
  HANDLE(AffineLoadOp);
  HANDLE(AffineStoreOp);
  HANDLE(AffineYieldOp);

  // Memref-related statements.
  HANDLE(AllocOp);
  HANDLE(LoadOp);
  HANDLE(StoreOp);

  // Unary expressions.
  HANDLE(AbsFOp);
  HANDLE(CeilFOp);
  HANDLE(NegFOp);
  HANDLE(CosOp);
  HANDLE(SinOp);
  HANDLE(TanhOp);
  HANDLE(SqrtOp);
  HANDLE(RsqrtOp);
  HANDLE(ExpOp);
  HANDLE(Exp2Op);
  HANDLE(LogOp);
  HANDLE(Log2Op);
  HANDLE(Log10Op);

  // Float binary expressions.
  HANDLE(CmpFOp);
  HANDLE(AddFOp);
  HANDLE(SubFOp);
  HANDLE(MulFOp);
  HANDLE(DivFOp);
  HANDLE(RemFOp);

  // Integer binary expressions.
  HANDLE(CmpIOp);
  HANDLE(AddIOp);
  HANDLE(SubIOp);
  HANDLE(MulIOp);
  HANDLE(SignedDivIOp);
  HANDLE(SignedRemIOp);
  HANDLE(UnsignedDivIOp);
  HANDLE(UnsignedRemIOp);
  HANDLE(XOrOp);
  HANDLE(AndOp);
  HANDLE(OrOp);
  HANDLE(ShiftLeftOp);
  HANDLE(SignedShiftRightOp);
  HANDLE(UnsignedShiftRightOp);

  // Special operations.
  HANDLE(ConstantOp);
  HANDLE(ReturnOp);
#undef HANDLE
};
} // namespace

//===----------------------------------------------------------------------===//
// Utils
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// ModuleEmitter Class Definition
//===----------------------------------------------------------------------===//

namespace {
class ModuleEmitter : public HLSCppEmitterBase {
public:
  using operand_range = Operation::operand_range;
  explicit ModuleEmitter(HLSCppEmitterState &state)
      : HLSCppEmitterBase(state) {}

  /// Affine statement emitters.
  void emitAffineFor(AffineForOp *op);
  void emitAffineIf(AffineIfOp *op);
  void emitAffineParallel(AffineParallelOp *op);
  void emitAffineApply(AffineApplyOp *op);
  void emitAffineMax(AffineMaxOp *op);
  void emitAffineMin(AffineMinOp *op);
  void emitAffineLoad(AffineLoadOp *op);
  void emitAffineStore(AffineStoreOp *op);
  void emitAffineYield(AffineYieldOp *op);

  /// Memref-related statement emitters.
  void emitAlloc(AllocOp *op);
  void emitLoad(LoadOp *op);
  void emitStore(StoreOp *op);

  /// Standard expression emitters.
  void emitBinary(Operation *op, const char *syntax);
  void emitUnary(Operation *op, const char *syntax);

  /// Top-level MLIR module emitter.
  void emitModule(ModuleOp module);

private:
  /// MLIR component emitters.
  void emitValue(Value val, bool isPtr = false);
  void emitOperation(Operation *op);
  void emitBlock(Block &block);
  void emitFunction(FuncOp func);
};
} // namespace

//===----------------------------------------------------------------------===//
// AffineEmitter Class
//===----------------------------------------------------------------------===//

namespace {
class AffineExprEmitter : public HLSCppEmitterBase,
                          public AffineExprVisitor<AffineExprEmitter> {
public:
  using operand_range = Operation::operand_range;
  explicit AffineExprEmitter(HLSCppEmitterState &state, unsigned numDim,
                             operand_range operands)
      : HLSCppEmitterBase(state), numDim(numDim), operands(operands) {}

  void visitAddExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "+"); }
  void visitMulExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "*"); }
  void visitModExpr(AffineBinaryOpExpr expr) { emitAffineBinary(expr, "%"); }
  void visitFloorDivExpr(AffineBinaryOpExpr expr) {
    emitAffineBinary(expr, "/");
  }
  void visitCeilDivExpr(AffineBinaryOpExpr expr) {
    // This is super inefficient.
    os << "(";
    visit(expr.getLHS());
    os << " + ";
    visit(expr.getRHS());
    os << " - 1) / ";
    visit(expr.getRHS());
    os << ")";
  }

  void visitConstantExpr(AffineConstantExpr expr) {
    auto exprValue = expr.getValue();
    if (exprValue < 0)
      os << "(" << exprValue << ")";
    else
      os << exprValue;
  }

  void visitDimExpr(AffineDimExpr expr) {
    os << getName(operands[expr.getPosition()]);
  }
  void visitSymbolExpr(AffineSymbolExpr expr) {
    os << getName(operands[numDim + expr.getPosition()]);
  }

  /// Affine expression emitters.
  void emitAffineBinary(AffineBinaryOpExpr expr, const char *syntax) {
    os << "(";
    visit(expr.getLHS());
    os << " " << syntax << " ";
    visit(expr.getRHS());
    os << ")";
  }

  void emitAffineExpr(AffineExpr expr) { visit(expr); }

private:
  unsigned numDim;
  operand_range operands;
};
} // namespace

//===----------------------------------------------------------------------===//
// StmtVisitor Class
//===----------------------------------------------------------------------===//

namespace {
class StmtVisitor : public HLSCppVisitorBase<StmtVisitor, bool> {
public:
  StmtVisitor(ModuleEmitter &emitter) : emitter(emitter) {}

  using HLSCppVisitorBase::visitOp;
  /// Affine statements (with region).
  bool visitOp(AffineForOp op) { return emitter.emitAffineFor(&op), true; }
  bool visitOp(AffineIfOp op) { return emitter.emitAffineIf(&op), true; }
  bool visitOp(AffineParallelOp op) {
    return emitter.emitAffineParallel(&op), true;
  }
  bool visitOp(AffineApplyOp op) { return emitter.emitAffineApply(&op), true; }
  bool visitOp(AffineMaxOp op) { return emitter.emitAffineMax(&op), true; }
  bool visitOp(AffineMinOp op) { return emitter.emitAffineMin(&op), true; }
  bool visitOp(AffineLoadOp op) { return emitter.emitAffineLoad(&op), true; }
  bool visitOp(AffineStoreOp op) { return emitter.emitAffineStore(&op), true; }
  bool visitOp(AffineYieldOp op) { return emitter.emitAffineYield(&op), true; }

  /// Memref related statements.
  bool visitOp(AllocOp op) { return emitter.emitAlloc(&op), true; }
  bool visitOp(LoadOp op) { return emitter.emitLoad(&op), true; }
  bool visitOp(StoreOp op) { return emitter.emitStore(&op), true; }

  /// Special operations.
  bool visitOp(ConstantOp op) { return true; }
  bool visitOp(ReturnOp op) { return true; }

private:
  ModuleEmitter &emitter;
};
} // namespace

//===----------------------------------------------------------------------===//
// ExprVisitor Class
//===----------------------------------------------------------------------===//

namespace {
class ExprVisitor : public HLSCppVisitorBase<ExprVisitor, bool> {
public:
  ExprVisitor(ModuleEmitter &emitter) : emitter(emitter) {}

  using HLSCppVisitorBase::visitOp;
  /// Float binary expressions.
  bool visitOp(CmpFOp op);
  bool visitOp(AddFOp op) { return emitter.emitBinary(op, "+"), true; }
  bool visitOp(SubFOp op) { return emitter.emitBinary(op, "-"), true; }
  bool visitOp(MulFOp op) { return emitter.emitBinary(op, "*"), true; }
  bool visitOp(DivFOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(RemFOp op) { return emitter.emitBinary(op, "%"), true; }

  /// Integer binary expressions.
  bool visitOp(CmpIOp op);
  bool visitOp(AddIOp op) { return emitter.emitBinary(op, "+"), true; }
  bool visitOp(SubIOp op) { return emitter.emitBinary(op, "-"), true; }
  bool visitOp(MulIOp op) { return emitter.emitBinary(op, "*"), true; }
  bool visitOp(SignedDivIOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(SignedRemIOp op) { return emitter.emitBinary(op, "/"), true; }
  bool visitOp(UnsignedDivIOp op) { return emitter.emitBinary(op, "%"), true; }
  bool visitOp(UnsignedRemIOp op) { return emitter.emitBinary(op, "%"), true; }
  bool visitOp(XOrOp op) { return emitter.emitBinary(op, "^"), true; }
  bool visitOp(AndOp op) { return emitter.emitBinary(op, "&"), true; }
  bool visitOp(OrOp op) { return emitter.emitBinary(op, "|"), true; }
  bool visitOp(ShiftLeftOp op) { return emitter.emitBinary(op, "<<"), true; }
  bool visitOp(SignedShiftRightOp op) {
    return emitter.emitBinary(op, ">>"), true;
  }
  bool visitOp(UnsignedShiftRightOp op) {
    return emitter.emitBinary(op, ">>"), true;
  }

  /// Unary expressions.
  bool visitOp(AbsFOp op) { return emitter.emitUnary(op, "abs"), true; }
  bool visitOp(CeilFOp op) { return emitter.emitUnary(op, "ceil"), true; }
  bool visitOp(NegFOp op) { return emitter.emitUnary(op, "-"), true; }
  bool visitOp(CosOp op) { return emitter.emitUnary(op, "cos"), true; }
  bool visitOp(SinOp op) { return emitter.emitUnary(op, "sin"), true; }
  bool visitOp(TanhOp op) { return emitter.emitUnary(op, "tanh"), true; }
  bool visitOp(SqrtOp op) { return emitter.emitUnary(op, "sqrt"), true; }
  bool visitOp(RsqrtOp op) { return emitter.emitUnary(op, "1.0 / sqrt"), true; }
  bool visitOp(ExpOp op) { return emitter.emitUnary(op, "exp"), true; }
  bool visitOp(Exp2Op op) { return emitter.emitUnary(op, "exp2"), true; }
  bool visitOp(LogOp op) { return emitter.emitUnary(op, "log"), true; }
  bool visitOp(Log2Op op) { return emitter.emitUnary(op, "log2"), true; }
  bool visitOp(Log10Op op) { return emitter.emitUnary(op, "log10"), true; }

private:
  ModuleEmitter &emitter;
};
} // namespace

bool ExprVisitor::visitOp(CmpFOp op) {
  switch (op.getPredicate()) {
  case CmpFPredicate::OEQ:
  case CmpFPredicate::UEQ:
    return emitter.emitBinary(op, "=="), true;
  case CmpFPredicate::ONE:
  case CmpFPredicate::UNE:
    return emitter.emitBinary(op, "!="), true;
  case CmpFPredicate::OLT:
  case CmpFPredicate::ULT:
    return emitter.emitBinary(op, "<"), true;
  case CmpFPredicate::OLE:
  case CmpFPredicate::ULE:
    return emitter.emitBinary(op, "<="), true;
  case CmpFPredicate::OGT:
  case CmpFPredicate::UGT:
    return emitter.emitBinary(op, ">"), true;
  case CmpFPredicate::OGE:
  case CmpFPredicate::UGE:
    return emitter.emitBinary(op, ">="), true;
  default:
    return true;
  }
}

bool ExprVisitor::visitOp(CmpIOp op) {
  switch (op.getPredicate()) {
  case CmpIPredicate::eq:
    return emitter.emitBinary(op, "=="), true;
  case CmpIPredicate::ne:
    return emitter.emitBinary(op, "!="), true;
  case CmpIPredicate::slt:
  case CmpIPredicate::ult:
    return emitter.emitBinary(op, "<"), true;
  case CmpIPredicate::sle:
  case CmpIPredicate::ule:
    return emitter.emitBinary(op, "<="), true;
  case CmpIPredicate::sgt:
  case CmpIPredicate::ugt:
    return emitter.emitBinary(op, ">"), true;
  case CmpIPredicate::sge:
  case CmpIPredicate::uge:
    return emitter.emitBinary(op, ">="), true;
  }
}

//===----------------------------------------------------------------------===//
// ModuleEmitter Class Implementation
//===----------------------------------------------------------------------===//

/// Affine statement emitters.
void ModuleEmitter::emitAffineFor(AffineForOp *op) {
  indent();
  os << "for (";
  auto iterVar = op->getInductionVar();

  // Emit lower bound.
  emitValue(iterVar);
  os << " = ";
  auto lowerMap = op->getLowerBoundMap();
  AffineExprEmitter lowerEmitter(state, lowerMap.getNumDims(),
                                 op->getLowerBoundOperands());
  if (lowerMap.getNumResults() == 1)
    lowerEmitter.emitAffineExpr(lowerMap.getResult(0));
  else {
    for (unsigned i = 0, e = lowerMap.getNumResults() - 1; i < e; ++i) {
      os << "max(";
    }
    lowerEmitter.emitAffineExpr(lowerMap.getResult(0));
    for (auto &expr : llvm::drop_begin(lowerMap.getResults(), 1)) {
      os << ", ";
      lowerEmitter.emitAffineExpr(expr);
      os << ")";
    }
  }
  os << "; ";

  // Emit upper bound.
  emitValue(iterVar);
  os << " < ";
  auto upperMap = op->getUpperBoundMap();
  AffineExprEmitter upperEmitter(state, upperMap.getNumDims(),
                                 op->getUpperBoundOperands());
  if (upperMap.getNumResults() == 1)
    upperEmitter.emitAffineExpr(upperMap.getResult(0));
  else {
    for (unsigned i = 0, e = upperMap.getNumResults() - 1; i < e; ++i) {
      os << "min(";
    }
    upperEmitter.emitAffineExpr(upperMap.getResult(0));
    for (auto &expr : llvm::drop_begin(upperMap.getResults(), 1)) {
      os << ", ";
      upperEmitter.emitAffineExpr(expr);
      os << ")";
    }
  }
  os << "; ";

  // Emit increase step.
  emitValue(iterVar);
  os << " += " << op->getStep() << ") {\n";

  emitBlock(op->getRegion().front());
  indent();
  os << "}\n";
}

void ModuleEmitter::emitAffineIf(AffineIfOp *op) {
  // Declare all values returned by AffineYieldOp. They will be further handled
  // by the AffineYieldOp emitter.
  for (auto result : op->getResults()) {
    indent();
    emitValue(result);
    os << ";\n";
  }

  indent();
  os << "if (";
  auto constrSet = op->getIntegerSet();
  AffineExprEmitter constrEmitter(state, constrSet.getNumDims(),
                                  op->getOperands());

  // Emit all constraints.
  unsigned constrIdx = 0;
  for (auto &expr : constrSet.getConstraints()) {
    constrEmitter.emitAffineExpr(expr);
    if (constrSet.isEq(constrIdx))
      os << " == 0";
    else
      os << " >= 0";

    if (constrIdx != constrSet.getNumConstraints() - 1)
      os << " && ";

    constrIdx += 1;
  }
  os << ") {\n";
  emitBlock(*op->getThenBlock());

  if (op->hasElse()) {
    indent();
    os << "} else {\n";
    emitBlock(*op->getElseBlock());
  }

  indent();
  os << "}\n";
}

void ModuleEmitter::emitAffineParallel(AffineParallelOp *op) { return; }

void ModuleEmitter::emitAffineApply(AffineApplyOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  auto affineMap = op->getAffineMap();
  AffineExprEmitter(state, affineMap.getNumDims(), op->getOperands())
      .emitAffineExpr(affineMap.getResult(0));
  os << ";\n";
}

void ModuleEmitter::emitAffineMax(AffineMaxOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getOperands());
  for (unsigned i = 0, e = affineMap.getNumResults() - 1; i < e; ++i) {
    os << "max(";
  }
  affineEmitter.emitAffineExpr(affineMap.getResult(0));
  for (auto &expr : llvm::drop_begin(affineMap.getResults(), 1)) {
    os << ", ";
    affineEmitter.emitAffineExpr(expr);
    os << ")";
  }
  os << ";\n";
}

void ModuleEmitter::emitAffineMin(AffineMinOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getOperands());
  for (unsigned i = 0, e = affineMap.getNumResults() - 1; i < e; ++i) {
    os << "min(";
  }
  affineEmitter.emitAffineExpr(affineMap.getResult(0));
  for (auto &expr : llvm::drop_begin(affineMap.getResults(), 1)) {
    os << ", ";
    affineEmitter.emitAffineExpr(expr);
    os << ")";
  }
  os << ";\n";
}

void ModuleEmitter::emitAffineLoad(mlir::AffineLoadOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  emitValue(op->getMemRef());
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getMapOperands());
  for (auto indice : affineMap.getResults()) {
    os << "[";
    affineEmitter.emitAffineExpr(indice);
    os << "];\n";
  }
}

void ModuleEmitter::emitAffineStore(AffineStoreOp *op) {
  indent();
  emitValue(op->getMemRef());
  auto affineMap = op->getAffineMap();
  AffineExprEmitter affineEmitter(state, affineMap.getNumDims(),
                                  op->getMapOperands());
  for (auto indice : affineMap.getResults()) {
    os << "[";
    affineEmitter.emitAffineExpr(indice);
    os << "]";
  }
  os << " = ";
  emitValue(op->getValueToStore());
  os << ";\n";
}

void ModuleEmitter::emitAffineYield(AffineYieldOp *op) {
  // For now, only AffineIfOp may yield values.
  if (auto affineIf = dyn_cast<AffineIfOp>(op->getParentOp())) {
    unsigned resultIdx = 0;
    for (auto result : affineIf.getResults()) {
      indent();
      emitValue(result);
      os << " = ";
      emitValue(op->getOperand(resultIdx));
      os << ";\n";
      resultIdx += 1;
    }
  }
}

/// Memref-related statement emitters.
void ModuleEmitter::emitAlloc(AllocOp *op) {
  indent();
  emitValue(op->getResult());
  for (auto &shape : op->getType().getShape())
    os << "[" << shape << "];\n";
}

void ModuleEmitter::emitLoad(LoadOp *op) {
  indent();
  emitValue(op->getResult());
  os << " = ";
  emitValue(op->getMemRef());
  for (auto indice : op->getIndices()) {
    os << "[";
    emitValue(indice);
    os << "];\n";
  }
}

void ModuleEmitter::emitStore(StoreOp *op) {
  indent();
  emitValue(op->getMemRef());
  for (auto indice : op->getIndices()) {
    os << "[";
    emitValue(indice);
    os << "]";
  }
  os << " = ";
  emitValue(op->getValueToStore());
  os << ";\n";
}

/// Standard expression emitters.
void ModuleEmitter::emitBinary(Operation *op, const char *syntax) {
  indent();
  emitValue(op->getResult(0));
  os << " = ";
  emitValue(op->getOperand(0));
  os << " " << syntax << " ";
  emitValue(op->getOperand(1));
  os << ";\n";
}

void ModuleEmitter::emitUnary(Operation *op, const char *syntax) {
  indent();
  emitValue(op->getResult(0));
  os << " = " << syntax << "(";
  emitValue(op->getOperand(0));
  os << ");\n";
}

/// MLIR component emitters.
void ModuleEmitter::emitValue(Value val, bool isPtr) {
  // Value has been declared before or is a constant number.
  auto valName = getName(val);
  if (!valName.empty()) {
    os << valName;
    return;
  }

  // Handle memref type.
  auto valType = val.getType();
  if (auto memType = valType.dyn_cast<MemRefType>())
    valType = memType.getElementType();

  // Emit value type for declaring a new value.
  switch (valType.getKind()) {
  // Handle float types.
  case StandardTypes::F32:
    os << "float ";
    break;
  case StandardTypes::F64:
    os << "double ";
    break;

  // Handle integer types.
  case StandardTypes::Index:
    os << "int ";
    break;
  case StandardTypes::Integer: {
    auto intType = valType.cast<IntegerType>();
    os << "ap_";
    if (intType.getSignedness() == IntegerType::SignednessSemantics::Unsigned)
      os << "u";
    os << "int<" << intType.getWidth() << "> ";
    break;
  }
  default:
    emitError(val.getDefiningOp(), "has unsupported type.");
    break;
  }

  // Add the new value to nameTable and emit its name.
  os << addName(val, isPtr);
  return;
}

void ModuleEmitter::emitOperation(Operation *op) {
  if (ExprVisitor(*this).dispatchVisitor(op))
    return;

  if (StmtVisitor(*this).dispatchVisitor(op))
    return;

  emitError(op, "can't be correctly emitted.");
}

void ModuleEmitter::emitBlock(Block &block) {
  addIndent();
  for (auto &op : block) {
    emitOperation(&op);
  }
  reduceIndent();
}

void ModuleEmitter::emitFunction(FuncOp func) {
  if (func.getBlocks().size() != 1)
    emitError(func, "has more than one basic blocks.");
  os << "void " << func.getName() << "(\n";

  // Emit function signature.
  addIndent();

  // Handle input arguments.
  unsigned argIdx = 0;
  for (auto &arg : func.getArguments()) {
    indent();
    emitValue(arg);
    if (auto memType = arg.getType().dyn_cast<MemRefType>())
      for (auto &shape : memType.getShape())
        os << "[" << shape << "]";
    if (argIdx == func.getNumArguments() - 1 && func.getNumResults() == 0)
      os << "\n";
    else
      os << ",\n";
    argIdx += 1;
  }

  // Handle results.
  if (auto funcReturn = dyn_cast<ReturnOp>(func.front().getTerminator())) {
    unsigned resultIdx = 0;
    for (auto result : funcReturn.getOperands()) {
      indent();
      emitValue(result, /*isPtr=*/true);
      if (auto memType = result.getType().dyn_cast<MemRefType>())
        for (auto &shape : memType.getShape())
          os << "[" << shape << "]";
      if (resultIdx == func.getNumResults() - 1)
        os << "\n";
      else
        os << ",\n";
      resultIdx += 1;
    }
  } else {
    emitError(func, "doesn't have return operation as terminator.");
  }

  reduceIndent();
  os << ") {\n";

  // Emit function body.
  emitBlock(func.front());
  os << "}\n";
}

/// Top-level MLIR module emitter.
void ModuleEmitter::emitModule(ModuleOp module) {
  os << R"XXX(
//===------------------------------------------------------------*- C++ -*-===//
//
// Automatically generated file for High-level Synthesis (HLS).
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <ap_axi_sdata.h>
#include <ap_fixed.h>
#include <ap_int.h>
#include <hls_math.h>
#include <hls_stream.h>
#include <math.h>
#include <stdint.h>

using namespace std;

)XXX";

  for (auto &op : *module.getBody()) {
    if (auto func = dyn_cast<FuncOp>(op))
      emitFunction(func);
    else if (!isa<ModuleTerminatorOp>(op))
      emitError(&op, "is unsupported operation.");
  }
}

//===----------------------------------------------------------------------===//
// Entry of hlsld-translate
//===----------------------------------------------------------------------===//

static LogicalResult emitHLSCpp(ModuleOp module, llvm::raw_ostream &os) {
  HLSCppEmitterState state(os);
  ModuleEmitter(state).emitModule(module);
  return failure(state.encounteredError);
}

void hlsld::registerHLSCppEmitterTranslation() {
  static TranslateFromMLIRRegistration toHLSCpp("emit-hlscpp", emitHLSCpp);
}
