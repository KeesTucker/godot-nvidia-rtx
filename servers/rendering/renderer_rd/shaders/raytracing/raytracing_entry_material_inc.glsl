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

// Absorption per metre of the medium a path is in (PathState::medium); zero in air.
vec3 rt_medium_absorption(uint p_medium) {
	if (p_medium == 0u) {
		return vec3(0.0);
	}
	MaterialData medium_material = materials[(p_medium & ~RT_MEDIUM_OUTSIDE_BIT) - 1u];
	return (p_medium & RT_MEDIUM_OUTSIDE_BIT) != 0u ? medium_material.outside_medium_absorption.xyz : medium_material.medium_absorption.xyz;
}
