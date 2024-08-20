// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/js-disposable-stack.h"

#include "include/v8-maybe.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/handles/maybe-handles.h"
#include "src/heap/factory.h"
#include "src/objects/fixed-array-inl.h"
#include "src/objects/heap-object.h"
#include "src/objects/js-disposable-stack-inl.h"
#include "src/objects/js-function.h"
#include "src/objects/js-objects.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/js-promise.h"
#include "src/objects/objects-inl.h"
#include "src/objects/objects.h"
#include "src/objects/oddball.h"
#include "src/objects/tagged.h"
#include "v8-promise.h"

namespace v8 {
namespace internal {

// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposeresources
// (TODO:rezvan):
// https://github.com/tc39/proposal-explicit-resource-management/pull/219
MaybeHandle<Object> JSDisposableStackBase::DisposeResources(
    Isolate* isolate, DirectHandle<JSDisposableStackBase> disposable_stack,
    MaybeHandle<Object> maybe_continuation_error,
    DisposableStackResourcesType resources_type) {
  DCHECK(!IsUndefined(disposable_stack->stack()));

  DirectHandle<FixedArray> stack(disposable_stack->stack(), isolate);

  int length = disposable_stack->length();

  MaybeHandle<Object> result;
  Handle<Object> continuation_error;

  if (maybe_continuation_error.ToHandle(&continuation_error)) {
    disposable_stack->set_error(*continuation_error);
  }

  // 1. For each element resource of
  // disposeCapability.[[DisposableResourceStack]], in reverse list order, do
  while (length > 0) {
    Tagged<Object> stack_type = stack->get(--length);

    Tagged<Object> tagged_method = stack->get(--length);
    Handle<Object> method(tagged_method, isolate);

    Tagged<Object> tagged_value = stack->get(--length);
    Handle<Object> value(tagged_value, isolate);

    Handle<Object> argv[] = {value};

    //  a. Let result be Completion(Dispose(resource.[[ResourceValue]],
    //  resource.[[Hint]], resource.[[DisposeMethod]])).
    auto stack_type_case = static_cast<int>(Cast<Smi>(stack_type).value());
    DisposeMethodCallType call_type =
        DisposeCallTypeBit::decode(stack_type_case);
    DisposeMethodHint hint = DisposeHintBit::decode(stack_type_case);

    v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
    try_catch.SetVerbose(false);
    try_catch.SetCaptureMessage(false);

    if (call_type == DisposeMethodCallType::kValueIsReceiver) {
      result = Execution::Call(isolate, method, value, 0, nullptr);
    } else if (call_type == DisposeMethodCallType::kValueIsArgument) {
      result = Execution::Call(isolate, method,
                               ReadOnlyRoots(isolate).undefined_value_handle(),
                               1, argv);
    }

    Handle<Object> result_handle;

    if (result.ToHandle(&result_handle)) {
      if (hint == DisposeMethodHint::kAsyncDispose) {
        DCHECK_NE(resources_type, DisposableStackResourcesType::kAllSync);
        disposable_stack->set_length(length);

        if (result.ToHandle(&result_handle)) {
          Handle<JSFunction> promise_function = isolate->promise_function();
          Handle<Object> argv[] = {result_handle};
          Handle<Object> resolve_result =
              Execution::CallBuiltin(isolate, isolate->promise_resolve(),
                                     promise_function, arraysize(argv), argv)
                  .ToHandleChecked();
          return Cast<JSReceiver>(resolve_result);
        }
      }
    } else {
      // b. If result is a throw completion, then
      DCHECK(isolate->has_exception());
      DCHECK(try_catch.HasCaught());
      Handle<Object> current_error(isolate->exception(), isolate);
      if (!isolate->is_catchable_by_javascript(*current_error)) {
        return {};
      }
      HandleErrorInDisposal(isolate, disposable_stack, current_error);
    }
  }

  // 2. NOTE: After disposeCapability has been disposed, it will never be used
  // again. The contents of disposeCapability.[[DisposableResourceStack]] can be
  // discarded in implementations, such as by garbage collection, at this point.
  // 3. Set disposeCapability.[[DisposableResourceStack]] to a new empty List.
  disposable_stack->set_stack(ReadOnlyRoots(isolate).empty_fixed_array());
  disposable_stack->set_length(0);
  disposable_stack->set_state(DisposableStackState::kDisposed);

  Handle<Object> existing_error_handle(disposable_stack->error(), isolate);
  disposable_stack->set_error(*(isolate->factory()->uninitialized_value()));

  // 4. Return ? completion.
  if (!IsUninitialized(*existing_error_handle) &&
      !(existing_error_handle.equals(continuation_error))) {
    isolate->Throw(*existing_error_handle);
    return MaybeHandle<Object>();
  }
  return isolate->factory()->undefined_value();
}

}  // namespace internal
}  // namespace v8
