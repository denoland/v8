// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/startup-deserializer.h"

#include "src/api/api.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/flush-instruction-cache.h"
#include "src/execution/v8threads.h"
#include "src/handles/handles-inl.h"
#include "src/heap/paged-spaces-inl.h"
#include "src/logging/counters-scopes.h"
#include "src/logging/log.h"
#include "src/objects/oddball.h"
#include "src/objects/string-table.h"
#include "src/roots/roots-inl.h"

namespace v8 {
namespace internal {

// Defined in deserializer.cc (heap-image bake/restore + DENO_HEAP_DUMP).
void EmitHeapDump(const char* label);
bool HeapImageVerbose();
void BakeHeapImageRoots(Isolate* isolate, const char* path);
void RestoreHeapImageRoots(Isolate* isolate, const char* path);
void BakeHeapImagePages(Isolate* isolate, const char* path);

void StartupDeserializer::DeserializeIntoIsolate() {
  // Heap image: warm restore (experimental, gated): replay the baked isolate heap
  // image instead of running the object-graph deserializer. First milestone:
  // re-allocate + memcpy objects and verify offsets match the bake.
  if (const char* base = getenv("DENO_HEAP_IMAGE_RESTORE")) {
    char pages_path[1024], roots_path[1024], ep1[1024], ep2[1024];
    snprintf(pages_path, sizeof(pages_path), "%s.pages", base);
    snprintf(roots_path, sizeof(roots_path), "%s.roots", base);
    snprintf(ep1, sizeof(ep1), "%s.shared.extp", base);
    snprintf(ep2, sizeof(ep2), "%s.isolate.extp", base);
    base::ElapsedTimer t;
    t.Start();
    // Whole-page adoption (shared-heap deser was skipped; these pages hold both
    // shared + startup objects).
    int64_t n = RestoreHeapImagePages(pages_path);
    RestoreHeapImageRoots(isolate(), roots_path);
    RestoreHeapImageExternalPointers(ep1);  // shared-phase external pointers
    RestoreHeapImageExternalPointers(ep2);  // startup-phase external pointers
    // Raw memcpy restore of the off-heap string table (needs pinned seed).
    char stra_path[1024];
    snprintf(stra_path, sizeof(stra_path), "%s.strtabraw", base);
    isolate()->string_table()->RestoreHeapImage(stra_path);
    // Start the compilation cache empty (safe — V8 recompiles on demand). The
    // cache isn't part of the baked image, so this avoids any stale entry.
    isolate()->compilation_cache()->Clear();
    double ms = t.Elapsed().InMillisecondsF();
    if (HeapImageVerbose())
      fprintf(stderr, "[heap-image] startup replay: %lld pages in %.3f ms\n",
              (long long)n, ms);
    return;
  }
  // Flush objects accumulated by the (already-run) shared-heap deserializer
  // under their own label, so the "isolate" image contains startup objects
  // only. The shared-heap deser runs normally on restore, so its objects must
  // not be in the replay set.
  EmitHeapDump("shared");
  TRACE_EVENT0("v8", "V8.DeserializeIsolate");
  RCS_SCOPE(isolate(), RuntimeCallCounterId::kDeserializeIsolate);
  base::ElapsedTimer timer;
  if (V8_UNLIKELY(v8_flags.profile_deserialization)) timer.Start();
  NestedTimedHistogramScope histogram_timer(
      isolate()->counters()->snapshot_deserialize_isolate());
  HandleScope scope(isolate());

  // No active threads.
  DCHECK_NULL(isolate()->thread_manager()->FirstThreadStateInUse());
  // No active handles.
  DCHECK(isolate()->handle_scope_implementer()->blocks()->empty());
  // Startup object cache is not yet populated.
  DCHECK(isolate()->startup_object_cache()->empty());
  // Builtins are not yet created.
  DCHECK(!isolate()->builtins()->is_initialized());

  {
    DeserializeAndCheckExternalReferenceTable();

    isolate()->heap()->IterateSmiRoots(this);
    isolate()->heap()->IterateRoots(
        this,
        base::EnumSet<SkipRoot>{SkipRoot::kUnserializable, SkipRoot::kWeak,
                                SkipRoot::kTracedHandles});
    IterateStartupObjectCache(isolate(), this);

    isolate()->heap()->IterateWeakRoots(
        this, base::EnumSet<SkipRoot>{SkipRoot::kUnserializable});
    DeserializeDeferredObjects();
    if (USE_SIMULATOR_BOOL) {
      for (DirectHandle<AccessorInfo> info : accessor_infos()) {
        info->RestoreCallbackRedirectionAfterDeserialization(isolate());
      }
      for (DirectHandle<InterceptorInfo> info : interceptor_infos()) {
        info->RestoreCallbackRedirectionAfterDeserialization(isolate());
      }
      for (DirectHandle<FunctionTemplateInfo> info :
           function_template_infos()) {
        info->RestoreCallbackRedirectionAfterDeserialization(isolate());
      }
    }
    // Flush the instruction cache for the entire code-space. Must happen after
    // builtins deserialization.
    FlushICache();
  }

  isolate()->heap()->set_native_contexts_list(
      ReadOnlyRoots(isolate()).undefined_value());
  // The allocation site list is build during root iteration, but if no sites
  // were encountered then it needs to be initialized to undefined.
  if (isolate()->heap()->allocation_sites_list() == Smi::zero()) {
    isolate()->heap()->set_allocation_sites_list(
        ReadOnlyRoots(isolate()).undefined_value());
  }
  isolate()->heap()->set_dirty_js_finalization_registries_list(
      ReadOnlyRoots(isolate()).undefined_value());
  isolate()->heap()->set_dirty_js_finalization_registries_list_tail(
      ReadOnlyRoots(isolate()).undefined_value());

  isolate()->builtins()->MarkInitialized();

  LogNewMapEvents();
  WeakenDescriptorArrays();

  if (should_rehash()) {
    // Hash seed was initialized in ReadOnlyDeserializer.
    Rehash();
  }

  if (V8_UNLIKELY(v8_flags.profile_deserialization)) {
    // ATTENTION: The Memory.json benchmark greps for this exact output. Do not
    // change it without also updating Memory.json.
    const size_t bytes = source()->length();
    const double ms = timer.Elapsed().InMillisecondsF();
    PrintF("[Deserializing isolate (%zu bytes) took %0.3f ms]\n", bytes, ms);
  }

  EmitHeapDump("isolate");
  if (const char* base = getenv("DENO_HEAP_IMAGE_BAKE")) {
    char roots_path[1024], pages_path[1024];
    snprintf(roots_path, sizeof(roots_path), "%s.roots", base);
    BakeHeapImageRoots(isolate(), roots_path);
    snprintf(pages_path, sizeof(pages_path), "%s.pages", base);
    BakeHeapImagePages(isolate(), pages_path);
    // Off-heap string table baked as a raw blob (memcpy restore).
    char stra_path[1024];
    snprintf(stra_path, sizeof(stra_path), "%s.strtabraw", base);
    isolate()->string_table()->BakeHeapImage(stra_path);
  }
}

void StartupDeserializer::DeserializeAndCheckExternalReferenceTable() {
  // Verify that any external reference entries that were deduplicated in the
  // serializer are also deduplicated in this isolate.
  ExternalReferenceTable* table = isolate()->external_reference_table();
  while (true) {
    uint32_t index = source()->GetUint30();
    if (index == ExternalReferenceTable::kSizeIsolateIndependent) break;
    uint32_t encoded_index = source()->GetUint30();
    CHECK_EQ(table->address(index), table->address(encoded_index));
  }
}

void StartupDeserializer::LogNewMapEvents() {
  if (v8_flags.log_maps) LOG(isolate(), LogAllMaps());
}

void StartupDeserializer::FlushICache() {
  DCHECK(!deserializing_user_code());
  // The entire isolate is newly deserialized. Simply flush all code pages.
  for (NormalPage* p : *isolate()->heap()->code_space()) {
    FlushInstructionCache(p->area_start(), p->area_end() - p->area_start());
  }
}

}  // namespace internal
}  // namespace v8
