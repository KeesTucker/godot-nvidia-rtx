# Fork plan

Engine work the fork needs, mostly driven by `~/Projects/seagrass-tank` (see its
PLAN.md). Everything lands straight on `nvidia-pt-dlss`.

## Async compute queue

RenderingDevice only creates a main, transfer and present queue
(`main_queue`, `transfer_queue`, `present_queue` in `rendering_device.h`). Every
compute dispatch runs in line with the path tracer on the main queue. While
warps wait on the RT cores for traversal, the SMs can't pick up unrelated sim
work from another queue.

Do this only after Nsight shows the SMs sitting idle during tracing in
seagrass-tank. Until then it's a guess.

Rough shape:

- At device creation, find a queue family with compute but not graphics (on
  NVIDIA, the dedicated compute family) and create a queue on it.
- Expose a separate list API, something like `async_compute_list_begin()`, that
  records outside the render graph. The graph is single-queue, and teaching it
  about two queues is a much bigger job.
- Sync with timeline semaphores: the main queue waits on the sim's signal before
  the passes that read sim output (vertex capture, BLAS refit), and the sim
  waits on the previous frame's reads before it overwrites anything.
- Buffers shared across queues need `VK_SHARING_MODE_CONCURRENT` or explicit
  queue-family ownership transfers. Concurrent is simpler; check whether it
  costs anything on NVIDIA.

## Water in the path tracer

The path tracer has no refraction: `material.transmissivness` is hard-coded to 0
in `raytracing_closest_hit_common_inc.glsl`, and transparency is alpha peeling
with no ray bending. seagrass-tank M4 needs:

1. A dielectric BSDF: Fresnel, Snell refraction, total internal reflection.
2. A medium stack in raygen (air → glass → water), with per-channel
   Beer–Lambert absorption and Henyey–Greenstein in-scattering.
3. Caustics for a height-field water surface: a pass that refracts sun rays
   through the surface and splats them into an irradiance map, which
   underwater sun NEE then reads instead of casting a shadow ray through the
   surface.
4. DLSS-RR guide buffers that make sense for refracted hits, the same way the
   existing transparency peel handles them.

This goes into the renderer itself, not `drivers/streamline/`, so expect harder
upstream merges in `servers/rendering/renderer_rd/` from here on.

*Built (2026-11-11):* step 1 (e501664), and step 2's medium tracking with
Beer–Lambert absorption (4952156). Media are now shared: a medium is a set of
global shader parameters, `rt_medium_<id>_index_of_refraction`,
`_absorption_per_m`, `_scattering_per_m` and `_scattering_anisotropy` (ids
1–7, 0 = air), read live from the global uniform buffer. A dielectric shader
declares `uniform int rt_medium` for behind its back faces and
`rt_outside_medium` for in front, so one face can separate glass from water
and every face bounding the water agrees on it. Transparency peel layers use
the medium their segment started in, and are absorbed by it. Shadow rays now pass straight through dielectrics (unbent)
and pick up each medium's absorption on the way. The any-hit multiplies in one
factor per crossing that doesn't depend on hit order. Haze is in: every
segment in a scattering medium loses its extinction (absorption + scattering)
and gains single scattering of one light at one equiangular point, with a
Henyey–Greenstein phase and a shadow ray. Exact to first order in optical
depth; multiple scattering is left out, which is fine for thin haze. Costs
about 0.9 ms in seagrass-tank. Step 4 is in: a primary dielectric hit sends
a guide ray through the glass and water to the surface behind, whose depth,
motion, normal, roughness and albedos replace the dielectric's guides at its
apparent position. Since 8165ce8 the path's own hit writes those guides when
it went straight through, and the guide ray only walks where the path
reflected or ran out of bounces; haze past a diffuse bounce is traced by
Russian roulette. Keep raygen's state across traces small: resuming the walk
from where the path stopped (two more vec3s alive) was slower than
retracing. Step 3 is in, the project doing the optics: it fills a
3D `rt_caustics_volume` global (each caustic light's slices stacked in z)
with bent over unbent light, and names its lights by position in
`rt_caustics_light_<n>_position` globals. `gather_lights` matches lights to
those positions (within 1 mm) into `RT_LightData::caustics_layer`, and
`lights_shadow_transmittance` multiplies in the volume at the shadow ray's
start (binding 42), so haze single scattering gets caustic shafts too. Up to
32 caustic lights.

Opaque surfaces inside a medium now reflect against it, not air
(`rt_f0_in_medium`): the material's F0 gives its index, and F0 is recomputed
against the medium's. A leaf (n ≈ 1.45) underwater drops from about 3% to
0.2%. The DLSS-RR specular albedo guide still uses the in-air F0.

## Diffuse transmission for bounce light

`BACKLIGHT` only reaches direct light: `evalCombinedBRDFSeparate` in
`brdf_inc.glsl` adds the backlit term for NEE, but `evalIndirectCombinedBRDF`
never samples through the surface, so bounce light (off sand, glass, other
leaves) can't pass through a thin leaf. A dense seagrass canopy should glow
from inside, and today it doesn't. `SSS_STRENGTH` is parsed and then ignored,
but thin leaves don't need SSS; this is the right model for them.

1. Add a diffuse transmission lobe to the indirect BRDF: with probability set
   by the backlight's weight, sample a cosine hemisphere around −N and weight
   the path by `backlight`. Keep the reflection and transmission choice
   energy-conserving.
2. Wire the same lobe for `BaseMaterial3D`'s back lighting, which is raster-only
   today (the TODO in `scene_raytracing_raygen.glsl`).
3. Check DLSS-RR's guide buffers stay sensible when the first bounce goes
   through a leaf.

Small and self-contained: about 1–2 hours. It fits alongside the water work.

## Bug: source_color defaults skip sRGB conversion

`pack_uniform` in `render_raytracing.cpp` converts a `source_color` uniform
from sRGB to linear when the material sets it as a Color, but packs the
shader's default value raw, so the path tracer sees defaults about 9× too
bright in the darks, unlike the rasterizer. Found 2026-11-12 when
seagrass-tank moved its blade colours from shader defaults to material
parameters and the meadow went black. Fix: convert defaults the same way
(`_def_real` for VEC3/VEC4 with HINT_SOURCE_COLOR). Check seagrass-tank's
remaining custom shaders after, since any colours tuned on the raw defaults
will darken.

## Bug: shadow rays through a medium overflow

A shadow ray starts with its origin medium's loss over its whole length,
`exp(-extinction * t_max)` (`ray_query_shadow_transmittance`), and each
dielectric crossing multiplies in `exp(-extinction_change * rest_of_ray)`
(`rt_shadow_dielectric_crossing_transmittance`), so a ray that leaves the water
takes back what the start charged for the air beyond. Past about 88 in the
exponent a float runs out: the start underflows to 0, the crossing overflows
to ∞, and 0 × ∞ is NaN, which DLSS-RR shows as bare albedo on black. Found
2026-11-15 in seagrass-tank's fish gym, an open sea under the sun:

- A directional light's shadow ray is effectively endless, so any directional
  light is NaN for a point under water. The gym lights its sea with a point
  lamp 10 m off and keeps the directional sun for the sky's disc.
- Murky water (extinction 1.9 a metre) overflowed on 65 m shadow rays to a
  point lamp 40 m off, so the gym shrank its scene to fit.

The tank never hit it: its lights are a short way above shallow, clear water.

Fix: add up optical depth instead of multiplying transmittances. Start the
ray with `extinction * t_max`, add `extinction_change * rest_of_ray` at each
crossing (a sum is as order-free as the product, as any-hit needs), and take
`exp(-optical_depth)` once at the end, clamped at 0. The ray-query path keeps
it in a local. The traceRay path (`scene_raytracing_raygen.glsl`'s shadow
any-hit) multiplies `shadow_ps.throughput`, so it needs the sum in the shadow
payload instead. For a directional light, cap `t_max` at something like 10 km
first, or `extinction * t_max - extinction * (t_max - t_hit)` loses `t_hit`
to rounding. Then check the gym's sea with a real directional sun, walls 30 m
off and the haze up.

Two smaller things from the same session:

- A ray that misses while in a medium (`#[miss]`) returns the sky with no
  absorption or haze at all, so open water with nothing to hit shows the raw
  sky, dark below the horizon. The gym walls its sea in at 10 m. Ending the miss
  at a fixed distance would need the haze's light loop and shadow rays in
  the miss stage, so leave it unless a scene needs open water.
- `rt_medium` (and `rt_outside_medium`) only count when the material sets
  them: `get_medium` reads `material_get_param`, which is nil for a value
  left at the shader's default, so a dielectric relying on its default is an
  opaque black lid. Fall back to the shader's default, as the source_color
  bug above wants for colours.

## Come back to: feature/tank-perf

`feature/tank-perf` (0421cf4, worktree `../godot-nvidia-rtx-tank-perf`) and seagrass-tank's own `feature/tank-perf`
(c06570d, worktree `~/Projects/seagrass-tank-perf`), from 2026-11-15. Kept on purpose, not merged:

- Glass and water cost no bounce (up to 6 crossings a path), and a hit on clear glass carries on to its far face with
  a ray query. It measured slower: 86.5 fps at 1 bounce against 92 on the old engine at 3, as more bounce light
  reaches the room. The thin dielectric slabs (on main since 91483d0) now do the pane half of it.
- Worth picking out: the per-pixel motion vector view (`debug_motion_vectors_per_pixel`, F3 in the tank; its hue
  wraps at "moving left"), and `TankMedia.get_medium_at()` telling the path tracer the camera's medium.
- Open there: side panes got bogus vertical motion vectors in bands on a still scene.

Both branches are behind main, so rebase rather than merge, and measure against main before keeping any of it.
seagrass-tank's PLAN.md ("Frame rate pass (2026-11-15)" on that branch) has the numbers.

## Later

Opacity micromaps, linear swept spheres, cluster/partitioned acceleration
structures and cooperative vectors. The 620.70 driver exposes all of them, and
none are wired into RenderingDevice yet. Nothing in the seagrass-tank MVP needs
them.
