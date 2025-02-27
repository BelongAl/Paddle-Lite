// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "lite/api/paddle_place.h"
#include "lite/backends/arm/math/type_trans.h"
#include "lite/core/context.h"
#include "lite/core/target_wrapper.h"
#include "lite/core/type_system.h"
#include "lite/core/types.h"
#include "lite/core/workspace.h"
#include "lite/operators/op_params.h"
#include "lite/utils/all.h"
#include "lite/utils/replace_stl/stream.h"

#ifdef LITE_WITH_PROFILE
#include "lite/core/profile/profiler.h"
#endif  // LITE_WITH_PROFILE

namespace paddle {
namespace lite {

// An base with virtual functions to unify all the kernel implementation on
// different targets.
class KernelBase {
 public:
  // type_infer_handler is used to inference a output type by considering the
  // input types in the type system.
  using type_infer_handler_t = std::function<const Type*(
      const std::map<std::string, const Type*>& input_types,
      const std::string& out_arg)>;

  /// Run some initialization before `Run`, it will invoke after `SetParam` and
  /// `SetContext`, that is both the param_ and context_ are valid.
  virtual void PrepareForRun() {}

  /// Run kernel initialization if needed at every run (eg. input shape changed)
  virtual void ReInitWhenNeeded() {}

  /// Run the kernel. Before Run, both the param_ and context_ should be valid.
  virtual void Run() = 0;

#ifdef LITE_WITH_METAL
  virtual void SaveOutput() {}
#endif

#ifdef LITE_WITH_PROFILE
  void SetProfiler(profile::Profiler* profiler, int id) {
    profiler_ = profiler;
    profile_id_ = id;
  }

  virtual void SetProfileRuntimeKernelInfo(
      paddle::lite::profile::OpCharacter* ch) {
    ch->kernel_func_name = std::string("NotImpl");
#ifdef LITE_WITH_OPENCL
    ch->cl_event = event_;
#endif
  }

  virtual void SetIsKernelTest(bool is_kernel_test) {
    is_kernel_test_ = is_kernel_test;
  }
#endif

  void Launch() {
    /// First run, init kernel, do weights transform once
    if (is_first_epoch_) {
      PrepareForRun();
      is_first_epoch_ = false;
    }
    /// re-init the kernel if needed (input shape should be checked in conv
    /// kernel)
    ReInitWhenNeeded();

    // Reset the workspace to make every kernel in the same thread to share the
    // temporary memory.
    WorkSpace::Global_Host().AllocReset();
#if defined(LITE_WITH_X86)
    WorkSpace::Global_X86().AllocReset();
#endif
#if defined(LITE_WITH_METAL)
    WorkSpace::Global_METAL().AllocReset();
#endif

#ifdef LITE_WITH_PROFILE
    if (!is_kernel_test_) {
      profiler_->StopTiming(profile::Type::kCreate, profile_id_, ctx_.get());
      profiler_->StartTiming(profile::Type::kDispatch, profile_id_, ctx_.get());
    }

    Run();

    // skip test
    if (!is_kernel_test_) {
      SetProfileRuntimeKernelInfo(profiler_->GetOpCharacter(profile_id_));
      profiler_->StopTiming(profile::Type::kDispatch, profile_id_, ctx_.get());
    }

#else
    Run();
#endif
  }

  void SetContext(std::unique_ptr<KernelContext>&& ctx) {
    ctx_ = std::move(ctx);
  }
  template <typename T>
  void SetParam(T param) {
    param_.set(param);
  }
  template <typename P>
  P& Param() const {
    return *param_.get_mutable<P>();
  }

  // This is used in the kernels that takes 'kAny' places and inference the
  // output place. For `ScaleCompute` and `IoCopyCompute`, their input types are
  // declared as 'kAny' in some Place field, and the output is also `kAny`, but
  // when in real execution, when takes some non-kAny type as input, the
  // output's kAny-fields can be determained. For example, when the
  // `ScaleCompute` takes `TensorFp32NCHWTy` as input, its output should be also
  // `TensorFp32NCHWTy`. This type inference rule is different for each kernel,
  // so we make it a virtual method.
  // One can custom this handler to make a specific type inference rule for a
  // kernel, or leave the default to force the kernel use the system's
  // type-inference rules.
  virtual std::unique_ptr<type_infer_handler_t> GetTypeInferHandler() {
    return nullptr;
  }

  void set_op_type(const std::string& type) { op_type_ = type; }
  const std::string& op_type() const { return op_type_; }

  // Get input declaration Type.
  const Type* GetInputDeclType(const std::string& arg_name) const;

  // Get output declaration Type.
  const Type* GetOutputDeclType(const std::string& arg_name) const;

  void set_alias(const std::string& x) { alias_ = x; }
  const std::string& alias() const { return alias_; }

  virtual Place place() const = 0;
  virtual TargetType target() const = 0;
  virtual PrecisionType precision() const = 0;
  virtual DataLayoutType layout() const = 0;
  const KernelContext* context() const { return ctx_.get(); }
  KernelContext* mutable_context() { return ctx_.get(); }
  virtual std::string name() const = 0;

  // Short human-readable document.
  std::string summary() const;
  // Long human-readable document.
  virtual std::string doc() const { return ""; }
  // Generate the key of the parameter type.
  std::string GenParamTypeKey() const;

  // Used to serialize the kernel.
  std::string SerializedKernelType() const {
    return SerializeKernelType(op_type(), alias(), place());
  }

  static std::string SerializeKernelType(const std::string& op_type,
                                         const std::string& alias,
                                         const Place& place);

  static void ParseKernelType(const std::string& kernel_type,
                              std::string* op_type,
                              std::string* alias,
                              Place* place);

  std::string key_with_alias() const { return op_type() + "/" + alias(); }

  virtual ~KernelBase() = default;
  void Torch() {}

 protected:
  std::unique_ptr<KernelContext> ctx_{nullptr};
  mutable operators::param_t param_;
  // The corresponding op type.
  std::string op_type_{};
  // The extra identity to help defficiate a specific kernel, op_type_ + alias_
  // is the unique ID for the kernel.
  std::string alias_{};
  bool is_first_epoch_{true};

#ifdef LITE_WITH_PROFILE
  profile::Profiler* profiler_{nullptr};
  int profile_id_{-1};
  bool is_kernel_test_{true};
#endif
#ifdef LITE_WITH_OPENCL
  cl::Event event_;
  cl::Event event_1;
  cl::Event event_2;
  bool fp16_support_{paddle::lite::CLRuntime::Global()->get_precision() ==
                     lite_api::CL_PRECISION_FP16};
#endif
};

// Light-weight kernel implementation.
// The OpKernel is designed to implement the specific algorithm on a target
// device.
template <TargetType Target,
          PrecisionType Precision,
          DataLayoutType DataLayout = DataLayoutType::kNCHW>
class KernelLite : public KernelBase {
 public:
  // Run the kernel.
  void Run() override { CHECK(false) << "Not Implemented"; }

  TargetType target() const override { return Target; }
  PrecisionType precision() const override { return Precision; }
  DataLayoutType layout() const override { return DataLayout; }
  Place place() const override { return Place{Target, Precision, DataLayout}; }
  std::string name() const override;

  void Touch() {}

  KernelLite() = default;
  virtual ~KernelLite() = default;
};

template <TargetType Target, PrecisionType Precision, DataLayoutType DataLayout>
std::string KernelLite<Target, Precision, DataLayout>::name() const {
  return op_type() + ":" + TargetToStr(Target) + "/" +
         PrecisionToStr(Precision) + "/" + DataLayoutToStr(DataLayout);
}

}  // namespace lite
}  // namespace paddle
