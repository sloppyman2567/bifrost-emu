# Vulkan Migration Plan — Generated Marshalling (Phases A → C → B)

> **COMPLETE (2026-08-24):** all phases shipped — graphics-pipeline stage
> (`vkCreateGraphicsPipelines`/`vkCreateShaderModule`/descriptors),
> vkMapMemory guest-window bounce, per-image framebuffers, and the
> `test_vulkan_swapchain.elf` triangle demo (76 checks, JIT + interp).
> See docs/CHANGELOG.md (1.5.3-alpha … 1.5.5-alpha entries). The rest of
> this file is the historical working plan.
>
> Status at completion: **A1 ✅ · A2 ✅ · C1 ✅ · C3 ✅ · B batch 1 ✅**.

## Current state (post milestone-2, commit 82f8147)

- vk.xml machinery live: `tools/opgen/vkxml.py` (parser + validated
  layout engine, 1698 structs byte-checked vs vendored vulkan_core.h),
  `vkmarshalgen.py` → `include/opgen_vkmarshal.hpp` (1543 struct
  descriptors / 1479 chainable / 1227 sType entries / 28 command
  plans), `vkxmlcheck.py` (`make vkxml-check`, errors=0 gate,
  warnings 20 → 6).
- Runtime: `Policy::VK_CMD_DEEP` two-pass staging marshal in
  display_thunk.cpp (+ new `VK_CMD_DEEP_OUT` for enumerations).
- Converted: SetViewport/SetScissor/PipelineBarrier/clears/copies/
  ExecuteCommands/WaitEvents/ResetFences + A2 set below.

## Done in this session

### A1 — generated pNext chains ✅
- `vkxml.py`: full VkStructureType value extraction (core `<enums>`,
  `<extension number=M>` blocks with extnumber-default-M rule,
  `<feature>` promotion blocks, alias chains). Validated: ALL 1250
  constants in the vendored vulkan_core.h match — zero mismatches.
  (Two old hand-table entries were WRONG — INHERITANCE_INFO 11→41,
  DEVICE_GROUP_CMD_BEGIN 1000060001→1000060004 — harmless, never chained.)
- `vkmarshalgen.py`: descriptors emitted for plan closure ∪ EVERY
  chainable struct; structs whose nested refs have no computable layout
  are PRUNED (LUNARG funcptr case) so chains truncate at them safely;
  `VkFieldDesc.elem` gains VKM_PNEXT (0x80) flag marking pNext links;
  sorted `kVkStypeIndex[]` + binary-search `vk_find_struct_by_stype()`.
- Runtime: `vk_marshal_pnext_chain` reimplemented over the generated map
  (16-entry hand sType table DELETED); writeback now skips the pNext
  link field [8..16) — the staged HOST pointer must never land in guest
  memory (latent guest-chain-corruption fix); one-shot unknown-sType
  diagnostic. VK_CMD_DEEP fill/size passes walk guest chains for
  VKM_PNEXT fields (input-only).
- CREATE_DEVICE / Properties2 / Features2 / BEGIN_COMMAND_BUFFER arms
  upgraded implicitly (same walker).

### A2 — OUT plans ✅ (scoped)
- `VkPlanRef` gains `out` flag + `elem_size`: auto-derived direction —
  count sibling is a POINTER → out=1 enumeration; non-const data ptr →
  out=2 copyback-only; scalar/enum/bitmask arrays stage as raw bytes.
- Runtime: enumeration staging = 4-byte count bounce (allocated AFTER
  reserve — hard contract #2) + array staging; post-call copyback writes
  min(staged, actual) elements AND the actual count to guest memory.
  Failed plans restore original guest count pointers before the generic
  path runs.
- Migrated rows → VK_CMD_DEEP_OUT: EnumeratePhysicalDevices,
  GetSwapchainImagesKHR, SurfaceFormatsKHR, SurfacePresentModesKHR,
  QueueFamilyProperties, GetQueryPoolResults. → VK_CMD_DEEP (pure IN):
  FreeCommandBuffers, FreeDescriptorSets, CmdBindDescriptorSets,
  CmdBindVertexBuffers, CmdUpdateBuffer, CmdPushConstants.
- NOT migrated (deliberate): Enumerate{Instance,Device}Extension/Layer
  properties stay on their hand arms (RT-extension filter lives there);
  AllocateCommandBuffers needs a struct-nested count source.

### C1 — modern rows ✅ partial
- Added: vkCmdEndRendering(+KHR), DrawIndirect/DrawIndexedIndirect,
  ResetQueryPool, SignalSemaphore, GetSemaphoreCounterValue,
  AcquireNextImage2KHR (all plain VULKAN), BindVertexBuffers2(+KHR),
  SetViewportWithCount/SetScissorWithCount(+KHR) (auto VK_CMD_DEEP).
  Table 1042 → 1056 symbols.
- DEFERRED (nested-array-in-first-arg marshal = command-level recursive
  plans, see Open work): vkCmdBeginRendering, vkCmdPipelineBarrier2,
  vkQueueSubmit2, vkCmdPushDescriptorSetKHR, descriptor update templates.

### C3 — declarative extension policy ✅
- VK_EXT_descriptor_buffer added to the hidden list (advertise ⟺
  implemented). Update templates are core-1.1-promoted — hiding from
  enumeration achieves nothing; left alone.

## Deferred decisions (recorded, do not silently re-scope)

- **A3 nullify option**: no consumer exists until Phase B pipelines
  migration — implement WITH that migration, not before (dead config).
- **C2 vkCreateAndroidSurfaceKHR**: NOT a small arm. An Android guest
  enables VK_KHR_android_surface at INSTANCE creation; the host needs
  Wayland/XCB variants or SDL_Vulkan_CreateSurface instead. Requires
  instance-extension rewriting in the CREATE_INSTANCE arm first, plus an
  Android-Vulkan guest test to validate. Revisit as its own milestone.

## Phase B progress

**Batch 1 DONE (2026-08-24)**: create-style plans landed — new ref
roles out=3 NULLIFY (implements A3), out=4 SINGLE_STRUCT_IN (stage one
struct recursively via its descriptor), out=5 OUT_HANDLE (8-byte bounce +
writeback). latexmath len expressions resolve their member name and
stage byte-granular (ShaderModuleCreateInfo.pCode). Migrated:
vkCreateShaderModule / PipelineLayout / DescriptorPool / Framebuffer /
vkCmdBeginRenderPass — five hand arms + five H structs DELETED, rows on
VK_CMD_DEEP. Also fixed phantom C1 alias names (WithCount/BindVB2 are
EXT aliases, not KHR). Gates re-run: 207/207, RADV ×3 each.

**Batch 2 DONE (2026-08-24)**: vkCreateRenderPass + 
vkCreateDescriptorSetLayout migrated off their hand arms — the
attachment/subpass/reference/preserve trees and the
pImmutableSamplers handle arrays are consumed wholesale by generated
recursion. Seven more H structs deleted. Found+fixed en route:
scalar-typed pointer fields (uint32_t* pPreserveAttachments) staged
with elem_size 8 instead of 4 ('scalar:N' never decoded in field
emission). Gates re-run: 207/207, RADV ×3 each.

**Batch 3 DONE (2026-08-24)**: vkCreateGraphicsPipelines +
vkCreateComputePipelines migrated — the full pipeline state tree
(pName STRINGS via new VKM_STR flag, specialization blobs, vertex
input/viewport/multisample/color-blend/dynamic-state sub-structs) plus
OUT handle arrays with register counts (STRUCT_IN/OUT_HANDLE roles
unified over count_arg; 0xFF sentinel = single). Two more hand arms +
14 H structs deleted (~110 lines). Gates re-run: 207/207, RADV ×3 each.

**Batch 4 DONE (2026-08-24)**: SUBMIT + SUBMIT2(+KHR) + 
UPDATE_DESCRIPTOR_SETS auto-derived plans; ALLOC_DESCRIPTOR_SETS via
count_arg=0xFE/aux (count member inside staged struct). Two bugs found:
(A) type_size-before-layouts classification staged nested struct arrays
as raw bytes (untranslated interior pointers → RADV SIGSEGV) — layouts
check now first; (B) incomplete failure sweep in b2/b3 left stale done
bits (fixed by batch-4 unconditional sweep; those intermediates can
crash intermittently). PCWFC push moved to a name-based pre-call hook.
**Batch 5 DONE (2026-08-24) — Phase B COMPLETE**: vkCreateInstance /
vkCreateDevice / vkAllocateCommandBuffers migrated (VKM_STRARR string-
array staging added; out=2 now byte-vs-element aware; SYNC_PULL arm
dissolved into plans + a name-gated post-call pull hook). Four more
arms + five H structs deleted.

Permanently hand-coded (declared): PRESENT (field-level OUT pResults +
present behavior), memory family (MAP/UNMAP/FLUSH/INVALIDATE bounce +
PCWFC bookkeeping), GET_PROC, extension filtering, debug-messenger
interception, BEGIN_COMMAND_BUFFER (small frozen-shape arm).

## Hard contracts (violating any = known crash class)

1. VkStage arena lives at DISPATCH FUNCTION SCOPE until after the host
   call (scoped-inside-if SIGSEGV'd mambo_vulkan).
2. Two-pass staging only: dry size pass → single reserve() → fill.
   NO pointer handed out before final reserve — including the A2 count
   bounces (they allocate in the fill phase, counts first).
3. Caps mandatory: ≤1024 elems/array, ≤4 MiB staging, depth ≤4,
   ≤8 pNext nodes; failure falls back to generic bounce (never worse
   than before) AND restores rewritten guest pointers first.
4. Guest callbacks never cross to host code (debug-messenger precedent:
   intercept by name, fake handle).
5. Policy enum is row-derived: adding VALID_POLICY entries does nothing
   until a spec row uses it; regen opgen_thunk.hpp after spec edits.
6. Gates per phase: build 0 warnings, `opgen-thunk-check`, `vkxml-check`
   errors=0, quick suite 207/207, swapchain + mambo rc=0 ×3 on RADV.
7. Generated chain walking: size pass and fill pass MUST mirror each
   other allocation-for-allocation (two-pass contract); unknown sType →
   NULL link + one-shot diagnostic, never garbage staging.

## Verification protocol (every phase)

```
make -j$(nproc)                      # 0 warnings
make opgen-thunk-check && make vkxml-check   # up-to-date / errors=0
./scripts/run_tests.sh --quick       # 207/207
cd ctest_real && DISPLAY=:0 timeout -s KILL 30 ../bifrost-emu \
    ./test_vulkan_swapchain.elf      # rc=0 ×3
DISPLAY=:0 timeout -s KILL 30 ../bifrost-emu \
    ./test_mambo_vulkan.elf          # rc=0 ×3
```

All gates PASS for A1+A2+C1+C3 (2026-08-24).

## Open decisions (user answered / defaults)

- Order: A → C → B (user accepted recommendation).
- Descriptor update templates: hide extension initially ✓ (moot — core-
  promoted; documented above).
- PRESENT/SUBMIT arms migrate LAST ✓.
