// Per-entry material access. Include right after geometries[] and materials[] are declared;
// every read of materials[] goes through these.

// Material of the TLAS geometry entry p_geometry_idx, with the entry's flags and instance transparency applied.
MaterialData rt_entry_material(uint p_geometry_idx) {
	const uint material_index = geometries[p_geometry_idx].material_index;
	MaterialData mat = materials[material_index & RT_MATERIAL_INDEX_MASK];
	mat.albedo_texture |= material_index & ~RT_MATERIAL_INDEX_MASK;
	mat.flags |= geometries[p_geometry_idx].material_flags;
	return mat;
}

// RT_MAT_FLAG_* of the TLAS geometry entry p_geometry_idx.
uint rt_entry_material_flags(uint p_geometry_idx) {
	return materials[geometries[p_geometry_idx].material_index & RT_MATERIAL_INDEX_MASK].flags | geometries[p_geometry_idx].material_flags;
}

#ifdef RT_MEDIA_COUNT
// rt_shadow_dielectric_crossing_transmittance is the factor a shadow ray picks
// up where it crosses a dielectric at p_hit_t, given it starts with
// exp(-sigma_origin * p_ray_end_t). Each crossing swaps sigma for the rest of
// the ray, so the factors multiply in any order, as any-hit needs.
vec3 rt_shadow_dielectric_crossing_transmittance(MaterialData p_mat, bool p_is_front_face, float p_hit_t, float p_ray_end_t) {
	uint entered_medium = p_is_front_face ? p_mat.inside_medium : p_mat.outside_medium;
	uint left_medium = p_is_front_face ? p_mat.outside_medium : p_mat.inside_medium;
	vec3 extinction_change = rt_medium_extinction(entered_medium) - rt_medium_extinction(left_medium);
	float rest_of_ray = max(p_ray_end_t - p_hit_t, 0.0);
	return exp(-extinction_change * rest_of_ray);
}
#endif
