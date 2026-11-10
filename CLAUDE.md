# godot-nvidia-rtx

Kees's fork of NVIDIA-RTX/godot: Godot with NVIDIA's path tracer and DLSS. The
default branch is `nvidia-pt-dlss`. `origin` is the fork and `upstream` is
NVIDIA. Push straight to `nvidia-pt-dlss` on `origin`; no PR needed. If a PR is
ever wanted, use `gh pr create -R KeesTucker/godot-nvidia-rtx`; without `-R`, gh
targets NVIDIA's repo.

Fetch one branch at a time: `git fetch --no-tags origin nvidia-pt-dlss`. A bare
`git fetch` pulls every Godot branch and tag, hundreds of thousands of objects.

## Building

```sh
task install            # package the committed HEAD with makepkg and install it (godot-rtx, launcher entry)
task install:templates  # build and install the Linux export templates
task uninstall          # remove the package
```

`task install` builds from `misc/dist/arch/PKGBUILD`, which clones the checkout's
HEAD, so uncommitted changes don't make it in. Nothing rebuilds it automatically
(the dotfiles' install.sh doesn't know about it), so run it after pulling new
commits. It builds with makepkg's flags,
`-march` swapped for x86-64-v3, and `optimize=speed` (-O3). Its scons cache in
`misc/dist/arch/.scons-cache` keeps rebuilds quick.

`task build` builds in the checkout with Godot's default flags, for working on
the fork. The editor lands in `bin/godot.linuxbsd.editor.x86_64`. A clean build
takes about 5 minutes on atalanta. The launcher entry runs the editor on the
Wayland driver, which is what gives HDR output on Hyprland.

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
