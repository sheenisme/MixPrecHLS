//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/LoopUtils.h"
#include "scalehls/Transforms/Passes.h"
#include "scalehls/Transforms/Utils.h"

using namespace mlir;
using namespace scalehls;

/// Apply function pipelining to the input function, all contained loops are
/// automatically fully unrolled.
static bool applyFuncPipelining(FuncOp func, int64_t targetII) {
  if (!applyFullyLoopUnrolling(func.front()))
    return false;

  auto builder = Builder(func);

  // Set pipeling pragma and target II.
  func->setAttr("pipeline", builder.getBoolAttr(true));
  func->setAttr("target_ii", builder.getI64IntegerAttr(targetII));

  // Once a function is pipelined, dataflow pragma will be ignored.
  func->setAttr("dataflow", builder.getBoolAttr(false));

  return true;
}

namespace {
struct FuncPipelining : public FuncPipeliningBase<FuncPipelining> {
  void runOnOperation() override {
    auto func = getOperation();

    if (func.getName() == targetFunc)
      applyFuncPipelining(func, targetII);
  }
};
} // namespace

std::unique_ptr<Pass> scalehls::createFuncPipeliningPass() {
  return std::make_unique<FuncPipelining>();
}