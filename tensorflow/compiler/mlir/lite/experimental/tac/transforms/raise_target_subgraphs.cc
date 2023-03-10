/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "absl/strings/str_cat.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Block.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/experimental/common/outline_operations.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/common/subgraph.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/common/targets.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/common/utils.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/transforms/passes.h"
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"

namespace mlir {
namespace TFL {
namespace tac {
namespace {

using ::mlir::TFL::common::OpsAdded;
using ::mlir::TFL::common::Subgraph;

class RaiseTargetSubgraphsPass
    : public PassWrapper<RaiseTargetSubgraphsPass, OperationPass<ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RaiseTargetSubgraphsPass)

 private:
  llvm::StringRef getArgument() const final {
    return "tfl-raise-target-subgraphs";
  }
  llvm::StringRef getDescription() const final {
    return "This pass will merge those have target-annotated TFL IRs together "
           "& raise them as a function.";
  }
  void runOnOperation() override;

  void RaiseTargetSubgraphsForBlock(Block& block, OpBuilder& builder,
                                    ModuleOp module, bool skip_cpu,
                                    int& func_count);
};

// After raising ops and adding the Func & Call op, call this function
// to set attributes specific to this pass.
void AddAttrs(OpsAdded& ops_added, OpBuilder& builder, int func_count) {
  func::FuncOp& added_func_op = ops_added.func_op;
  func::CallOp& added_call_op = ops_added.call_op;
  StringAttr interface_name =
      builder.getStringAttr(absl::StrCat("func_", func_count));

  added_func_op->setAttr(kInterfaceNameAttr, interface_name);
  added_call_op->setAttr(kInterfaceNameAttr, interface_name);

  StringAttr device = added_func_op->getRegion(0)
                          .getBlocks()
                          .front()
                          .front()
                          .getAttr(kDevice)
                          .cast<StringAttr>();
  StringAttr inference_type = added_func_op->getRegion(0)
                                  .getBlocks()
                                  .front()
                                  .front()
                                  .getAttr(kInferenceType)
                                  .cast<StringAttr>();
  added_call_op->setAttr(kDevice, device);
  added_call_op->setAttr(kInferenceType, inference_type);
  added_func_op->setAttr(kDevice, device);
  added_func_op->setAttr(kInferenceType, inference_type);

  std::string function_name = absl::StrCat(interface_name.getValue().str(), "_",
                                           device.getValue().str(), "_",
                                           inference_type.getValue().str());
  added_func_op.setName(builder.getStringAttr(function_name));
  added_call_op.setCallee(builder.getStringAttr(function_name));
}

// Raises partitioned sequential `Operations` from a block to a new function
// definition. `Operations` are partitioned into classes from the cartesian
// product of possible devices and inference datatypes. For example, we might
// raise a chunk of sequential operations from a block all having attributes
// `{ tac.device = "GPU", tac.inference_type = "FLOAT"}` to a function
// with the matching attributes. Assumed is that device type "CPU"
// is the only device that is allowed to call other devices. I.e. ancestors of a
// "CPU" `Operation` may only `Operations` without a device or other "CPU"
// `Operations`. Implied is that "CPU" ops may contain subgraphs of different
// device types which also need to be raised.
void RaiseTargetSubgraphsPass::RaiseTargetSubgraphsForBlock(Block& block,
                                                            OpBuilder& builder,
                                                            ModuleOp module,
                                                            bool skip_cpu,
                                                            int& func_count) {
  llvm::SetVector<Operation*> partition_ops;

  auto device_is = [&](InferenceDeviceType t, llvm::StringRef device) -> bool {
    return t.hardware == device;
  };

  auto op_has_device = [&](Operation& op, InferenceDeviceType& device) -> bool {
    Optional<InferenceDeviceType> op_device = GetInferenceDeviceTypeForOp(&op);
    if (!op_device.has_value()) return false;
    device = op_device.value();
    return true;
  };

  auto op_device_is = [&](Operation& op, llvm::StringRef device) -> bool {
    InferenceDeviceType device_type;
    if (!op_has_device(op, device_type)) return false;
    return device_is(device_type, device);
  };

  // Given a list of `Operation`s to partitition, raise them to a new
  // function. If the partitons is of type "CPU" then it may contain
  // other deivice subgraphs that need to be raised. We recur on
  // any nested blocks of "CPU" ops and skip raising "CPU" ops for the
  // remainder of that recursive call.
  auto extract = [&](llvm::SetVector<Operation*>& partition_ops) -> void {
    if (partition_ops.empty()) return;
    InferenceDeviceType device =
        GetInferenceDeviceTypeForOp(partition_ops.front()).value();
    Subgraph old_subgraph(partition_ops, ++func_count);
    OpsAdded ops_added;
    ExtractSubgraphToFunc(old_subgraph, builder, module, ops_added);
    AddAttrs(ops_added, builder, func_count);
    // Ops in "CPU" subgraphs may nested regions with other device subgraphs.
    // We recur into these nested blocks to raise those as well. We don't raise
    // "CPU" ops who are themselves nested within a "CPU" op, so set
    // `skip_cpu` to true.
    if (device_is(device, "CPU")) {
      for (auto& block : ops_added.func_op->getRegion(0).getBlocks())
        for (auto& op : block) {
          auto op_device = GetInferenceDeviceTypeForOp(&op);
          if (op_device_is(op, "CPU"))
            // The recently raised func is device type cpu & `op` is a "CPU".
            // Recursivley call again to raise any non-"CPU" subgraphs contained
            // within nested region of `op`.
            for (auto& region : op.getRegions())
              for (auto& block : region.getBlocks())
                RaiseTargetSubgraphsForBlock(block, builder, module,
                                             /*skip_cpu=*/true, func_count);
        }
    }
    partition_ops.clear();
  };

  // Given a block, partition into lists of similar `Operations` as described.
  Optional<InferenceDeviceType> current_device_type = llvm::None;
  for (Operation& current_op : block) {
    auto next_device_type = GetInferenceDeviceTypeForOp(&current_op);
    if (!next_device_type.has_value() ||
        (skip_cpu && device_is(next_device_type.value(), "CPU"))) {
      // If we aren't raising this op, we only need to raise the current
      // partition if this op depends on one the the partitioned ops results.
      for (Value operand : current_op.getOperands()) {
        if (partition_ops.contains(operand.getDefiningOp()))
          extract(partition_ops);
      }
      continue;
    }
    if (next_device_type == current_device_type) {
      partition_ops.insert(&current_op);
      continue;
    }
    extract(partition_ops);
    partition_ops.insert(&current_op);
    current_device_type = next_device_type;
  }
  extract(partition_ops);
}

void RaiseTargetSubgraphsPass::runOnOperation() {
  ModuleOp module = getOperation();
  SmallVector<func::FuncOp> funcs(module.getOps<func::FuncOp>());
  int func_count = -1;
  for (auto func : funcs) {
    for (auto& block : func) {
      OpBuilder builder = OpBuilder::atBlockBegin(&block);
      RaiseTargetSubgraphsForBlock(block, builder, module, /*skip_cpu=*/false,
                                   func_count);
    }
  }
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> CreateRaiseTargetSubgraphsPass() {
  return std::make_unique<RaiseTargetSubgraphsPass>();
}

static PassRegistration<RaiseTargetSubgraphsPass> pass;

}  // namespace tac
}  // namespace TFL
}  // namespace mlir
