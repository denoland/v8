# V8 14.9 fast-call snapshot serialization: investigation and conclusion

When upgrading Deno from `v8` 14.7 (rusty_v8 147.4.0) to 14.9 (149.0.0) in
[denoland/deno#34226], snapshot creation began aborting on every
`FunctionTemplate` that had `c_function_overloads` attached. The Deno PR
worked around it on the embedder side: skip `build_fast` while
`will_snapshot` is true, then re-attach the fast-call overloads in a runtime
pass (`upgrade_snapshotted_ops_with_fast_calls`) after deserialization.

This note records what changed upstream, why the workaround is correct,
whether `denoland/v8` should carry a patch, and what the embedder side still
owes us in follow-up hardening.

[denoland/deno#34226]: https://github.com/denoland/deno/pull/34226

## What changed in V8 between 14.7 and 14.9

In V8 14.7, `FunctionTemplate::SetCallHandler` stored each fast-call
overload as a pair of `Foreign` objects in a `FixedArray`:

    src/api/api.cc (14.7)
      i::DirectHandle<i::Object> address   = FromCData<kCFunctionTag>(...);
      i::DirectHandle<i::Object> signature = FromCData<kCFunctionInfoTag>(...);
      function_overloads->set(2*i,     *address);
      function_overloads->set(2*i + 1, *signature);

Both `kCFunctionTag` and `kCFunctionInfoTag` are listed under
`FOREIGN_TAG_LIST` in `include/v8-internal.h` and live in the *non*-managed
portion of the external pointer tag space.

In V8 14.9, this was consolidated into a single `Managed` per overload that
holds both pieces of data atomically:

    src/api/api.cc (14.9, lines 1349–1357)
      i::DirectHandle<i::Managed<i::CFunctionWithSignature>> overload =
          i::Managed<i::CFunctionWithSignature>::From(
              i_isolate, sizeof(i::CFunctionWithSignature),
              std::make_shared<i::CFunctionWithSignature>(
                  reinterpret_cast<const i::Address>(c_function.GetAddress()),
                  c_function.GetTypeInfo()));
      function_overloads->set(i, *overload);

The new tag `kCFunctionWithSignatureTag` is in `MANAGED_TAG_LIST`, i.e.
inside the managed-resource range that `IsManagedExternalPointerType`
returns true for.

The motivation for the consolidation chain is atomicity. The introductory
CL [crrev.com/c/7668829] ("[fastapi] Store callback and signature in one
C++ object", merged 2026-03-25 into V8 main, ~14.8) replaces the
`[address, signature]` `FixedArray` pair with a single
`Managed<CFunctionWithSignature>` per overload. Its follow-up
[crrev.com/c/7715581] ("[wasm] Read c-function and signature atomically",
2026-04-01) and [crrev.com/c/7705870 / f5ac1a82] ("[fastapi] Read
c-function and signature atomically", `crbug.com/492077213`, 2026-03-27)
remove the now-redundant separate loads: a concurrent mutation of the
`FixedArray` could previously hand the JIT a `(function, signature)` pair
where the signature came from a different overload. The new single-pointer
load closes that gap. Two later CLs polished the shape:
[crrev.com/c/7721359] ("Use uint32_t for overload count/index",
2026-04-02) and [crrev.com/c/7725983] ("Use no_gc variants of Managed ptr
getters", 2026-04-02).

[crrev.com/c/7668829]: https://chromium-review.googlesource.com/c/v8/v8/+/7668829
[crrev.com/c/7715581]: https://chromium-review.googlesource.com/c/v8/v8/+/7715581
[crrev.com/c/7705870 / f5ac1a82]: https://chromium-review.googlesource.com/c/v8/v8/+/7708355
[crrev.com/c/7721359]: https://chromium-review.googlesource.com/c/v8/v8/+/7721359
[crrev.com/c/7725983]: https://chromium-review.googlesource.com/c/v8/v8/+/7725983

## Why this breaks startup snapshot serialization

The external pointer carried by a `Managed<CFunctionWithSignature>` is the
process-local heap address of the `CFunctionWithSignature` instance that
`std::make_shared` just allocated (lifetime is tracked by
`ManagedPtrDestructor`). It is not, and structurally cannot be, an entry in
`ExternalReferenceTable`.

`Serializer::ObjectSerializer::VisitExternalPointer` (in
`src/snapshot/serializer.cc`) handles `FunctionTemplateInfo` slots through
`OutputExternalReference`, which calls
`Serializer::EncodeExternalReference`:

    src/snapshot/serializer.cc:375
      ExternalReferenceEncoder::Value Serializer::EncodeExternalReference(
          Address addr) {
        Maybe<ExternalReferenceEncoder::Value> result =
            external_reference_encoder_.TryEncode(addr);
        if (result.IsNothing()) {
          ...
          v8::base::OS::PrintError("Unknown external reference %p.\n", addr_ptr);
          ...
          v8::base::OS::Abort();
        }
        ...
      }

Because the `Managed`'s external pointer is a fresh heap address, `TryEncode`
returns `Nothing` and `OS::Abort()` fires. The deserializer-side comment
that surfaced this is in `src/snapshot/deserializer.cc:256–276`:

    if (IsManagedExternalPointerType(tag)) {
      // This can currently only happen during snapshot stress mode as we
      // cannot normally serialized managed resources. ...
      DCHECK(v8_flags.stress_snapshot);

i.e. V8 explicitly assumes managed external pointers do not appear in a
startup snapshot outside of stress-snapshot tests. Under
`v8_flags.stress_snapshot` the snapshotter "transfers" ownership by
restoring `managed_resource->owning_table_` after writing the slot, knowing
the old isolate is about to be discarded — that is not applicable to a real
startup snapshot loaded into a new isolate.

In 14.7 the same code path encoded two non-managed Foreigns whose addresses
the embedder could (and Deno did, in `OpCtx::external_references()`)
register via `ExternalReferences`. That path no longer exists.

## Upstream has already landed a fix for this

While drafting this note we found that V8 main has already reversed the
serialization regression — Node.js hit the same issue and pushed the fix
upstream. The sequence is:

- [crrev.com/c/7814117] — Joyee Cheung (Igalia / Node.js), 2026-05-04,
  *"[fastapi] Keep paired-Foreign overload layout in non-sandbox builds"*.
  Adds a non-sandbox compile-time fork so non-Chrome embedders keep the
  serializable (`kCFunctionTag`, `kCFunctionInfoTag`) pair. **Abandoned**
  2026-05-12 — Andreas Haas (V8 fastapi owner) preferred a single
  cross-configuration shape and superseded it with the next CL.
- [crrev.com/c/7828135] — Andreas Haas, **merged 2026-05-12**,
  `refs/heads/main@{#107265}`, *"[fastapi] Store v8::CFunction pointer
  directly in FunctionTemplateInfo"*.

The merged CL is exactly the shape we'd want as a backport:

- `CFunctionWithSignatureTag` is removed from `MANAGED_TAG_LIST`.
- A new non-managed `CFunctionTag` is added to `FOREIGN_TAG_LIST`.
- `FunctionTemplate::SetCallHandler` now stores each `v8::CFunction*` as
  a plain `Foreign<kCFunctionTag>` instead of allocating a
  `Managed<CFunctionWithSignature>`:

      i::DirectHandle<i::Foreign> overload =
          i_isolate->factory()->NewForeign<i::kCFunctionTag>(
              reinterpret_cast<i::Address>(c_function));

- `compiler/heap-refs.cc` reads back via `Foreign::foreign_address<kCFunctionTag>`
  and calls `CFunction::GetAddress()` / `GetTypeInfo()` on the pointer.

The CL commit message states the new invariant the embedder owes V8:

> This change relies on the assumption that the CFunction object passed to
> FunctionTemplate::New outlives the FunctionTemplate itself. In practice,
> all embedders — including Chrome — already ensure this by holding the
> CFunction object in a static variable.

The companion change to `src/d8/d8-test.cc` adds `static` to every test
`CFunction` declaration, illustrating what the embedder contract looks
like in practice.

Net effect for Deno: once a V8 branch carrying this CL rolls into this
patch repo (it lands on `main@{#107265}`, post-14.9 branch cut, so it will
arrive with V8 15.0), `FunctionTemplateInfo` becomes serializable again
without a Deno-side workaround. `op_ctx_template`'s `will_snapshot` branch
and `upgrade_snapshotted_ops_with_fast_calls` can be deleted in
`deno_core`.

[crrev.com/c/7814117]: https://chromium-review.googlesource.com/c/v8/v8/+/7814117
[crrev.com/c/7828135]: https://chromium-review.googlesource.com/c/v8/v8/+/7828135

## Why we are not backporting [crrev.com/c/7828135] today

The fix is small (78 / 79 lines, 7 files) and contained, but adopting it
on the 14.9-lkgr-denoland branch isn't a no-op. Three things have to be
true at the same time for the patch to be safe:

1. **rusty_v8 lifetime contract.** The new V8 API requires the
   `v8::CFunction` passed to `FunctionTemplate::NewWithCFunctionOverloads`
   to outlive the returned `FunctionTemplate`. rusty_v8's
   `FunctionTemplateBuilder::build_fast(scope, overloads: &[CFunction])`
   currently takes a borrow and forwards `overloads.as_ptr()` to
   `v8__FunctionTemplate__New` (see
   [rusty_v8/src/template.rs](https://github.com/denoland/rusty_v8/blob/main/src/template.rs)
   and the corresponding C++ shim in
   [rusty_v8/src/binding.cc](https://github.com/denoland/rusty_v8/blob/main/src/binding.cc)).
   Callers that pass a stack-local slice — `builder.build_fast(scope, &[fast_function])`,
   which is what `deno_core`'s `op_ctx_template` already does — would,
   under the new V8 ABI, hand V8 a pointer that becomes dangling the moment
   `build_fast` returns. rusty_v8 needs an internal copy/leak (e.g. into
   an isolate-scoped arena) or a `'static`-bounded overload signature.
2. **deno_core CFunction storage.** `libs/ops/op2/dispatch_fast.rs` emits
   `CFunction::new(Self::#fast_function as _, &CFunctionInfo::new(...))`
   at codegen time. The resulting `CFunction` is stored in the
   `OpDecl::fast_fn` field of a `const`/`static` declaration, so on the
   Rust side the underlying bytes have static lifetime; but it gets
   copied through `op_ctx.decl.fast_fn` and into the stack-local slice at
   the `build_fast` call site, which is the layer that needs the audit
   above. The `CFunctionInfo` reference inside it must also be `'static`
   for the new V8 API to be sound.
3. **14.9-lkgr-denoland is shared.** This branch is consumed by
   `rusty_v8` releases and downstream binaries. Carrying a behavioral
   patch that flips the layout of `FunctionTemplateInfo` overloads on a
   stable branch — without (1) and (2) in place — risks breaking
   embedders that picked up the same rusty_v8 release.

If we want to pull the fix in early, the cheapest path is on V8 15.x as a
fresh `15.0-lkgr-denoland` (post-roll) rather than on 14.9, paired with a
rusty_v8 release that has switched `build_fast` to a static-lifetime
overload list. Until then the Deno embedder workaround already shipping
in [denoland/deno#34226] is the right thing to keep.

## Why a V8-side patch alternative to [crrev.com/c/7828135] is unattractive

For completeness, the patches that were considered before the upstream
fix surfaced:

1. teach the snapshotter to decompose a `Managed<CFunctionWithSignature>`
   back into its `(function_address, type_info_pointer)` pair, serialize
   each via the external reference table, and re-allocate a fresh
   `Managed` on the deserialization side; or
2. add a sanitize-and-restore pass on every `FunctionTemplateInfo` (and
   `FunctionTemplateRareData::c_function_overloads`) so the serializer
   sees an empty overloads array, symmetric to the existing
   `SanitizeIsolateScope` / `RemoveCallbackRedirectionForSerialization`
   pattern.

(1) is a non-trivial behavioral change to V8's sandbox-aware serializer
for a managed-resource type whose lifetime semantics V8 considers private
— it is precisely what the deserializer comment above warns off. (2) is
mostly identical in effect to the Deno-side workaround already shipping;
the embedder must still rebuild fast calls after deserialization, since
neither the function address nor the `CFunctionInfo` pointer survives a
process boundary. Pushing (2) down into V8 would let Deno drop the
`will_snapshot` plumbing but would not remove
`upgrade_snapshotted_ops_with_fast_calls`, and would add Deno-shaped
machinery to V8 core that no other embedder asks for. Both options are
strictly worse than [crrev.com/c/7828135], which is already in V8 main.

The cost of *not* patching V8 today — keeping the workaround in
`deno_core` — is small (see "Startup impact" below) and the workaround
is the right shape: the snapshot bakes the slow function for every op,
and the runtime re-attaches fast paths immediately after deserialization,
before any user JS runs.

[`README.md`]: ../README.md
[denoland/deno#34226]: https://github.com/denoland/deno/pull/34226

## Startup impact estimate

`upgrade_snapshotted_ops_with_fast_calls` runs once per `JsRuntime`
construction, before any user JS executes. For every op declared with a
`fast_fn`, it:

- builds a fresh `FunctionTemplate` via `op_ctx_template` (which calls
  `NewWithCFunctionOverloads`, allocating one `Managed<CFunctionWithSignature>`
  per overload);
- materialises a function via `template.get_function`;
- sets `.name`;
- for class methods, looks up the class from `Deno.core.ops`, walks its
  `prototype`, and writes the new function onto the prototype (instance
  methods) or onto the class function itself (static methods);
- for async ops, calls `Deno.core.setUpAsyncStub` to re-wrap the function.

A typical Deno binary defines a few hundred ops with fast overloads
(`deno_core`, `deno_node`, the runtime ops, the WebAPIs). Per op the cost
is on the order of a few microseconds (a couple of V8 allocations and
property writes). Even at the high end this puts the total upgrade pass at
a few milliseconds of cold-start cost — measurable in microbenchmarks but
well below the GC, parse, and JIT-warmup work that already dominates the
first 50–100 ms of a Deno startup. There is no per-call overhead at steady
state: once the upgrade pass finishes, `Deno.core.ops` looks bit-identical
to a from-scratch (non-snapshot) build.

Conclusion: the startup regression from carrying the workaround vs. having
fast calls baked into the snapshot is negligible.

## Value serializer version drift (15 → 16) is unrelated

The fixture changes in
`libs/core_testing/unit/serialize_deserialize_test.ts` (e.g.
`[255, 15, 34, 0]` → `[255, 16, 34, 0]`) are a side-effect of an unrelated
upstream bump.

`src/objects/value-serializer.cc:74–86` documents the format history. The
relevant change is:

> Version 16: don't truncate JSArrayBuffer's and JSArrayBufferView's lengths
>             and offsets to 32-bit values and write full size_t's instead,
>             make sure deserializer is ready to handle 64-bit values even
>             on 32-bit architectures. Allow serialization of resizable
>             ArrayBuffers with maxByteLength larger than 4GB.

This is purely an `ArrayBuffer`/`ArrayBufferView` widening. It does not
touch fast calls, `FunctionTemplate`, managed external pointers, or the
startup snapshot format (which is independent of the structured-clone /
`ValueSerializer` wire format). Deno's fixture update is the only adjustment
required.

## Recommended embedder-side follow-up

The Deno PR's `upgrade_snapshotted_ops_with_fast_calls` has several
fall-through paths that silently skip work when an expected JS shape is
missing:

    let class_fn =
        class_fn_val.and_then(|v| v8::Local::<v8::Function>::try_from(v).ok());
    let prototype = class_fn.and_then(|f| {
        let p = f.get(scope, prototype_key.into())?;
        v8::Local::<v8::Object>::try_from(p).ok()
    });
    ...
    let Some(prototype) = prototype else { continue };
    let Some(class_fn)  = class_fn  else { continue };
    let _ = set_up_async_stub_fn.call(...);   // result discarded

For a Deno-built snapshot the invariant is that every `decl` in
`op_method_decls` corresponds to a function present at `Deno.core.ops[name]`
whose `prototype` is an `Object`, and that `setUpAsyncStub` succeeds. Any
violation means the snapshot is inconsistent with the binary that's about
to load it, and the fast-call upgrade would silently degrade ops to their
slow form — exactly the kind of regression that's invisible in functional
tests and only surfaces as a perf cliff later.

Recommend turning these into hard errors (panic / `Result::expect`) rather
than `continue` / `let _ = `, scoped to the snapshot path. This is the
piece of follow-up that should land in `deno_core`, not in this V8 patch
repo.

## Decision

No `denoland/v8` patch on the 14.9 branch. The upstream consolidation into
`Managed<CFunctionWithSignature>` was intentional (it fixed a real
`(function, signature)` load race, `crbug.com/492077213`), and the
snapshotter's refusal to encode managed external pointers is a
sandbox-correctness invariant we should not unilaterally relax.

The Deno embedder-side workaround in [denoland/deno#34226] is the right
shape for V8 14.9. The follow-up work falls in two places:

- **`deno_core`**: harden the silent fallbacks in
  `upgrade_snapshotted_ops_with_fast_calls`. Three `continue`-on-`None`
  paths and one `let _ = …call(…)` should become hard errors for a
  Deno-built snapshot.
- **`denoland/v8` next roll**: when V8 15.x rolls in,
  [crrev.com/c/7828135] removes the `Managed` wrapping and restores
  `FunctionTemplateInfo` serialization. At that point the embedder-side
  workaround can be deleted in `deno_core`, conditional on `rusty_v8`
  having first switched its `build_fast` overload-list signature to one
  that satisfies the new V8 lifetime invariant (the C++-side `v8::CFunction`
  must outlive the resulting `FunctionTemplate`).
