/**************************************************************************/
/*  streamline_ngx.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// StreamlineNGX implements the slice of the Streamline API that Godot calls on
// top of NVIDIA NGX, since Streamline only ships for Windows. It covers DLSS
// Super Resolution and Ray Reconstruction; frame generation, Reflex, PCL and
// NIS report as unsupported.

#ifdef STREAMLINE_NGX

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

class StreamlineContext;

namespace StreamlineNGX {

void install_streamline_functions(StreamlineContext &r_context);

// Extensions NGX needs on the Vulkan instance and device. Godot requests them as
// optional, so a GPU without them still starts, just without DLSS.
LocalVector<CharString> get_required_vulkan_instance_extensions();
LocalVector<CharString> get_required_vulkan_device_extensions();

void remember_vulkan_instance(void *p_vk_instance);
void remember_vulkan_physical_device(void *p_vk_physical_device);
void *get_remembered_vulkan_physical_device();

// Initializes NGX once the device exists. NGX can't report what it supports
// before this, so capabilities have to be enumerated again afterwards.
bool initialize_for_vulkan_device(void *p_vk_device);

} // namespace StreamlineNGX

#endif // STREAMLINE_NGX
