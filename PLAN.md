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

## Later

Opacity micromaps, linear swept spheres, cluster/partitioned acceleration
structures and cooperative vectors. The 620.70 driver exposes all of them, and
none are wired into RenderingDevice yet. Nothing in the seagrass-tank MVP needs
them.
