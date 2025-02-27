// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include <gtest/gtest.h>

#include "lite/api/paddle_use_kernels.h"
#include "lite/api/paddle_use_ops.h"
#include "lite/core/test/arena/framework.h"
#include "lite/tests/utils/fill_data.h"

namespace paddle {
namespace lite {

template <typename T>
inline void ShiftAlongDim(T* data,
                          const DDim& input_dim,
                          int64_t dim,
                          int64_t shift) {
  if (dim < 0) {
    dim += input_dim.size();
  }
  if (input_dim[dim] == 0) {
    return;
  }
  shift = shift % input_dim[dim];
  if (shift < 0) {
    shift += input_dim[dim];
  }

  auto outer_loops = 1;
  for (auto i = 0; i < dim; i++) {
    outer_loops *= input_dim[i];
  }
  auto slice_width = 1;
  for (auto i = dim + 1; i < input_dim.size(); i++) {
    slice_width *= input_dim[i];
  }

  if (shift == 0) {
    return;
  }

  std::vector<T> head;
  auto head_size = slice_width * (input_dim[dim] - shift);
  head.resize(head_size);

  for (auto i = 0; i < outer_loops; i++) {
    for (auto j = 0; j < head_size; j++) {
      head[j] = data[i * input_dim[dim] * slice_width + j];
    }
    for (auto j = input_dim[dim] - shift; j < input_dim[dim]; j++) {
      auto dst_pos = j - input_dim[dim] + shift;
      for (auto k = 0; k < slice_width; k++) {
        data[(i * input_dim[dim] + dst_pos) * slice_width + k] =
            data[(i * input_dim[dim] + j) * slice_width + k];
      }
    }
    for (auto j = 0; j < head_size; j++) {
      data[(i * input_dim[dim] + shift) * slice_width + j] = head[j];
    }
  }
}

class RollComputeTester : public arena::TestCase {
 protected:
  // common attributes for this op.
  std::string x_ = "X";
  std::string out_ = "Out";
  std::string shifts_tensor_;
  DDim x_dims_{{1, 5, 6, 7}};
  std::vector<int64_t> axis_;
  std::vector<int64_t> shifts_;

 public:
  RollComputeTester(const Place& place,
                    const std::string& alias,
                    const DDim& x_dims,
                    const std::vector<int64_t>& axis,
                    const std::vector<int64_t>& shifts,
                    const bool use_shifts_tensor = false)
      : TestCase(place, alias), x_dims_(x_dims), axis_(axis), shifts_(shifts) {
    if (use_shifts_tensor) {
      shifts_tensor_ = "ShiftsTensor";
    }
  }

  void RunBaseline(Scope* scope) override {
    auto* x = scope->FindTensor(x_);
    std::vector<int64_t> shifts;
    if (!shifts_tensor_.empty()) {
      auto* shift = scope->FindTensor(shifts_tensor_);
      auto shift_data = shift->template data<int64_t>();
      for (int64_t i = 0; i < shift->numel(); i++) {
        shifts.push_back(shift_data[i]);
      }
    } else {
      shifts = shifts_;
    }
    int nums = shifts.size();
    DDim input_dim = x->dims();
    int input_size = input_dim.size();
    // axis = none, reshape to 1-D tensor
    if (axis_.size() == 0) {
      axis_.push_back(0l);
      input_dim = DDim(std::vector<int64_t>({input_size}));
    }
    auto* out = scope->NewTensor(out_);
    CHECK(out);
    out->CopyDataFrom(*x);
    auto* out_data = out->mutable_data<float>();
    for (size_t i = 0; i < nums; i++) {
      ShiftAlongDim(out_data, input_dim, axis_[i], shifts_[i]);
    }
  }

  void PrepareOpDesc(cpp::OpDesc* op_desc) override {
    op_desc->SetType("roll");
    op_desc->SetInput("X", {x_});
    op_desc->SetOutput("Out", {out_});
    op_desc->SetAttr("axis", axis_);
    op_desc->SetAttr("shifts", shifts_);
    if (!shifts_tensor_.empty()) {
      op_desc->SetInput("ShiftsTensor", {shifts_tensor_});
    }
  }

  void PrepareData() override {
    std::vector<float> x_data(x_dims_.production());
    fill_data_rand<float>(x_data.data(), -1, 1, x_data.size());
    SetCommonTensor<float>(x_, x_dims_, x_data.data());
    if (!shifts_tensor_.empty()) {
      SetCommonTensor(shifts_tensor_,
                      DDim({static_cast<int64_t>(axis_.size())}),
                      axis_.data(),
                      {},
                      true);
    }
  }
};

void TestRoll(const Place& place, float abs_error) {
  std::vector<std::vector<int64_t>> axes = {{
                                                0,
                                            },
                                            {
                                                2,
                                            },
                                            {1, 3}};
  std::string alias{"def"};
  for (std::vector<int64_t>& axis : axes) {
    for (int n : {1, 3}) {
      for (int c : {3, 6}) {
        for (int h : {9, 18}) {
          for (int w : {9, 18}) {
            for (bool use_shifts_tensor : {false, true}) {
              DDim dims{{n, c, h, w}};
              std::unique_ptr<arena::TestCase> tester(new RollComputeTester(
                  place, alias, dims, axis, axis, use_shifts_tensor));
              arena::Arena arena(std::move(tester), place, abs_error);
              arena.TestPrecision();
            }
          }
        }
      }
    }
  }
}

TEST(roll, precision) {
  Place place;
  float abs_error = 1e-5;
#if defined(LITE_WITH_X86) || defined(LITE_WITH_ARM)
  place = TARGET(kHost);
#else
  return;
#endif

  TestRoll(place, abs_error);
}

}  // namespace lite
}  // namespace paddle
