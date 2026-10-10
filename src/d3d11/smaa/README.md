# SMAA 1x, vendored

`SMAA.hlsl`, `Textures/AreaTex.h` and `Textures/SearchTex.h` are the official
SMAA reference implementation by Jorge Jimenez, Jose I. Echevarria, Belen
Masia, Fernando Navarro and Diego Gutierrez, **unmodified**, from
<https://github.com/iryoku/smaa> at commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d`.

Files as vendored (SHA-256 in parentheses):

| File | Bytes | SHA-256 |
|---|---|---|
| `SMAA.hlsl` | 55806 | 4DB53D92EF0B45661F8450303589FFABCE93C352CAE4282A6F32DD2B80EFBEB9 |
| `Textures/AreaTex.h` | 1166747 (179200 bytes of texture, as hex text) | 1933BF43CE86BA71F7ED5F0FA94B148610C634DC3395B9BC67A817D51EE73AF2 |
| `Textures/SearchTex.h` | 8610 (1024 bytes of texture, as hex text) | 1FAC315AB5F87B60083C9B8387AF17F056204D3D1BF21500F2ACC5B3E8C1A37A |
| `LICENSE.txt` | 1470 | 2B848B67BA50AC40B53E11CF1CCDAEE2A190CA83B4652CBA695A5F1CE81F52AE |

MIT licensed (`LICENSE.txt`, the upstream licence, kept with the sources).
Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando
Navarro and Diego Gutierrez.

## Why they are copied in rather than described

The map temporal-off frames run SMAA 1x below native, before EASU, to clean
the stair-steps off planet silhouettes that EASU then upscales (the map
decision of 2026-10-10, docs/design-flat-temporal-aa-2026-09-23.md). SMAA's
edge search and area lookup are tables and constants that are not something
to reproduce from memory, so these are the real text.

## How they reach the shader

`tools/gen_smaa_hlsl.py` turns `SMAA.hlsl` into string chunks at build time
(`smaa_hlsl_gen.h`), the same way `tools/gen_fsr_hlsl.py` does for FSR (a
literal is capped at 65535 bytes). `tools/temporal_shader_build` prepends a
small prologue (`src/d3d11/map_smaa_shader.h`), the chunks, and one entry
point per pass, then compiles them to bytecode. The runtime has no HLSL
compiler, so nothing here is compiled on the render thread.

The prologue selects `SMAA_CUSTOM_SL`, defines the sampler and texture macros
with the same bodies the reference's own `SMAA_HLSL_4` block uses (only the
sampler registers are fixed, to s0 and s1), sets `SMAA_PRESET_HIGH`, and maps
`SMAA_RT_METRICS` onto a constant buffer. The reference's vertex functions
compute the neighbour offsets; the compute entries call them directly.

Nothing in `SMAA.hlsl`, `AreaTex.h` or `SearchTex.h` is edited. A newer SMAA
is a file swap from upstream and a rebuild.
