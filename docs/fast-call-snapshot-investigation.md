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
returns true for. The motivation for the consolidation was atomicity:
upstream commit f5ac1a82 ([fastapi] Read c-function and signature atomically,
crbug 492077213) notes that loading the c-function and the signature
separately allowed a raceful FixedArray mutation to produce a mismatched
(function, signature) pair, and the new single-`Managed` shape closes that
gap.

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

## Why a `denoland/v8` patch is not appropriate

The repo's stated charter ([`README.md`]) is that floated patches should
"help accommodate build system differences between Chromium and rusty_v8"
and avoid functional changes to V8 internals. The functional patches we do
carry (`Apple Silicon mprotect`, `Windows ptrcmp padding`,
`V8_TLS_USED_IN_LIBRARY`) all target host-specific build/runtime invariants
that V8 upstream does not — and likely will not — accept as merge-able
changes; none of them alter snapshot, GC, or sandboxing internals.

A V8 patch that would actually preserve fast-call data in a Deno snapshot
would have to either:

1. teach the snapshotter to decompose a `Managed<CFunctionWithSignature>`
   back into its `(function_address, type_info_pointer)` pair, serialize
   each via the external reference table, and re-allocate a fresh
   `Managed` on the deserialization side; or
2. add a sanitize-and-restore pass on every `FunctionTemplateInfo` (and
   `FunctionTemplateRareData::c_function_overloads`) so the serializer sees
   an empty overloads array, symmetric to the existing
   `SanitizeIsolateScope` / `RemoveCallbackRedirectionForSerialization`
   pattern.

(1) is a non-trivial behavioral change to V8's sandbox-aware serializer for
a managed-resource type whose lifetime semantics V8 considers private — it
is precisely what the deserializer comment above is warning off. (2) is
mostly identical in effect to the Deno-side workaround already shipping;
the embedder must still rebuild fast calls after deserialization, since
neither the function address nor the `CFunctionInfo` pointer survives a
process boundary. Pushing (2) down into V8 would let Deno drop the
`will_snapshot` plumbing but would not remove
`upgrade_snapshotted_ops_with_fast_calls`, and would add Deno-shaped
machinery to V8 core that no other embedder asks for.

The cost of *not* patching V8 — keeping the workaround in `deno_core` —
is small (see "Startup impact" below) and the workaround is the right
shape: the snapshot bakes the slow function for every op, and the runtime
re-attaches fast paths immediately after deserialization, before any user
JS runs.

[`README.md`]: ../README.md

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

No `denoland/v8` patch. The upstream consolidation into
`Managed<CFunctionWithSignature>` is intentional (fixes a real race), and
the snapshotter's refusal to encode managed external pointers is a
sandbox-correctness invariant we should not unilaterally relax. The Deno
embedder-side workaround is the right shape; the only remaining work is to
make its post-deserialization upgrade pass invariant-strict rather than
best-effort.
