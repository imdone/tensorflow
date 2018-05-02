/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/data_flow_ops.cc.

#include <limits.h>
#include <atomic>
#include <vector>

#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;
#ifdef TENSORFLOW_USE_SYCL
typedef Eigen::SyclDevice SYCLDevice;
#endif  // TENSORFLOW_USE_SYCL

class Stack : public ResourceBase {
 public:
  static std::atomic<int64> stack_counter;

  struct TensorAndAllocation {
    Tensor tensor;
    AllocatorAttributes alloc_attrs;
    bool swapped_to_cpu;
  };

  Stack(const DataType& elem_type, const string& stack_name, int max_size)
      : elem_type_(elem_type),
        stack_name_(stack_name),
        max_size_(max_size),
        closed_(false) {}

  Status Push(const TensorAndAllocation& value) {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(CheckNotClosed());
    if (max_size_ >= 0 && stack_.size() >= max_size_) {
      return errors::InvalidArgument("Stack[", stack_name_, "] overflowed ",
                                     "its max_size (", max_size_, ")");
    }
    stack_.push_back(value);
    return Status::OK();
  }

  Status Pop(TensorAndAllocation* value) {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(CheckNotClosed());
    if (stack_.empty()) {
      return errors::InvalidArgument("Stack[", stack_name_,
                                     "] is empty when calling Pop().");
    }
    *value = stack_.back();
    stack_.pop_back();
    return Status::OK();
  }

  // We don't swap the first tensor on the stack and any subsequent tensors
  // that share the buffer with the first tensor.
  bool IsUsefulToSwap(const Tensor& tensor) const {
    mutex_lock l(mu_);
    if (stack_.empty()) {
      return false;
    }
    const Tensor& first = stack_.front().tensor;
    return !tensor.SharesBufferWith(first);
  }

  void Close() {
    mutex_lock l(mu_);
    stack_.clear();
    closed_ = true;
  }

  DataType ElemType() { return elem_type_; }

  string DebugString() override {
    mutex_lock l(mu_);
    return strings::StrCat("Stack[", stack_name_, "]");
  }

  const string& stack_name() { return stack_name_; }

 private:
  friend class StackOp;
  mutex* mu() { return &mu_; }

  mutable mutex mu_;
  DataType elem_type_;
  const string stack_name_;
  Tensor handle_;
  int max_size_;
  bool closed_ GUARDED_BY(mu_);
  std::vector<TensorAndAllocation> stack_ GUARDED_BY(mu_);

  Status CheckNotClosed() const EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    if (closed_) {
      return errors::InvalidArgument("Stack[", stack_name_,
                                     "] has already been closed.");
    }
    return Status::OK();
  }
};

Status GetStack(OpKernelContext* ctx, Stack** stack) {
  string key;
  if (ctx->input_dtype(0) == DT_RESOURCE) {
    auto resource = ctx->input(0).flat<ResourceHandle>()(0);
    key = resource.name();
  } else {
    Tensor Tstack_handle = ctx->mutable_input(0, false);
    if (Tstack_handle.NumElements() != 2) {
      return errors::InvalidArgument(
          "Stack handle must have two elements, but had shape: ",
          Tstack_handle.shape().DebugString());
    }
    const string& container = Tstack_handle.flat<string>()(0);
    const string& stack_name = Tstack_handle.flat<string>()(1);
    key = strings::StrCat(container, stack_name);
  }
  ResourceMgr* rm = ctx->resource_manager();
  if (rm == nullptr) {
    return errors::Internal("No resource manager.");
  }
  auto* step_container = ctx->step_container();
  if (step_container == nullptr) {
    return errors::Internal("No step container.");
  }
  TF_RETURN_IF_ERROR(rm->Lookup(step_container->name(), key, stack));
  return Status::OK();
}

std::atomic<int64> Stack::stack_counter{0};

// A per-run local stack. The stack uses a "per-step" resource manager which
// ensures that correct garbage collection on error or successful completion.
class StackOp : public OpKernel {
 public:
  explicit StackOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("elem_type", &elem_type_));
    OP_REQUIRES_OK(context, context->GetAttr("stack_name", &stack_name_));
    if (stack_name_.empty()) stack_name_ = name();
  }

  void Compute(OpKernelContext* ctx) override {
    int32 size = std::numeric_limits<int32>::max();
    if (ctx->num_inputs() > 0) {
      const Tensor* tensor_size;
      OP_REQUIRES_OK(ctx, ctx->input("max_size", &tensor_size));

      OP_REQUIRES(ctx, TensorShapeUtils::IsScalar(tensor_size->shape()),
                  errors::InvalidArgument(
                      "Stack size must be a scalar, but had shape: ",
                      tensor_size->shape().DebugString()));

      int32 size_value = tensor_size->scalar<int32>()();
      if (size_value >= 0) {
        size = size_value;
      }
    }

    static const char kContainer[] = "_stacks";
    auto stack_id = Stack::stack_counter.fetch_add(1);
    string stack_name = strings::StrCat(stack_name_, "_", stack_id);
    // Store the handle in a per-step container.
    ResourceMgr* rm = ctx->resource_manager();
    OP_REQUIRES(ctx, rm != nullptr, errors::Internal("No resource manager."));
    string key = strings::StrCat(kContainer, stack_name);
    Stack* stack = new Stack(elem_type_, stack_name, size);
    auto* step_container = ctx->step_container();
    OP_REQUIRES(ctx, step_container != nullptr,
                errors::Internal("No step container."));
    OP_REQUIRES_OK(ctx, rm->Create(step_container->name(), key, stack));
    if (IsRefType(ctx->expected_output_dtype(0))) {
      // Create the stack handle.
      AllocatorAttributes alloc_attr;
      alloc_attr.set_on_host(true);
      OP_REQUIRES_OK(ctx, ctx->allocate_temp(tensorflow::DT_STRING,
                                             tensorflow::TensorShape({2}),
                                             &stack->handle_, alloc_attr));
      auto handle = stack->handle_.flat<string>();
      handle(0) = kContainer;
      handle(1) = std::move(stack_name);
      ctx->set_output_ref(0, stack->mu(), &stack->handle_);
    } else {
      Tensor* handle;
      OP_REQUIRES_OK(ctx, ctx->allocate_output(0, TensorShape({}), &handle));
      handle->flat<ResourceHandle>()(0) =
          MakePerStepResourceHandle<Stack>(ctx, key);
    }
  }

 private:
  DataType elem_type_;
  string stack_name_;

  TF_DISALLOW_COPY_AND_ASSIGN(StackOp);
};

REGISTER_KERNEL_BUILDER(Name("Stack").Device(DEVICE_CPU), StackOp);
REGISTER_KERNEL_BUILDER(Name("Stack").Device(DEVICE_GPU).HostMemory("handle"),
                        StackOp);
REGISTER_KERNEL_BUILDER(Name("StackV2").Device(DEVICE_CPU), StackOp);
REGISTER_KERNEL_BUILDER(Name("StackV2")
                            .Device(DEVICE_GPU)
                            .HostMemory("max_size")
                            .HostMemory("handle"),
                        StackOp);
#ifdef TENSORFLOW_USE_SYCL
REGISTER_KERNEL_BUILDER(Name("Stack").Device(DEVICE_SYCL).HostMemory("handle"),
                        StackOp);
REGISTER_KERNEL_BUILDER(Name("StackV2")
                            .Device(DEVICE_SYCL)
                            .HostMemory("max_size")
                            .HostMemory("handle"),
                        StackOp);
#endif  // TENSORFLOW_USE_SYCL

template <typename Device>
class StackPushOp : public AsyncOpKernel {
 public:
  explicit StackPushOp(OpKernelConstruction* context) : AsyncOpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("swap_memory", &swap_memory_));
  }

  void ComputeAsync(OpKernelContext* ctx, DoneCallback done) override {
    // Get the stack from the handle.
    Stack* stack = nullptr;
    OP_REQUIRES_OK_ASYNC(ctx, GetStack(ctx, &stack), done);
    core::ScopedUnref unref(stack);

    if (ctx->input_dtype(1) != stack->ElemType()) {
      ctx->CtxFailure(errors::InvalidArgument("Must have type ",
                                              stack->ElemType(), " but got ",
                                              ctx->input_dtype(1)));
      done();
      return;
    }

    // Push the tensor onto the stack. Swap the tensor to CPU if instructed.
    const Tensor& tensor = ctx->input(1);
    AllocatorAttributes alloc_attrs = ctx->input_alloc_attr(1);
    // For now, we use a simple heuristic for swapping: A GPU tensor is moved
    // to CPU if the tensor has more than kCopyThreshold bytes and the GPU
    // allocator says more than kOccupancy of the memory is in use.
    static constexpr int kCopyThreshold = 2048;
    static constexpr double kOccupancy = 0.7;
    if (swap_memory_ && !alloc_attrs.on_host() &&
        (std::is_same<Device, GPUDevice>::value
#ifdef TENSORFLOW_USE_SYCL
         || std::is_same<Device, SYCLDevice>::value
#endif  // TENSORFLOW_USE_SYCL
         ) &&
        tensor.TotalBytes() > kCopyThreshold && stack->IsUsefulToSwap(tensor)) {
      DeviceContext* device_ctxt = ctx->op_device_context();
      auto device = static_cast<tensorflow::Device*>(ctx->device());
      Allocator* allocator = device->GetAllocator(alloc_attrs);
      AllocatorStats stats;
      allocator->GetStats(&stats);
      if (stats.bytes_in_use > (stats.bytes_limit * kOccupancy)) {
        // Asynchronously copy the tensor from GPU to CPU memory.
        // TODO (yuanbyu): Swap the oldest tensor first. id:3342
        // https://github.com/imdone/tensorflow/issues/3341
        AllocatorAttributes host_alloc_attrs;
        host_alloc_attrs.set_gpu_compatible(true);
        host_alloc_attrs.set_on_host(true);
        Allocator* cpu_allocator = device->GetAllocator(host_alloc_attrs);
        Tensor* cpu_tensor =
            new Tensor(cpu_allocator, tensor.dtype(), tensor.shape());
        device_ctxt->CopyDeviceTensorToCPU(
            &tensor, "StackPush", device, cpu_tensor,
            [cpu_tensor, stack, ctx, done](const Status& s) {
              ctx->SetStatus(s);
              if (s.ok()) {
                AllocatorAttributes alloc_attrs = ctx->input_alloc_attr(1);
                ctx->SetStatus(stack->Push({*cpu_tensor, alloc_attrs, true}));
              }
              if (ctx->status().ok()) {
                ctx->set_output(0, *cpu_tensor);
              }
              done();
              delete cpu_tensor;
            });
        return;
      }
    }

    // Execute synchronously if not swapped.
    OP_REQUIRES_OK_ASYNC(ctx, stack->Push({tensor, alloc_attrs, false}), done);
    ctx->set_output(0, tensor);
    done();
  }

  bool IsExpensive() override { return false; }

 private:
  bool swap_memory_;
};

REGISTER_KERNEL_BUILDER(Name("StackPush").Device(DEVICE_CPU),
                        StackPushOp<CPUDevice>);
REGISTER_KERNEL_BUILDER(Name("StackPushV2").Device(DEVICE_CPU),
                        StackPushOp<CPUDevice>);

#define REGISTER_GPU_KERNEL(type)                         \
  REGISTER_KERNEL_BUILDER(Name("StackPush")               \
                              .Device(DEVICE_GPU)         \
                              .HostMemory("handle")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<GPUDevice>);        \
  REGISTER_KERNEL_BUILDER(Name("StackPushV2")             \
                              .Device(DEVICE_GPU)         \
                              .HostMemory("handle")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<GPUDevice>);

TF_CALL_NUMBER_TYPES_NO_INT32(REGISTER_GPU_KERNEL);
#undef REGISTER_GPU_KERNEL

// Special GPU kernels for int32 and bool.
// TODO (b/25387198): Also enable int32 in device memory. This kernel id:4046
// https://github.com/imdone/tensorflow/issues/4044
// registration requires all int32 inputs and outputs to be in host memory.
#define REGISTER_GPU_HOST_KERNEL(type)                    \
  REGISTER_KERNEL_BUILDER(Name("StackPush")               \
                              .Device(DEVICE_GPU)         \
                              .HostMemory("handle")       \
                              .HostMemory("elem")         \
                              .HostMemory("output")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<GPUDevice>);        \
  REGISTER_KERNEL_BUILDER(Name("StackPushV2")             \
                              .Device(DEVICE_GPU)         \
                              .HostMemory("handle")       \
                              .HostMemory("elem")         \
                              .HostMemory("output")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<GPUDevice>);

REGISTER_GPU_HOST_KERNEL(int32);
REGISTER_GPU_HOST_KERNEL(bool);

#undef REGISTER_GPU_HOST_KERNEL

#ifdef TENSORFLOW_USE_SYCL
#define REGISTER_SYCL_KERNEL(type)                        \
  REGISTER_KERNEL_BUILDER(Name("StackPush")               \
                              .Device(DEVICE_SYCL)        \
                              .HostMemory("handle")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<SYCLDevice>);

TF_CALL_GPU_NUMBER_TYPES(REGISTER_SYCL_KERNEL);

#define REGISTER_SYCL_HOST_KERNEL(type)                   \
  REGISTER_KERNEL_BUILDER(Name("StackPush")               \
                              .Device(DEVICE_SYCL)        \
                              .HostMemory("handle")       \
                              .HostMemory("elem")         \
                              .HostMemory("output")       \
                              .TypeConstraint<type>("T"), \
                          StackPushOp<SYCLDevice>)

REGISTER_SYCL_HOST_KERNEL(int32);
REGISTER_SYCL_HOST_KERNEL(bool);
#undef REGISTER_SYCL_KERNEL
#undef REGISTER_SYCL_HOST_KERNEL
#endif  // TENSORFLOW_USE_SYCL

class StackPopOp : public AsyncOpKernel {
 public:
  explicit StackPopOp(OpKernelConstruction* context) : AsyncOpKernel(context) {}

  void ComputeAsync(OpKernelContext* ctx, DoneCallback done) override {
    // Get the stack from the handle.
    Stack* stack = nullptr;
    OP_REQUIRES_OK_ASYNC(ctx, GetStack(ctx, &stack), done);
    core::ScopedUnref unref(stack);

    // Pop the tensor. Transfer the tensor back to device if it was
    // swapped out to CPU.
    Stack::TensorAndAllocation value;
    OP_REQUIRES_OK_ASYNC(ctx, stack->Pop(&value), done);
    if (value.swapped_to_cpu) {
      // Asynchronously copy the tensor back from CPU to GPU memory.
      DeviceContext* device_ctxt = ctx->op_device_context();
      Device* device = static_cast<Device*>(ctx->device());
      Tensor* cpu_tensor = &value.tensor;
      Allocator* gpu_allocator = device->GetAllocator(value.alloc_attrs);
      Tensor* device_tensor =
          new Tensor(gpu_allocator, cpu_tensor->dtype(), cpu_tensor->shape());
      device_ctxt->CopyCPUTensorToDevice(
          cpu_tensor, device, device_tensor,
          [device_tensor, ctx, done](const Status& s) {
            ctx->SetStatus(s);
            if (s.ok()) {
              ctx->set_output(0, *device_tensor);
            }
            done();
            delete device_tensor;
          });
    } else {
      // Execute synchronously if not swapped.
      ctx->set_output(0, value.tensor);
      done();
    }
  }

  bool IsExpensive() override { return false; }
};

REGISTER_KERNEL_BUILDER(Name("StackPop").Device(DEVICE_CPU), StackPopOp);
REGISTER_KERNEL_BUILDER(Name("StackPopV2").Device(DEVICE_CPU), StackPopOp);

#define REGISTER_GPU_KERNEL(type)                                 \
  REGISTER_KERNEL_BUILDER(Name("StackPop")                        \
                              .Device(DEVICE_GPU)                 \
                              .HostMemory("handle")               \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp);                            \
  REGISTER_KERNEL_BUILDER(Name("StackPopV2")                      \
                              .Device(DEVICE_GPU)                 \
                              .HostMemory("handle")               \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp);

TF_CALL_NUMBER_TYPES_NO_INT32(REGISTER_GPU_KERNEL);
#undef REGISTER_GPU_KERNEL

// Special GPU kernels for int32 and bool.
// TODO (b/25387198): Also enable int32 in device memory. This kernel id:2341
// https://github.com/imdone/tensorflow/issues/2340
// registration requires all int32 inputs and outputs to be in host memory.
#define REGISTER_GPU_HOST_KERNEL(type)                            \
  REGISTER_KERNEL_BUILDER(Name("StackPop")                        \
                              .Device(DEVICE_GPU)                 \
                              .HostMemory("handle")               \
                              .HostMemory("elem")                 \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp);                            \
  REGISTER_KERNEL_BUILDER(Name("StackPopV2")                      \
                              .Device(DEVICE_GPU)                 \
                              .HostMemory("handle")               \
                              .HostMemory("elem")                 \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp);

REGISTER_GPU_HOST_KERNEL(int32);
REGISTER_GPU_HOST_KERNEL(bool);

#undef REGISTER_GPU_HOST_KERNEL

#ifdef TENSORFLOW_USE_SYCL
#define REGISTER_SYCL_KERNEL(type)                                \
  REGISTER_KERNEL_BUILDER(Name("StackPop")                        \
                              .Device(DEVICE_SYCL)                \
                              .HostMemory("handle")               \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp)

TF_CALL_GPU_NUMBER_TYPES(REGISTER_SYCL_KERNEL);

#define REGISTER_SYCL_HOST_KERNEL(type)                           \
  REGISTER_KERNEL_BUILDER(Name("StackPop")                        \
                              .Device(DEVICE_SYCL)                \
                              .HostMemory("handle")               \
                              .HostMemory("elem")                 \
                              .TypeConstraint<type>("elem_type"), \
                          StackPopOp)

REGISTER_SYCL_HOST_KERNEL(int32);
REGISTER_SYCL_HOST_KERNEL(bool);

#undef REGISTER_SYCL_KERNEL
#undef REGISTER_SYCL_HOST_KERNEL
#endif  // TENSORFLOW_USE_SYCL

class StackCloseOp : public OpKernel {
 public:
  explicit StackCloseOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override {
    Stack* stack = nullptr;
    OP_REQUIRES_OK(ctx, GetStack(ctx, &stack));
    core::ScopedUnref unref(stack);
    stack->Close();
  }

  bool IsExpensive() override { return false; }
};

REGISTER_KERNEL_BUILDER(Name("StackClose").Device(DEVICE_CPU), StackCloseOp);
REGISTER_KERNEL_BUILDER(
    Name("StackClose").Device(DEVICE_GPU).HostMemory("handle"), StackCloseOp);
REGISTER_KERNEL_BUILDER(Name("StackCloseV2").Device(DEVICE_CPU), StackCloseOp);
REGISTER_KERNEL_BUILDER(
    Name("StackCloseV2").Device(DEVICE_GPU).HostMemory("handle"), StackCloseOp);
#ifdef TENSORFLOW_USE_SYCL
REGISTER_KERNEL_BUILDER(
    Name("StackClose").Device(DEVICE_SYCL).HostMemory("handle"), StackCloseOp);
REGISTER_KERNEL_BUILDER(
    Name("StackCloseV2").Device(DEVICE_SYCL).HostMemory("handle"),
    StackCloseOp);
#endif  // TENSORFLOW_USE_SYCL

}  // namespace tensorflow
