# Texture coordinate storage

Donut supports `TexCoordFormat::Float32` (the default), `TexCoordFormat::Float16`, and `TexCoordFormat::Unorm16` for GPU texture coordinates. `Float16` stores each component as an IEEE half float. `Unorm16` automatically normalizes source UV bounds, stores two unsigned normalized 16-bit components, and reconstructs the original UVs with generated FP32 scale and offset. Shader interpolation and material evaluation still use `float2`.

To use normalized UNORM16 for subsequent scene imports, set the default before loading:

```cpp
scene.SetDefaultTexCoordFormat(donut::engine::TexCoordFormat::Unorm16);
scene.Load("scene.json");
```

This also applies to `LoadWithThreadPool`. Changing the default does not convert existing buffer groups. To override an imported or manually created group, assign its format before its first GPU upload through `FinishedLoading`, `RefreshBuffers`, or `Refresh`:

```cpp
mesh->buffers->texCoordFormat = donut::engine::TexCoordFormat::Float32;
```

All meshes sharing a `BufferGroup` share this setting. Both UV streams use the same format. CPU-side `texcoord1Data` and `texcoord2Data` remain `std::vector<dm::float2>`; packing happens during upload. Do not change `texCoordFormat` after the vertex buffer has been created.

The requested format is a GPU storage policy, independent of the glTF accessor encoding. Direct importer users can select it with `GltfImporter(fs, sceneTypeFactory, texCoordFormat)`; omitting the format keeps FP32. The importer configuration is immutable. `SetDefaultTexCoordFormat` replaces the scene's importer, and queued model loads retain the importer selected when they were submitted.

| GPU format | Bytes per UV pair | Input assembler format | Decode metadata |
| --- | ---: | --- | --- |
| `Float32` | 8 | `RG32_FLOAT` | None |
| `Float16` | 4 | `RG16_FLOAT` | None |
| `Unorm16` | 4 | `RG16_UNORM` | FP32 U/V scale and offset, 16 bytes per UV set |

Both 16-bit formats halve the vertex UV payload. Each attribute range remains aligned to 16 bytes, so total allocation savings depend on vertex count and the other attributes. Use `BufferGroup::getTexCoordStride()` for CPU offset calculations. Skinned buffers inherit the prototype's actual format and decode metadata, and skinning copies the stored UV bits unchanged.

For FP16, a nonfinite component or a component with absolute value greater than 65504 causes upload to log a warning and fall back to FP32 for the entire group. This range check does not guarantee sufficient precision: negative and tiled UVs are supported, but large UV magnitudes retain less fractional detail.

## Automatic UNORM16 bounds

The same source asset can use any format; no additional authored UV transforms or asset metadata are required. During upload, Donut computes each UV set's minimum and range, then packs each component as follows:

```text
scale = maximumUV - minimumUV
offset = minimumUV
packedUV = round(clamp((sourceUV - offset) / scale, 0, 1) * 65535)
decodedUV = (packedUV / 65535) * scale + offset
```

A zero-width axis uses scale zero and packed value zero, reconstructing its constant offset without division by zero. Negative, tiled, and large-offset UVs do not need to fit into `[0,1]` in the asset. For example, U bounds `[3000,3002]` produce scale `2` and offset `3000`; only the packed coordinates occupy `[0,1]`.

Bounds are computed independently per mesh vertex range and UV set. Disjoint meshes sharing one `BufferGroup` retain separate bounds. Meshes with overlapping vertex ranges share merged bounds so each stored vertex has a consistent decode. Unreferenced vertices receive separate ranges. `BufferGroup::texCoordDecodeRanges` retains the generated ranges, and `getTexCoordDecodeRange(vertexIndex)` looks them up using a buffer-relative vertex index. Skinned buffers rebase these ranges to their copied vertices.

Buffer preparation also assigns each geometry a decode-range index hint. The hinted lookup checks the index and vertex containment against the current, sorted, non-overlapping ranges before using it, and falls back to the general lookup on a mismatch. It reads live scale/offset values, so rebuilding or editing decode metadata cannot leave stale copied values in the hint. Manually created geometry can leave the hint unset.

The FP32 scale and offset are shared metadata, not part of each vertex. `GeometryData` contains a `float4` for each UV set (`scale.xy`, `offset.zw`), adding 32 bytes to each geometry record. The raster passes pass the first UV set's decode in draw constants. Thus actual memory savings include this metadata overhead as well as vertex-buffer alignment.

UNORM16 falls back to FP32 for the entire group if either stream contains nonfinite values or finite FP32 decode bounds cannot be represented safely. Inspect `texCoordFormat` after upload for the actual format. Outliers enlarge the bounds and reduce precision for other vertices in the same range. The ideal per-component quantization error is at most `scale / (2 * 65535)`, plus FP32 reconstruction rounding; large offsets still face ordinary FP32 precision limits. There is no automatic visual-quality threshold. Keep groups in FP32 when texture detail or alpha-tested edges require it.

## Renderer and application integration

Donut's depth, forward, and GBuffer passes support all three formats through input assembler and buffer-load paths. The material ID pass inherits the GBuffer buffer-load support. Pass subclasses can override `CreateInputLayout(vertexShader, params, texCoordFormat)` to supply matching layouts for each format. The original two-argument overload remains available for existing FP32 implementations. UNORM16 input assembler attributes already arrive as normalized floats; apply the generated scale and offset once with `DecodeTexCoord`.

By default, raster passes select the `DECODE_TEXCOORD=0` variant of `input_assembler` for FP32/FP16 inputs, avoiding decode constants and per-draw push updates. UNORM16 uses `DECODE_TEXCOORD=1` and keeps its decode constants. The define selects the shared `DecodeTexCoord<false>` or `DecodeTexCoord<true>` HLSL template specialization; the compiler removes the disabled decode without introducing a runtime branch. Calls without a template argument retain decoding by default. `RenderView` can reuse unchanged UNORM constants between draws, invalidating them after every graphics-state change and at each recording scope boundary. Direct callers of the pass methods remain uncached and do not need cache-management notifications. Buffer-load rendering still supplies its full per-draw constants.

Shaders including `donut/shaders/bindless.h` require DXC with HLSL 2021 (`-HV 2021`). Donut's shader and test builds enable this explicitly for DX12 and Vulkan. `DONUT_WITH_DX11` defaults to `OFF`; an existing renderer configuration with DX11 enabled stops at configuration time with instructions to disable it. FXC/DX11 shader compilation is no longer supported by these templates.

`DepthPass`, `ForwardShadingPass`, and `GBufferFillPass` are ordinary classes. Their `CreateParameters::enableTexCoordOptimizations` setting defaults to `true`. With input-assembler rendering, this enables the floating-point shader variant, shared input binding sets, and UNORM constant caching. This initialization setting does not select a mesh's UV format; both enabled and disabled modes support FP32, FP16, and UNORM16. Buffer-load rendering keeps its full constants and per-buffer bindings.

Custom vertex shaders, input layouts, or input binding factories must set `enableTexCoordOptimizations = false` unless they preserve all of the built-in input assumptions. Disable it as well if another shader stage or custom draw code consumes the input push constants. The disabled path retains the existing virtual input factories, passes actual buffer groups to binding-set factories on cache misses, and writes the full input constants for every draw. Compatible pixel-shader customization can keep the optimizations enabled. Donut does not automatically detect incompatible overrides; this is an explicit initialization contract.

```cpp
class MyDepthPass : public donut::render::DepthPass
{
public:
    using DepthPass::DepthPass;

protected:
    nvrhi::BindingSetHandle CreateInputBindingSet(
        const donut::engine::BufferGroup* buffers) override;
};

MyDepthPass::CreateParameters params;
params.enableTexCoordOptimizations = false;
MyDepthPass::Context context;
```

A custom pass can enforce this setting in its `Init` override before calling the base implementation. No separate Custom pass class or C++ policy template is required. The optimized shader's availability selects the fast path; if it cannot load, the pass uses the generic input path. Reinitialization clears cached pipelines and input bindings so changing the setting does not reuse incompatible objects. Custom shader loaders selecting the built-in `input_assembler` entry must include the `DECODE_TEXCOORD` permutation alongside any other shader defines.

Application-specific ray tracing shaders and custom vertex-buffer readers require a separate migration before enabling a 16-bit format. `GeometryData::texCoordFormat` records the actual encoding (`0` for FP32, `1` for FP16, `2` for UNORM16). Include `donut/shaders/bindless.h` and use its shared geometry loader after checking for absent attributes:

```hlsl
float2 LoadGeometryUV(ByteAddressBuffer vertexBuffer, GeometryData geometry, uint vertexIndex)
{
    if (geometry.texCoord1Offset == ~0u)
        return float2(0, 0);
    return LoadGeometryTexCoord(vertexBuffer, geometry, vertexIndex, 0);
}
```

Here `vertexBuffer` is the buffer selected by `geometry.vertexBufferIndex`, and `vertexIndex` is relative to the geometry: its byte offset already includes mesh and geometry offsets. Pass `1` as the final argument for the second UV set, guarding `texCoord2Offset` in the same way. Alternatively, call `LoadTexCoord(buffer, offset, vertexIndex, format, scaleBias)` after checking that the offset is not `~0u`. The older four-argument `LoadTexCoord` returns normalized `[0,1]` coordinates for UNORM16 and therefore is insufficient to reconstruct source UVs. Use `GetTexCoordStride(geometry.texCoordFormat)` for other shader offset calculations. The legacy `c_SizeOfTexcoord` name is preserved at 8 bytes per FP32 UV pair; `c_SizeOfTexcoord16` is 4 bytes per FP16 or UNORM16 UV pair.

The decode is affine, so it can be applied before interpolation or after barycentric interpolation when all vertices share a decode range. UV-dependent material operations must use decoded coordinates. When using explicit gradients computed from packed coordinates, multiply those gradients by the decode scale.

Rebuild Donut and application shader binaries together. Relative to the original FP32-only implementation, `GeometryData` grows from 64 to 96 bytes, and the depth, forward, and GBuffer decode push-constant structures are now 48 bytes. The optimized floating-point IA shaders do not consume these constants. The geometry and decode ABI changes also affect applications that keep using FP32; stale compiled shaders and hard-coded structure strides are incompatible. The storage paths use ordinary 32-bit shader operations for decoding and do not require native 16-bit shader arithmetic.

## Tests

With `DONUT_WITH_UNIT_TESTS=ON`, build `donut_shaders` and `donut_all_tests`. `test_texcoords` runs CPU layout checks by default. Supply the compiled shader directories to run the GPU tests:

```text
test_texcoords -dx12 --gpu <Donut shaders>/dxil
test_texcoord_raster -dx12 --gpu <Donut shaders>/dxil --test-shaders <Donut test build>/shaders/dxil
```

Use `-vk` and `spirv` for Vulkan. GPU tests require a device for the requested API and enable NVRHI validation. Add `--debug-runtime` to request the API's native validation runtime; on Vulkan this also requires and verifies an active `VK_LAYER_KHRONOS_validation` layer. Tests fail on logged errors, including resource teardown errors. Since the device manager's legacy Vulkan debug-report callback logs errors as warnings, the test logger also fails on unclassified messages from that callback. Only its exact known unused-vertex-attribute warning at location 1 is accepted. The debug-utils callback preserves severity and needs no such classification. All messages are printed; `test_gpu_log` checks this classification without a GPU. For the full native Vulkan raster suite, set `VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT=false` in the test process environment. This keeps all repeated warnings visible and prevents the validation layer from changing their text when its duplicate-message limit is reached.

The raster test compiles and exercises pass subclasses with custom input and pixel shaders, both input-layout overloads, and input binding factories. It also checks a custom pass that disables UV optimizations in its `Init` override, so callers can use the default creation parameters safely.

The upload test checks all three formats, both UV streams, odd counts, large offsets, negative/tiled coordinates, zero-width bounds, shared/overlapping mesh ranges, FP32 fallback, and skinning. CPU checks cover missing/stale hints and live metadata changes. The raster test checks enabled and disabled texture-coordinate optimizations through input-assembler and buffer-load rendering across depth, forward, and GBuffer passes: mixed formats, changing/equal decode values, state resets, repeated views and command lists, and manual pass callers. It checks actual pipeline push-constant bindings and a custom Depth vertex shader and input binding factory. Custom Forward and GBuffer tests verify that overridden factories receive real buffer groups and that draws use the returned binding layouts and sets. It also observes material constants across buffer switches, compatible framebuffer changes, and a material-cache clear within a view. GBuffer checks UV output with both motion-vector variants. Focused lifecycle cases render after binding-cache resets and after reinitialization with the optimizations disabled and enabled again. Without GPU arguments, CTest marks the raster test as skipped.
