"""
ngx_sdk.py - Fetches the NVIDIA DLSS SDK pieces the Linux NGX backend needs.

The SDK's license forbids shipping it on its own, so it's fetched at build time
instead of committed. Every file is pinned to a release tag and checked against
its SHA-256 before use.
"""

import hashlib
import os
import shutil
import urllib.request

NGX_SDK_VERSION = "310.9.16"
NGX_SDK_BASE_URL = f"https://raw.githubusercontent.com/NVIDIA/DLSS/v{NGX_SDK_VERSION}/"
NGX_SDK_DIR = "thirdparty/ngx"

NGX_SDK_FILES = {
    "LICENSE.txt": "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
    "include/nvsdk_ngx.h": "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848",
    "include/nvsdk_ngx_defs.h": "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e",
    "include/nvsdk_ngx_defs_dlssd.h": "2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716",
    "include/nvsdk_ngx_defs_vk.h": "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031",
    "include/nvsdk_ngx_helpers.h": "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471",
    "include/nvsdk_ngx_helpers_dlssd.h": "a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532",
    "include/nvsdk_ngx_helpers_dlssd_vk.h": "11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b",
    "include/nvsdk_ngx_helpers_vk.h": "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623",
    "include/nvsdk_ngx_params.h": "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc",
    "include/nvsdk_ngx_params_dlssd.h": "d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e",
    "include/nvsdk_ngx_vk.h": "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d",
    "lib/Linux_x86_64/libnvsdk_ngx.a": "d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df",
    f"lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.{NGX_SDK_VERSION}": "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6",
    f"lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.{NGX_SDK_VERSION}": "a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96",
}

# NGX looks for these next to the executable, so the build copies them into bin/.
NGX_RUNTIME_LIBRARIES = [
    f"lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.{NGX_SDK_VERSION}",
    f"lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.{NGX_SDK_VERSION}",
]


def _sha256_of_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _has_expected_contents(path, expected_sha256):
    return os.path.isfile(path) and _sha256_of_file(path) == expected_sha256


def _download_verified(relative_path, expected_sha256):
    destination = os.path.join(NGX_SDK_DIR, relative_path)
    if _has_expected_contents(destination, expected_sha256):
        return

    url = NGX_SDK_BASE_URL + relative_path
    print(f"NVIDIA DLSS SDK: downloading {url}")
    with urllib.request.urlopen(url) as response:
        data = response.read()

    downloaded_sha256 = hashlib.sha256(data).hexdigest()
    if downloaded_sha256 != expected_sha256:
        raise RuntimeError(f"{relative_path} has SHA-256 {downloaded_sha256}, expected {expected_sha256}")

    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "wb") as f:
        f.write(data)


def fetch_ngx_sdk():
    """Returns True once every pinned SDK file is present and verified."""
    try:
        for relative_path, expected_sha256 in NGX_SDK_FILES.items():
            _download_verified(relative_path, expected_sha256)
        return True
    except Exception as e:
        print(f"ERROR: Failed to fetch the NVIDIA DLSS SDK, building without DLSS on Linux: {e}")
        return False


def copy_ngx_runtime_libraries(bin_dir="bin"):
    os.makedirs(bin_dir, exist_ok=True)
    for relative_path in NGX_RUNTIME_LIBRARIES:
        source = os.path.join(NGX_SDK_DIR, relative_path)
        destination = os.path.join(bin_dir, os.path.basename(relative_path))
        if not _has_expected_contents(destination, NGX_SDK_FILES[relative_path]):
            shutil.copyfile(source, destination)
