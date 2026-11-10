# DLSS on Linux

Upstream reaches DLSS through NVIDIA Streamline, which only ships for Windows.
On Linux, `streamline_ngx.cpp` implements the `sl*` calls Godot makes on top of
NVIDIA NGX instead, so `dlss.cpp` and the renderer run unchanged on both.

## What works

- DLSS Super Resolution and DLAA.
- DLSS Ray Reconstruction (the path tracer's denoiser).

Frame generation, Reflex, PCL and NIS report as unsupported, and the existing
code skips them.

## Building

`scons platform=linuxbsd` (x86_64, Vulkan) fetches the DLSS SDK pieces from
`github.com/NVIDIA/DLSS` at the version pinned in `misc/utility/ngx_sdk.py`,
checks each against its SHA-256, and puts them in `thirdparty/ngx/`. The SDK's
license bars shipping it on its own, so it's fetched at build time and never
committed. If the fetch fails, the build carries on without DLSS.

The build also copies the two runtime libraries into `bin/`, where NGX looks
for them.

## Shipping a game

Copy these two files next to the exported executable:

- `libnvidia-ngx-dlss.so.310.9.16` (Super Resolution)
- `libnvidia-ngx-dlssd.so.310.9.16` (Ray Reconstruction)

They're in `bin/` after a build. Without them, DLSS turns itself off and the
game renders without it. Players need an NVIDIA RTX GPU and a driver new enough
for this SDK; anything else starts normally, just without DLSS.

## Updating the SDK

Bump `NGX_SDK_VERSION` in `misc/utility/ngx_sdk.py`, download the new files
and replace every hash in `NGX_SDK_FILES`. Update the file names above to match.
