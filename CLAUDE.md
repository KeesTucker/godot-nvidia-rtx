# godot-nvidia-rtx

Kees's fork of NVIDIA-RTX/godot: Godot with NVIDIA's path tracer and DLSS. The
default branch is `nvidia-pt-dlss`. `origin` is the fork and `upstream` is
NVIDIA. Open PRs against the fork with `gh pr create -R KeesTucker/godot-nvidia-rtx`;
without `-R`, gh targets NVIDIA's repo.

## Building

```sh
mise exec -- scons platform=linuxbsd target=editor -j24
```

The editor lands in `bin/godot.linuxbsd.editor.x86_64`. A clean build takes
about 5 minutes on atalanta.

## Linux DLSS

Streamline is Windows-only, so Linux DLSS goes through an NGX shim. Read
`drivers/streamline/README.md` before touching it.

- Keep fork changes inside `drivers/streamline/` and small hooks elsewhere.
  `dlss.cpp` and the renderer stay as NVIDIA wrote them, so upstream merges stay
  easy.
- Never commit the DLSS SDK (`thirdparty/ngx/`). Its license bars shipping it
  on its own and the fork is public.
- NGX must only start after `slInit`. Godot's `--test-rd-support` probe makes a
  Vulkan device without starting Streamline. NGX started there crashes the
  probe on exit, and the Project Manager then claims the GPU has no
  RenderingDevice support.

## Testing

`~/Projects/dlss-rt-test` has a path-traced scene (`main.tscn`) and the godot-ai
MCP plugin, so Claude can drive the editor and take viewport screenshots. The
quick check for Ray Reconstruction: at 1 sample per pixel the frame is clean
with the denoiser on and grainy with it off.
