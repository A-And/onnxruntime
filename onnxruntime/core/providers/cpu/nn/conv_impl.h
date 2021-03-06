/**
* Copyright (c) 2016-present, Facebook, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
/* Modifications Copyright (c) Microsoft. */

#pragma once

#include "core/providers/cpu/nn/conv.h"
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"
#include "core/mlas/inc/mlas.h"

namespace onnxruntime {
template <typename T>
void fuse_activation(const std::string& activation, T* y_data, size_t size, float alpha) {
  EigenVectorArrayMap<T> y_vec(y_data, size);
  if (activation.empty()) {
    return;
  } else if (activation == "Relu") {
    y_vec = y_vec.cwiseMax(0);
  } else if (activation == "Sigmoid") {
    y_vec = (y_vec >= 0).select(1 / (1. + (-y_vec.abs()).exp()), 1 - 1 / (1. + (-y_vec.abs()).exp()));
  } else if (activation == "Tanh") {
    y_vec = y_vec.tanh();
  } else if (activation == "LeakyRelu") {
    y_vec = (y_vec >= 0).select(y_vec, (T)alpha * y_vec);
  } else {
    ONNXRUNTIME_NOT_IMPLEMENTED("Not implemented fused activation: ", activation);
  }
}

template <typename T>
Status Conv<T>::Compute(OpKernelContext* context) const {
  size_t num_inputs = OpKernel::Node().InputDefs().size();

  const Tensor* X = context->Input<Tensor>(0);
  const Tensor* W = context->Input<Tensor>(1);
  const Tensor* B = num_inputs == 3 ? context->Input<Tensor>(2) : nullptr;
  const int64_t N = X->Shape()[0];
  const int64_t C = X->Shape()[1];
  const int64_t M = W->Shape()[0];
  ONNXRUNTIME_RETURN_IF_ERROR(ValidateInputShape(X, W));

  std::vector<int64_t> kernel_shape = ComputeKernelShape(W->Shape());

  if (kernel_shape.size() + 2 != W->Shape().NumDimensions()) {
    return ONNXRUNTIME_MAKE_STATUS(ONNXRUNTIME, FAIL, "kernel_shape num_dims is not compatible with W num_dims.",
                                   " kernel_shape: ", TensorShape(kernel_shape).ToString().c_str(),
                                   " W: ", W->Shape().ToString().c_str());
  }

  for (size_t i = 0; i < kernel_shape.size(); ++i) {
    if (kernel_shape[i] != W->Shape()[i + 2]) {
      return ONNXRUNTIME_MAKE_STATUS(ONNXRUNTIME, FAIL, "kernel_shape is not compatible with W shape.",
                                     " kernel_shape: ", TensorShape(kernel_shape).ToString().c_str(),
                                     " W: ", W->Shape().ToString().c_str());
    }
  }

  bool Is2DKernel = kernel_shape.size() == 2;
  std::vector<int64_t> pads(pads_);
  if (pads.empty()) {
    pads.resize(kernel_shape.size() * 2, 0);
  }
  std::vector<int64_t> dilations(dilations_);
  if (dilations.empty()) {
    dilations.resize(kernel_shape.size(), 1);
  }
  std::vector<int64_t> strides(strides_);
  if (strides.empty()) {
    strides.resize(kernel_shape.size(), 1);
  }

  std::vector<int64_t> Y_dims;
  Y_dims.insert(Y_dims.begin(), {N, M});
  TensorShape input_shape = X->Shape().Slice(2);
  ONNXRUNTIME_RETURN_IF_ERROR(InferOutputShape(input_shape, kernel_shape, strides, dilations, &pads, &Y_dims));
  Tensor* Y = context->Output(0, TensorShape(Y_dims));
  TensorShape output_shape = Y->Shape().Slice(2);

  const int64_t input_image_size = input_shape.Size();
  const int64_t output_image_size = output_shape.Size();
  const int64_t kernel_size = TensorShape(kernel_shape).Size();
  const int64_t X_offset = C / group_ * input_image_size;
  const int64_t Y_offset = Y->Shape().Size() / Y->Shape()[0] / group_;
  const int64_t W_offset = W->Shape().Size() / group_;
  const int64_t kernel_dim = C / group_ * kernel_size;
  const int64_t col_buffer_size = kernel_dim * output_image_size;

  AllocatorPtr alloc;
  ONNXRUNTIME_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));

  auto col_data = alloc->Alloc(sizeof(T) * col_buffer_size);
  BufferUniquePtr col_buffer(col_data, BufferDeleter(alloc));
  T* col_buffer_data = static_cast<T*>(col_buffer.get());

  const T* Xdata = X->template Data<T>();
  T* Ydata = Y->template MutableData<T>();

  TensorShape image_shape = X->Shape().Slice(1);
  std::vector<int64_t> col_buffer_shape{kernel_dim};
  col_buffer_shape.insert(col_buffer_shape.end(), output_shape.GetDims().begin(),
                          output_shape.GetDims().end());

  for (int image_id = 0; image_id < N; ++image_id) {
    for (int group_id = 0; group_id < group_; ++group_id) {
      if (Is2DKernel) {
        math::Im2col<T, CPUMathUtil, StorageOrder::NCHW>(
            Xdata + group_id * X_offset,
            C / group_,
            input_shape[0],
            input_shape[1],
            kernel_shape[0],
            kernel_shape[1],
            dilations[0],
            dilations[1],
            pads[0],
            pads[1],
            pads[2],
            pads[3],
            strides[0],
            strides[1],
            col_buffer_data,
            &CPUMathUtil::Instance());
      } else {
        math::Im2colNd<T, CPUMathUtil, StorageOrder::NCHW>(
            Xdata + group_id * X_offset,
            image_shape.GetDims().data(),
            col_buffer_shape.data(),
            C * input_image_size,
            col_buffer_size,
            kernel_shape.data(),
            strides.data(),
            dilations.data(),
            pads.data(),
            static_cast<int>(kernel_shape.size()),
            col_buffer_data,
            &CPUMathUtil::Instance());
      }
      math::Gemm<T, CPUMathUtil>(
          CblasNoTrans,
          CblasNoTrans,
          M / group_,
          output_image_size,
          kernel_dim,
          1,
          W->template Data<T>() + group_id * W_offset,
          col_buffer_data,
          0,
          Ydata + group_id * Y_offset,
          &CPUMathUtil::Instance());
    }

    if (B != nullptr) {
      auto Ymatrix = EigenMatrixMap<T>(Ydata, output_image_size, M);
      auto Bvec = ConstEigenVectorMap<T>(B->template Data<T>(), M);
      Ymatrix.rowwise() += Bvec.transpose();
    }
    fuse_activation(activation_, Ydata, Y_offset * group_, alpha_);

    Xdata += X_offset * group_;
    Ydata += Y_offset * group_;
  }

  return Status::OK();
}

template <>
Status Conv<float>::Compute(OpKernelContext* context) const;

}  // namespace onnxruntime
