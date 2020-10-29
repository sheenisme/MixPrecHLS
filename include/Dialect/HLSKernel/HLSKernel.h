//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#ifndef SCALEHLS_DIALECT_HLSKERNEL_HLSKERNEL_H
#define SCALEHLS_DIALECT_HLSKERNEL_HLSKERNEL_H

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Function.h"

#include "Dialect/HLSKernel/HLSKernelDialect.h.inc"

#define GET_OP_CLASSES
#include "Dialect/HLSKernel/HLSKernel.h.inc"

#endif // SCALEHLS_DIALECT_HLSKERNEL_HLSKERNEL_H