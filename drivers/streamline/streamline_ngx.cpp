/**************************************************************************/
/*  streamline_ngx.cpp                                                    */
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

#include "streamline_ngx.h"

#ifdef STREAMLINE_NGX

#include "drivers/streamline/streamline_context.h"
#include "drivers/vulkan/godot_vulkan.h"

#include "core/io/dir_access.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

// The DLSS-D helpers lean on types the DLSS ones declare, so order matters.
#include <nvsdk_ngx_helpers_vk.h>
// Keep after nvsdk_ngx_helpers_vk.h.
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// NGX takes paths as wchar_t, which is UTF-32 on Linux, the same as Godot's String.
static_assert(sizeof(wchar_t) == sizeof(char32_t));

namespace {

// Upstream registers Godot with Streamline under this ID, so NGX gets the same one.
constexpr unsigned long long GODOT_NGX_APPLICATION_ID = 0x90d07004;

// Everything a DLSS feature is created with. A change to any of it means
// releasing the feature and creating a new one.
struct NGXFeatureKey {
	bool is_ray_reconstruction = false;
	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	NVSDK_NGX_PerfQuality_Value perf_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	int create_flags = 0;
	uint32_t preset = 0;
	NVSDK_NGX_DLSS_Roughness_Mode roughness_mode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;

	bool operator==(const NGXFeatureKey &p_other) const {
		return is_ray_reconstruction == p_other.is_ray_reconstruction &&
				render_width == p_other.render_width && render_height == p_other.render_height &&
				output_width == p_other.output_width && output_height == p_other.output_height &&
				perf_quality == p_other.perf_quality && create_flags == p_other.create_flags &&
				preset == p_other.preset && roughness_mode == p_other.roughness_mode;
	}
};

struct NGXViewport {
	HashMap<sl::BufferType, sl::Resource> tagged_resources;
	sl::Constants constants;
	sl::DLSSOptions super_resolution_options;
	sl::DLSSDOptions ray_reconstruction_options;

	NVSDK_NGX_Parameter *parameters = nullptr;
	NVSDK_NGX_Handle *feature = nullptr;
	NGXFeatureKey feature_key;
	// Stops a feature NGX refused to create from being retried, and logged, every frame.
	bool has_failed_feature_key = false;
	NGXFeatureKey failed_feature_key;
};

struct NGXFrameToken : public sl::FrameToken {
	uint32_t frame_index = 0;
	operator uint32_t() const override { return frame_index; }
};

struct NGXState {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	NVSDK_NGX_Parameter *capability_parameters = nullptr;
	// Godot's RenderingDevice support probe makes a device without calling slInit,
	// and never shuts Streamline down. NGX started there crashes the probe on exit.
	bool was_streamline_initialized = false;
	bool is_initialized = false;
	bool is_super_resolution_available = false;
	bool is_ray_reconstruction_available = false;
	bool is_logging_enabled = false;
	HashMap<uint32_t, NGXViewport> viewports;
	NGXFrameToken frame_token;
};

NGXState ngx;
Mutex ngx_mutex;

void NVSDK_CONV print_ngx_log_line(const char *p_message, NVSDK_NGX_Logging_Level p_logging_level, NVSDK_NGX_Feature p_source_component) {
	print_line("NGX: " + String::utf8(p_message).strip_edges());
}

String describe_ngx_result(NVSDK_NGX_Result p_result) {
	return vformat("NGX result 0x%x", (uint32_t)p_result);
}

NVSDK_NGX_PerfQuality_Value to_ngx_perf_quality(sl::DLSSMode p_mode) {
	switch (p_mode) {
		case sl::DLSSMode::eMaxPerformance:
			return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case sl::DLSSMode::eBalanced:
			return NVSDK_NGX_PerfQuality_Value_Balanced;
		case sl::DLSSMode::eUltraPerformance:
			return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case sl::DLSSMode::eUltraQuality:
			return NVSDK_NGX_PerfQuality_Value_UltraQuality;
		case sl::DLSSMode::eDLAA:
			return NVSDK_NGX_PerfQuality_Value_DLAA;
		case sl::DLSSMode::eMaxQuality:
		default:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

// Streamline's preset enums use NGX's numbering, so the values pass straight through.
template <typename Options>
uint32_t get_preset_for_mode(const Options &p_options, sl::DLSSMode p_mode) {
	switch (p_mode) {
		case sl::DLSSMode::eDLAA:
			return (uint32_t)p_options.dlaaPreset;
		case sl::DLSSMode::eMaxQuality:
			return (uint32_t)p_options.qualityPreset;
		case sl::DLSSMode::eBalanced:
			return (uint32_t)p_options.balancedPreset;
		case sl::DLSSMode::eMaxPerformance:
			return (uint32_t)p_options.performancePreset;
		case sl::DLSSMode::eUltraPerformance:
			return (uint32_t)p_options.ultraPerformancePreset;
		case sl::DLSSMode::eUltraQuality:
			return (uint32_t)p_options.ultraQualityPreset;
		default:
			return 0;
	}
}

struct NGXOptimalRenderSize {
	uint32_t optimal_width = 0;
	uint32_t optimal_height = 0;
	uint32_t min_width = 0;
	uint32_t min_height = 0;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	float sharpness = 0.0f;
};

// Same steps as NGX_DLSS_GET_OPTIMAL_SETTINGS, but for either feature's callback.
bool query_optimal_render_size(const char *p_callback_parameter_name, uint32_t p_output_width, uint32_t p_output_height, sl::DLSSMode p_mode, NGXOptimalRenderSize &r_size) {
	if (!ngx.is_initialized || p_mode == sl::DLSSMode::eOff) {
		return false;
	}

	void *callback = nullptr;
	NVSDK_NGX_Parameter_GetVoidPointer(ngx.capability_parameters, p_callback_parameter_name, &callback);
	if (callback == nullptr) {
		return false;
	}

	NVSDK_NGX_Parameter_SetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_Width, p_output_width);
	NVSDK_NGX_Parameter_SetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_Height, p_output_height);
	NVSDK_NGX_Parameter_SetI(ngx.capability_parameters, NVSDK_NGX_Parameter_PerfQualityValue, to_ngx_perf_quality(p_mode));
	NVSDK_NGX_Parameter_SetI(ngx.capability_parameters, NVSDK_NGX_Parameter_RTXValue, 0);

	NVSDK_NGX_Result result = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback)(ngx.capability_parameters);
	if (NVSDK_NGX_FAILED(result)) {
		return false;
	}

	NGXOptimalRenderSize size;
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_OutWidth, &size.optimal_width);
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_OutHeight, &size.optimal_height);
	size.min_width = size.max_width = size.optimal_width;
	size.min_height = size.max_height = size.optimal_height;
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &size.max_width);
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &size.max_height);
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &size.min_width);
	NVSDK_NGX_Parameter_GetUI(ngx.capability_parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &size.min_height);
	NVSDK_NGX_Parameter_GetF(ngx.capability_parameters, NVSDK_NGX_Parameter_Sharpness, &size.sharpness);

	// NGX reports 0x0 for a mode it doesn't offer at this output size.
	const bool is_mode_offered = size.optimal_width > 0 && size.optimal_height > 0;
	if (is_mode_offered) {
		r_size = size;
	}
	return is_mode_offered;
}

NGXViewport &get_or_create_viewport(const sl::ViewportHandle &p_viewport) {
	const uint32_t viewport_id = (uint32_t)p_viewport;
	NGXViewport *existing_viewport = ngx.viewports.getptr(viewport_id);
	if (existing_viewport != nullptr) {
		return *existing_viewport;
	}

	NGXViewport &viewport = ngx.viewports[viewport_id];
	if (ngx.is_initialized) {
		NVSDK_NGX_VULKAN_AllocateParameters(&viewport.parameters);
	}
	return viewport;
}

void release_feature(NGXViewport &r_viewport) {
	if (r_viewport.feature == nullptr) {
		return;
	}
	// The GPU may still be running the last frame's DLSS pass.
	vkDeviceWaitIdle(ngx.device);
	NVSDK_NGX_VULKAN_ReleaseFeature(r_viewport.feature);
	r_viewport.feature = nullptr;
}

const sl::Resource *find_tagged_resource(const NGXViewport &p_viewport, sl::BufferType p_buffer_type) {
	const sl::Resource *resource = p_viewport.tagged_resources.getptr(p_buffer_type);
	const bool is_usable = resource != nullptr && resource->native != nullptr && resource->view != nullptr;
	return is_usable ? resource : nullptr;
}

VkImageAspectFlags get_aspect_mask_for_format(VkFormat p_format) {
	switch (p_format) {
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		default:
			return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

NVSDK_NGX_Resource_VK to_ngx_resource(const sl::Resource &p_resource, bool p_is_written_by_dlss) {
	const VkFormat format = (VkFormat)p_resource.nativeFormat;
	const VkImageSubresourceRange subresource_range = { get_aspect_mask_for_format(format), 0, 1, 0, 1 };
	return NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)p_resource.view, (VkImage)p_resource.native, subresource_range, format, p_resource.width, p_resource.height, p_is_written_by_dlss);
}

// Godot hands every tagged texture over in its sampled layout, as Streamline
// wants, but NGX writes the output as a storage image, so it has to be GENERAL
// while DLSS runs.
void transition_image_layout(VkCommandBuffer p_command_buffer, const sl::Resource &p_resource, VkImageLayout p_from, VkImageLayout p_to) {
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = p_from;
	barrier.newLayout = p_to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = (VkImage)p_resource.native;
	barrier.subresourceRange = { get_aspect_mask_for_format((VkFormat)p_resource.nativeFormat), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
	vkCmdPipelineBarrier(p_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

int get_common_create_flags(const NGXViewport &p_viewport, const sl::Resource &p_motion_vectors, const sl::Resource &p_output, sl::Boolean p_color_buffers_hdr) {
	int flags = NVSDK_NGX_DLSS_Feature_Flags_None;
	if (p_color_buffers_hdr == sl::Boolean::eTrue) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	}
	const bool are_motion_vectors_render_resolution = p_motion_vectors.width < p_output.width;
	if (are_motion_vectors_render_resolution) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	}
	if (p_viewport.constants.motionVectorsJittered == sl::Boolean::eTrue) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
	}
	if (p_viewport.constants.depthInverted == sl::Boolean::eTrue) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	}
	return flags;
}

void set_super_resolution_preset_hints(NVSDK_NGX_Parameter *p_parameters, const sl::DLSSOptions &p_options) {
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (uint32_t)p_options.dlaaPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (uint32_t)p_options.qualityPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (uint32_t)p_options.balancedPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (uint32_t)p_options.performancePreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, (uint32_t)p_options.ultraPerformancePreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, (uint32_t)p_options.ultraQualityPreset);
}

void set_ray_reconstruction_preset_hints(NVSDK_NGX_Parameter *p_parameters, const sl::DLSSDOptions &p_options) {
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, (uint32_t)p_options.dlaaPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, (uint32_t)p_options.qualityPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, (uint32_t)p_options.balancedPreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, (uint32_t)p_options.performancePreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, (uint32_t)p_options.ultraPerformancePreset);
	NVSDK_NGX_Parameter_SetUI(p_parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, (uint32_t)p_options.ultraQualityPreset);
}

bool ensure_feature_matches(NGXViewport &r_viewport, const NGXFeatureKey &p_key, VkCommandBuffer p_command_buffer) {
	const bool is_current_feature_reusable = r_viewport.feature != nullptr && r_viewport.feature_key == p_key;
	if (is_current_feature_reusable) {
		return true;
	}
	const bool already_failed_with_this_key = r_viewport.has_failed_feature_key && r_viewport.failed_feature_key == p_key;
	if (already_failed_with_this_key || r_viewport.parameters == nullptr) {
		return false;
	}

	release_feature(r_viewport);

	NVSDK_NGX_Result result;
	if (p_key.is_ray_reconstruction) {
		set_ray_reconstruction_preset_hints(r_viewport.parameters, r_viewport.ray_reconstruction_options);
		NVSDK_NGX_DLSSD_Create_Params create_params = {};
		create_params.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
		create_params.InRoughnessMode = p_key.roughness_mode;
		create_params.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
		create_params.InWidth = p_key.render_width;
		create_params.InHeight = p_key.render_height;
		create_params.InTargetWidth = p_key.output_width;
		create_params.InTargetHeight = p_key.output_height;
		create_params.InPerfQualityValue = p_key.perf_quality;
		create_params.InFeatureCreateFlags = p_key.create_flags;
		result = NGX_VULKAN_CREATE_DLSSD_EXT1(ngx.device, p_command_buffer, 1, 1, &r_viewport.feature, r_viewport.parameters, &create_params);
	} else {
		set_super_resolution_preset_hints(r_viewport.parameters, r_viewport.super_resolution_options);
		NVSDK_NGX_DLSS_Create_Params create_params = {};
		create_params.Feature.InWidth = p_key.render_width;
		create_params.Feature.InHeight = p_key.render_height;
		create_params.Feature.InTargetWidth = p_key.output_width;
		create_params.Feature.InTargetHeight = p_key.output_height;
		create_params.Feature.InPerfQualityValue = p_key.perf_quality;
		create_params.InFeatureCreateFlags = p_key.create_flags;
		result = NGX_VULKAN_CREATE_DLSS_EXT1(ngx.device, p_command_buffer, 1, 1, &r_viewport.feature, r_viewport.parameters, &create_params);
	}

	if (NVSDK_NGX_FAILED(result)) {
		r_viewport.feature = nullptr;
		r_viewport.has_failed_feature_key = true;
		r_viewport.failed_feature_key = p_key;
		ERR_PRINT(vformat("NGX couldn't create DLSS %s for %dx%d -> %dx%d (%s).", p_key.is_ray_reconstruction ? "Ray Reconstruction" : "Super Resolution",
				p_key.render_width, p_key.render_height, p_key.output_width, p_key.output_height, describe_ngx_result(result)));
		return false;
	}

	r_viewport.feature_key = p_key;
	r_viewport.has_failed_feature_key = false;
	return true;
}

sl::Result evaluate_super_resolution(NGXViewport &r_viewport, VkCommandBuffer p_command_buffer) {
	const sl::Resource *color = find_tagged_resource(r_viewport, sl::kBufferTypeScalingInputColor);
	const sl::Resource *output = find_tagged_resource(r_viewport, sl::kBufferTypeScalingOutputColor);
	const sl::Resource *depth = find_tagged_resource(r_viewport, sl::kBufferTypeDepth);
	const sl::Resource *motion_vectors = find_tagged_resource(r_viewport, sl::kBufferTypeMotionVectors);
	if (color == nullptr || output == nullptr || depth == nullptr || motion_vectors == nullptr) {
		return sl::Result::eErrorMissingInputParameter;
	}
	const sl::Resource *exposure = find_tagged_resource(r_viewport, sl::kBufferTypeExposure);
	const sl::DLSSOptions &options = r_viewport.super_resolution_options;

	NGXFeatureKey key;
	key.render_width = color->width;
	key.render_height = color->height;
	key.output_width = output->width;
	key.output_height = output->height;
	key.perf_quality = to_ngx_perf_quality(options.mode);
	key.preset = get_preset_for_mode(options, options.mode);
	key.create_flags = get_common_create_flags(r_viewport, *motion_vectors, *output, options.colorBuffersHDR);
	if (options.useAutoExposure == sl::Boolean::eTrue) {
		key.create_flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	}
	if (options.alphaUpscalingEnabled == sl::Boolean::eTrue) {
		key.create_flags |= NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
	}
	if (!ensure_feature_matches(r_viewport, key, p_command_buffer)) {
		return sl::Result::eErrorNGXFailed;
	}

	NVSDK_NGX_Resource_VK color_resource = to_ngx_resource(*color, false);
	NVSDK_NGX_Resource_VK output_resource = to_ngx_resource(*output, true);
	NVSDK_NGX_Resource_VK depth_resource = to_ngx_resource(*depth, false);
	NVSDK_NGX_Resource_VK motion_vector_resource = to_ngx_resource(*motion_vectors, false);
	NVSDK_NGX_Resource_VK exposure_resource = exposure != nullptr ? to_ngx_resource(*exposure, false) : NVSDK_NGX_Resource_VK{};

	NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
	eval_params.Feature.pInColor = &color_resource;
	eval_params.Feature.pInOutput = &output_resource;
	eval_params.pInDepth = &depth_resource;
	eval_params.pInMotionVectors = &motion_vector_resource;
	eval_params.pInExposureTexture = exposure != nullptr ? &exposure_resource : nullptr;
	eval_params.InJitterOffsetX = r_viewport.constants.jitterOffset.x;
	eval_params.InJitterOffsetY = r_viewport.constants.jitterOffset.y;
	eval_params.InRenderSubrectDimensions = { color->width, color->height };
	eval_params.InReset = r_viewport.constants.reset == sl::Boolean::eTrue ? 1 : 0;
	// Streamline's mvecScale normalizes motion vectors; NGX wants them in render pixels.
	eval_params.InMVScaleX = r_viewport.constants.mvecScale.x * (float)color->width;
	eval_params.InMVScaleY = r_viewport.constants.mvecScale.y * (float)color->height;
	eval_params.InPreExposure = options.preExposure;
	eval_params.InExposureScale = options.exposureScale;
	eval_params.InIndicatorInvertXAxis = options.indicatorInvertAxisX == sl::Boolean::eTrue ? 1 : 0;
	eval_params.InIndicatorInvertYAxis = options.indicatorInvertAxisY == sl::Boolean::eTrue ? 1 : 0;

	const VkImageLayout output_layout_from_godot = (VkImageLayout)output->state;
	transition_image_layout(p_command_buffer, *output, output_layout_from_godot, VK_IMAGE_LAYOUT_GENERAL);
	NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(p_command_buffer, r_viewport.feature, r_viewport.parameters, &eval_params);
	transition_image_layout(p_command_buffer, *output, VK_IMAGE_LAYOUT_GENERAL, output_layout_from_godot);

	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT_ONCE("NGX couldn't evaluate DLSS Super Resolution (" + describe_ngx_result(result) + ").");
		return sl::Result::eErrorNGXFailed;
	}
	return sl::Result::eOk;
}

sl::Result evaluate_ray_reconstruction(NGXViewport &r_viewport, VkCommandBuffer p_command_buffer) {
	const sl::Resource *color = find_tagged_resource(r_viewport, sl::kBufferTypeScalingInputColor);
	const sl::Resource *output = find_tagged_resource(r_viewport, sl::kBufferTypeScalingOutputColor);
	const sl::Resource *depth = find_tagged_resource(r_viewport, sl::kBufferTypeDepth);
	const sl::Resource *motion_vectors = find_tagged_resource(r_viewport, sl::kBufferTypeMotionVectors);
	const sl::Resource *diffuse_albedo = find_tagged_resource(r_viewport, sl::kBufferTypeAlbedo);
	const sl::Resource *specular_albedo = find_tagged_resource(r_viewport, sl::kBufferTypeSpecularAlbedo);
	const sl::Resource *normal_roughness = find_tagged_resource(r_viewport, sl::kBufferTypeNormalRoughness);
	if (color == nullptr || output == nullptr || depth == nullptr || motion_vectors == nullptr ||
			diffuse_albedo == nullptr || specular_albedo == nullptr || normal_roughness == nullptr) {
		return sl::Result::eErrorMissingInputParameter;
	}
	const sl::Resource *roughness = find_tagged_resource(r_viewport, sl::kBufferTypeRoughness);
	const sl::Resource *specular_motion_vectors = find_tagged_resource(r_viewport, sl::kBufferTypeSpecularMotionVectors);
	const sl::Resource *exposure = find_tagged_resource(r_viewport, sl::kBufferTypeExposure);
	const sl::DLSSDOptions &options = r_viewport.ray_reconstruction_options;

	const bool is_roughness_in_normal_alpha = options.normalRoughnessMode == sl::DLSSDNormalRoughnessMode::ePacked;
	if (!is_roughness_in_normal_alpha && roughness == nullptr) {
		return sl::Result::eErrorMissingInputParameter;
	}

	NGXFeatureKey key;
	key.is_ray_reconstruction = true;
	key.render_width = color->width;
	key.render_height = color->height;
	key.output_width = output->width;
	key.output_height = output->height;
	key.perf_quality = to_ngx_perf_quality(options.mode);
	key.preset = get_preset_for_mode(options, options.mode);
	key.roughness_mode = is_roughness_in_normal_alpha ? NVSDK_NGX_DLSS_Roughness_Mode_Packed : NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
	key.create_flags = get_common_create_flags(r_viewport, *motion_vectors, *output, options.colorBuffersHDR);
	if (options.alphaUpscalingEnabled == sl::Boolean::eTrue) {
		key.create_flags |= NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
	}
	if (!ensure_feature_matches(r_viewport, key, p_command_buffer)) {
		return sl::Result::eErrorNGXFailed;
	}

	NVSDK_NGX_Resource_VK color_resource = to_ngx_resource(*color, false);
	NVSDK_NGX_Resource_VK output_resource = to_ngx_resource(*output, true);
	NVSDK_NGX_Resource_VK depth_resource = to_ngx_resource(*depth, false);
	NVSDK_NGX_Resource_VK motion_vector_resource = to_ngx_resource(*motion_vectors, false);
	NVSDK_NGX_Resource_VK diffuse_albedo_resource = to_ngx_resource(*diffuse_albedo, false);
	NVSDK_NGX_Resource_VK specular_albedo_resource = to_ngx_resource(*specular_albedo, false);
	NVSDK_NGX_Resource_VK normal_resource = to_ngx_resource(*normal_roughness, false);
	NVSDK_NGX_Resource_VK roughness_resource = roughness != nullptr ? to_ngx_resource(*roughness, false) : NVSDK_NGX_Resource_VK{};
	NVSDK_NGX_Resource_VK specular_motion_vector_resource = specular_motion_vectors != nullptr ? to_ngx_resource(*specular_motion_vectors, false) : NVSDK_NGX_Resource_VK{};
	NVSDK_NGX_Resource_VK exposure_resource = exposure != nullptr ? to_ngx_resource(*exposure, false) : NVSDK_NGX_Resource_VK{};

	NVSDK_NGX_VK_DLSSD_Eval_Params eval_params = {};
	eval_params.pInColor = &color_resource;
	eval_params.pInOutput = &output_resource;
	eval_params.pInDepth = &depth_resource;
	eval_params.pInMotionVectors = &motion_vector_resource;
	eval_params.pInDiffuseAlbedo = &diffuse_albedo_resource;
	eval_params.pInSpecularAlbedo = &specular_albedo_resource;
	eval_params.pInNormals = &normal_resource;
	eval_params.pInRoughness = is_roughness_in_normal_alpha ? nullptr : &roughness_resource;
	eval_params.pInMotionVectorsReflections = specular_motion_vectors != nullptr ? &specular_motion_vector_resource : nullptr;
	eval_params.pInExposureTexture = exposure != nullptr ? &exposure_resource : nullptr;
	eval_params.InJitterOffsetX = r_viewport.constants.jitterOffset.x;
	eval_params.InJitterOffsetY = r_viewport.constants.jitterOffset.y;
	eval_params.InRenderSubrectDimensions = { color->width, color->height };
	eval_params.InReset = r_viewport.constants.reset == sl::Boolean::eTrue ? 1 : 0;
	eval_params.InMVScaleX = r_viewport.constants.mvecScale.x * (float)color->width;
	eval_params.InMVScaleY = r_viewport.constants.mvecScale.y * (float)color->height;
	eval_params.InPreExposure = options.preExposure;
	eval_params.InExposureScale = options.exposureScale;
	eval_params.InIndicatorInvertXAxis = options.indicatorInvertAxisX == sl::Boolean::eTrue ? 1 : 0;
	eval_params.InIndicatorInvertYAxis = options.indicatorInvertAxisY == sl::Boolean::eTrue ? 1 : 0;
	// Streamline hands these matrices to NGX as they are, so the shim does too.
	eval_params.pInWorldToViewMatrix = (float *)&r_viewport.ray_reconstruction_options.worldToCameraView;
	eval_params.pInViewToClipMatrix = (float *)&r_viewport.constants.cameraViewToClip;

	const VkImageLayout output_layout_from_godot = (VkImageLayout)output->state;
	transition_image_layout(p_command_buffer, *output, output_layout_from_godot, VK_IMAGE_LAYOUT_GENERAL);
	NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(p_command_buffer, r_viewport.feature, r_viewport.parameters, &eval_params);
	transition_image_layout(p_command_buffer, *output, VK_IMAGE_LAYOUT_GENERAL, output_layout_from_godot);

	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT_ONCE("NGX couldn't evaluate DLSS Ray Reconstruction (" + describe_ngx_result(result) + ").");
		return sl::Result::eErrorNGXFailed;
	}
	return sl::Result::eOk;
}

const sl::ViewportHandle *find_viewport_input(const sl::BaseStructure **p_inputs, uint32_t p_input_count) {
	for (uint32_t i = 0; i < p_input_count; i++) {
		const bool is_viewport_handle = p_inputs[i] != nullptr && p_inputs[i]->structType == sl::ViewportHandle::s_structType;
		if (is_viewport_handle) {
			return static_cast<const sl::ViewportHandle *>(p_inputs[i]);
		}
	}
	return nullptr;
}

/* The sl* entry points Godot calls through StreamlineContext. */

sl::Result sl_init(const sl::Preferences &p_preferences, uint64_t p_sdk_version) {
	MutexLock lock(ngx_mutex);
	ngx.is_logging_enabled = p_preferences.logLevel != sl::LogLevel::eOff;
	ngx.was_streamline_initialized = true;
	return sl::Result::eOk;
}

sl::Result sl_shutdown() {
	MutexLock lock(ngx_mutex);
	if (!ngx.is_initialized) {
		return sl::Result::eOk;
	}

	vkDeviceWaitIdle(ngx.device);
	for (KeyValue<uint32_t, NGXViewport> &viewport : ngx.viewports) {
		if (viewport.value.feature != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(viewport.value.feature);
		}
		if (viewport.value.parameters != nullptr) {
			NVSDK_NGX_VULKAN_DestroyParameters(viewport.value.parameters);
		}
	}
	NVSDK_NGX_VULKAN_DestroyParameters(ngx.capability_parameters);
	NVSDK_NGX_VULKAN_Shutdown1(ngx.device);

	const bool was_logging_enabled = ngx.is_logging_enabled;
	ngx = NGXState();
	ngx.is_logging_enabled = was_logging_enabled;
	return sl::Result::eOk;
}

sl::Result sl_is_feature_supported(sl::Feature p_feature, const sl::AdapterInfo &p_adapter_info) {
	MutexLock lock(ngx_mutex);
	const bool is_supported = (p_feature == sl::kFeatureDLSS && ngx.is_super_resolution_available) ||
			(p_feature == sl::kFeatureDLSS_RR && ngx.is_ray_reconstruction_available);
	return is_supported ? sl::Result::eOk : sl::Result::eErrorFeatureNotSupported;
}

sl::Result sl_set_feature_loaded(sl::Feature p_feature, bool p_loaded) {
	return sl::Result::eOk;
}

sl::Result sl_get_new_frame_token(sl::FrameToken *&r_token, const uint32_t *p_frame_index) {
	MutexLock lock(ngx_mutex);
	ngx.frame_token.frame_index = p_frame_index != nullptr ? *p_frame_index : ngx.frame_token.frame_index + 1;
	r_token = &ngx.frame_token;
	return sl::Result::eOk;
}

sl::Result sl_allocate_resources(sl::CommandBuffer *p_command_buffer, sl::Feature p_feature, const sl::ViewportHandle &p_viewport) {
	return sl::Result::eOk;
}

sl::Result sl_free_resources(sl::Feature p_feature, const sl::ViewportHandle &p_viewport) {
	MutexLock lock(ngx_mutex);
	NGXViewport *viewport = ngx.viewports.getptr((uint32_t)p_viewport);
	if (viewport != nullptr) {
		release_feature(*viewport);
	}
	return sl::Result::eOk;
}

sl::Result sl_set_tag(const sl::ViewportHandle &p_viewport, const sl::ResourceTag *p_tags, uint32_t p_tag_count, sl::CommandBuffer *p_command_buffer) {
	MutexLock lock(ngx_mutex);
	NGXViewport &viewport = get_or_create_viewport(p_viewport);
	for (uint32_t i = 0; i < p_tag_count; i++) {
		const sl::ResourceTag &tag = p_tags[i];
		if (tag.resource == nullptr) {
			viewport.tagged_resources.erase(tag.type);
		} else {
			// The caller's sl::Resource lives on its stack, so keep a copy.
			viewport.tagged_resources[tag.type] = *tag.resource;
		}
	}
	return sl::Result::eOk;
}

sl::Result sl_set_constants(const sl::Constants &p_constants, const sl::FrameToken &p_frame, const sl::ViewportHandle &p_viewport) {
	MutexLock lock(ngx_mutex);
	get_or_create_viewport(p_viewport).constants = p_constants;
	return sl::Result::eOk;
}

sl::Result sl_evaluate_feature(sl::Feature p_feature, const sl::FrameToken &p_frame, const sl::BaseStructure **p_inputs, uint32_t p_input_count, sl::CommandBuffer *p_command_buffer) {
	MutexLock lock(ngx_mutex);
	const sl::ViewportHandle *viewport_handle = find_viewport_input(p_inputs, p_input_count);
	if (viewport_handle == nullptr) {
		return sl::Result::eErrorMissingInputParameter;
	}
	NGXViewport *viewport = ngx.viewports.getptr((uint32_t)*viewport_handle);
	if (viewport == nullptr) {
		return sl::Result::eErrorMissingInputParameter;
	}

	VkCommandBuffer command_buffer = (VkCommandBuffer)p_command_buffer;
	if (p_feature == sl::kFeatureDLSS && ngx.is_super_resolution_available) {
		return evaluate_super_resolution(*viewport, command_buffer);
	}
	if (p_feature == sl::kFeatureDLSS_RR && ngx.is_ray_reconstruction_available) {
		return evaluate_ray_reconstruction(*viewport, command_buffer);
	}
	return sl::Result::eErrorFeatureNotSupported;
}

sl::Result sl_set_d3d_device(void *p_d3d_device) {
	return sl::Result::eErrorUnsupportedInterface;
}

sl::Result sl_dlss_get_optimal_settings(const sl::DLSSOptions &p_options, sl::DLSSOptimalSettings &r_settings) {
	MutexLock lock(ngx_mutex);
	NGXOptimalRenderSize size;
	if (!query_optimal_render_size(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, p_options.outputWidth, p_options.outputHeight, p_options.mode, size)) {
		return sl::Result::eErrorInvalidParameter;
	}
	r_settings.optimalRenderWidth = size.optimal_width;
	r_settings.optimalRenderHeight = size.optimal_height;
	r_settings.optimalSharpness = size.sharpness;
	r_settings.renderWidthMin = size.min_width;
	r_settings.renderHeightMin = size.min_height;
	r_settings.renderWidthMax = size.max_width;
	r_settings.renderHeightMax = size.max_height;
	return sl::Result::eOk;
}

sl::Result sl_dlss_get_state(const sl::ViewportHandle &p_viewport, sl::DLSSState &r_state) {
	r_state = sl::DLSSState();
	return sl::Result::eOk;
}

sl::Result sl_dlss_set_options(const sl::ViewportHandle &p_viewport, const sl::DLSSOptions &p_options) {
	MutexLock lock(ngx_mutex);
	get_or_create_viewport(p_viewport).super_resolution_options = p_options;
	return sl::Result::eOk;
}

sl::Result sl_dlssd_get_optimal_settings(const sl::DLSSDOptions &p_options, sl::DLSSDOptimalSettings &r_settings) {
	MutexLock lock(ngx_mutex);
	NGXOptimalRenderSize size;
	if (!query_optimal_render_size(NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, p_options.outputWidth, p_options.outputHeight, p_options.mode, size)) {
		return sl::Result::eErrorInvalidParameter;
	}
	r_settings.optimalRenderWidth = size.optimal_width;
	r_settings.optimalRenderHeight = size.optimal_height;
	r_settings.optimalSharpness = size.sharpness;
	r_settings.renderWidthMin = size.min_width;
	r_settings.renderHeightMin = size.min_height;
	r_settings.renderWidthMax = size.max_width;
	r_settings.renderHeightMax = size.max_height;
	return sl::Result::eOk;
}

sl::Result sl_dlssd_get_state(const sl::ViewportHandle &p_viewport, sl::DLSSDState &r_state) {
	r_state = sl::DLSSDState();
	return sl::Result::eOk;
}

sl::Result sl_dlssd_set_options(const sl::ViewportHandle &p_viewport, const sl::DLSSDOptions &p_options) {
	MutexLock lock(ngx_mutex);
	get_or_create_viewport(p_viewport).ray_reconstruction_options = p_options;
	return sl::Result::eOk;
}

// Hands out only what NGX can back, so Reflex, PCL, DLSS-G and NIS stay null
// and the callers skip them.
sl::Result sl_get_feature_function(sl::Feature p_feature, const char *p_function_name, void *&r_function) {
	MutexLock lock(ngx_mutex);
	const String function_name = p_function_name;
	r_function = nullptr;

	if (p_feature == sl::kFeatureDLSS && ngx.is_super_resolution_available) {
		if (function_name == "slDLSSGetOptimalSettings") {
			r_function = (void *)&sl_dlss_get_optimal_settings;
		} else if (function_name == "slDLSSGetState") {
			r_function = (void *)&sl_dlss_get_state;
		} else if (function_name == "slDLSSSetOptions") {
			r_function = (void *)&sl_dlss_set_options;
		}
	} else if (p_feature == sl::kFeatureDLSS_RR && ngx.is_ray_reconstruction_available) {
		if (function_name == "slDLSSDGetOptimalSettings") {
			r_function = (void *)&sl_dlssd_get_optimal_settings;
		} else if (function_name == "slDLSSDGetState") {
			r_function = (void *)&sl_dlssd_get_state;
		} else if (function_name == "slDLSSDSetOptions") {
			r_function = (void *)&sl_dlssd_set_options;
		}
	}

	return r_function != nullptr ? sl::Result::eOk : sl::Result::eErrorFeatureMissing;
}

LocalVector<CharString> to_char_strings(unsigned int p_count, const char **p_names) {
	LocalVector<CharString> names;
	for (unsigned int i = 0; i < p_count; i++) {
		names.push_back(CharString(p_names[i]));
	}
	return names;
}

} // namespace

namespace StreamlineNGX {

void install_streamline_functions(StreamlineContext &r_context) {
	r_context.slInit = &sl_init;
	r_context.slShutdown = &sl_shutdown;
	r_context.slIsFeatureSupported = &sl_is_feature_supported;
	r_context.slGetFeatureFunction = &sl_get_feature_function;
	r_context.slGetNewFrameToken = &sl_get_new_frame_token;
	r_context.slAllocateResources = &sl_allocate_resources;
	r_context.slFreeResources = &sl_free_resources;
	r_context.slEvaluateFeature = &sl_evaluate_feature;
	r_context.slSetTag = &sl_set_tag;
	r_context.slSetConstants = &sl_set_constants;
	r_context.slSetFeatureLoaded = &sl_set_feature_loaded;
	r_context.slSetD3DDevice = &sl_set_d3d_device;
}

LocalVector<CharString> get_required_vulkan_instance_extensions() {
	unsigned int instance_extension_count = 0;
	unsigned int device_extension_count = 0;
	const char **instance_extensions = nullptr;
	const char **device_extensions = nullptr;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(&instance_extension_count, &instance_extensions, &device_extension_count, &device_extensions);
	if (NVSDK_NGX_FAILED(result)) {
		return LocalVector<CharString>();
	}
	return to_char_strings(instance_extension_count, instance_extensions);
}

LocalVector<CharString> get_required_vulkan_device_extensions() {
	unsigned int instance_extension_count = 0;
	unsigned int device_extension_count = 0;
	const char **instance_extensions = nullptr;
	const char **device_extensions = nullptr;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(&instance_extension_count, &instance_extensions, &device_extension_count, &device_extensions);
	if (NVSDK_NGX_FAILED(result)) {
		return LocalVector<CharString>();
	}
	return to_char_strings(device_extension_count, device_extensions);
}

void remember_vulkan_instance(void *p_vk_instance) {
	MutexLock lock(ngx_mutex);
	ngx.instance = (VkInstance)p_vk_instance;
}

void remember_vulkan_physical_device(void *p_vk_physical_device) {
	MutexLock lock(ngx_mutex);
	ngx.physical_device = (VkPhysicalDevice)p_vk_physical_device;
}

void *get_remembered_vulkan_physical_device() {
	MutexLock lock(ngx_mutex);
	return (void *)ngx.physical_device;
}

bool initialize_for_vulkan_device(void *p_vk_device) {
	MutexLock lock(ngx_mutex);
	if (ngx.is_initialized) {
		return true;
	}
	if (!ngx.was_streamline_initialized || ngx.instance == VK_NULL_HANDLE || ngx.physical_device == VK_NULL_HANDLE || p_vk_device == nullptr) {
		return false;
	}
	ngx.device = (VkDevice)p_vk_device;

	// NGX writes its logs and temporary files here.
	const String data_path = OS::get_singleton()->get_cache_path().path_join("godot-ngx");
	DirAccess::make_dir_recursive_absolute(data_path);

	NVSDK_NGX_FeatureCommonInfo feature_info = {};
	if (ngx.is_logging_enabled) {
		feature_info.LoggingInfo.LoggingCallback = &print_ngx_log_line;
		feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
	}

	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(GODOT_NGX_APPLICATION_ID, (const wchar_t *)data_path.get_data(),
			ngx.instance, ngx.physical_device, ngx.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &feature_info);
	if (NVSDK_NGX_FAILED(result)) {
		// Expected on anything but an NVIDIA RTX GPU, so not an error.
		print_verbose("NGX: not available, DLSS is off (" + describe_ngx_result(result) + ").");
		ngx.device = VK_NULL_HANDLE;
		return false;
	}

	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&ngx.capability_parameters);
	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT("NGX started but couldn't report its capabilities, DLSS is off (" + describe_ngx_result(result) + ").");
		NVSDK_NGX_VULKAN_Shutdown1(ngx.device);
		ngx.device = VK_NULL_HANDLE;
		return false;
	}

	int super_resolution_available = 0;
	int ray_reconstruction_available = 0;
	int needs_updated_driver = 0;
	NVSDK_NGX_Parameter_GetI(ngx.capability_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &super_resolution_available);
	NVSDK_NGX_Parameter_GetI(ngx.capability_parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &ray_reconstruction_available);
	NVSDK_NGX_Parameter_GetI(ngx.capability_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_updated_driver);
	if (needs_updated_driver != 0) {
		WARN_PRINT("NGX: DLSS needs a newer NVIDIA driver.");
	}

	ngx.is_initialized = true;
	ngx.is_super_resolution_available = super_resolution_available != 0;
	ngx.is_ray_reconstruction_available = ray_reconstruction_available != 0;
	print_verbose(vformat("NGX: DLSS Super Resolution %s, Ray Reconstruction %s.",
			ngx.is_super_resolution_available ? "available" : "unavailable",
			ngx.is_ray_reconstruction_available ? "available" : "unavailable"));

	// Viewports tagged before NGX was up still need their parameter blocks.
	for (KeyValue<uint32_t, NGXViewport> &viewport : ngx.viewports) {
		if (viewport.value.parameters == nullptr) {
			NVSDK_NGX_VULKAN_AllocateParameters(&viewport.value.parameters);
		}
	}
	return true;
}

} // namespace StreamlineNGX

#endif // STREAMLINE_NGX
