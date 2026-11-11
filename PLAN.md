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
factor per crossing that doesn't depend on hit order. Still to do:
Henyey–Greenstein in-scattering, steps 3 and 4. Step 3's light source is the
project's LED bar, not a sun.

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

## Later

Opacity micromaps, linear swept spheres, cluster/partitioned acceleration
structures and cooperative vectors. The 620.70 driver exposes all of them, and
none are wired into RenderingDevice yet. Nothing in the seagrass-tank MVP needs
them.
