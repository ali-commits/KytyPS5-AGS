#ifndef EMULATOR_SHADER_JOURNAL_LEGACY_INT20_H_
#define EMULATOR_SHADER_JOURNAL_LEGACY_INT20_H_

#include "graphics/shader/shader.h"

// Frozen layouts from 134f665926c04d418b2e7478310caf246c9400c4. Never add current
// fields here: these types describe bytes written by the int20 Linux/x64 build.
namespace Libs::Graphics::ShaderJournalMigration::Int20 {

struct TessellationInputInfo {
	uint32_t input_control_points  = 0;
	uint32_t output_control_points = 0;
	uint32_t ls_stride             = 0;
	uint32_t hs_stride             = 0;
	uint32_t domain                = 0;
	uint32_t partitioning          = 0;
	uint32_t output_topology       = 0;
};

struct VertexInputInfo {
	static constexpr int RES_MAX = 32;

	ShaderBufferResource     resources[RES_MAX];
	ShaderVertexDestination  resources_dst[RES_MAX];
	ShaderVertexInputBuffer  buffers[RES_MAX];
	ShaderStageRuntime       stage;
	ShaderType               logical_stage       = ShaderType::Vertex;
	int                      resources_num       = 0;
	int                      fetch_attrib_reg    = 0;
	int                      fetch_buffer_reg    = 0;
	int                      buffers_num         = 0;
	uint32_t                 wave_size           = 64;
	uint32_t                 scratch_size_dwords = 0;
	uint32_t                 pa_cl_vs_out_cntl   = 0;
	ShaderClipSpaceTransform clip_space;
	ShaderMeshInputInfo      mesh;
	TessellationInputInfo    tess;
	bool                     fetch_external = false;
	bool                     fetch_embedded = false;
	// The user SGPR (s8 + GS user data index) holding the draw's start instance, which an indirect
	// draw has the CP write; -1: the user data value.
	int32_t start_instance_sgpr = -1;
};

struct PixelInputInfo {
	uint32_t interpolator_settings[32]    = {0};
	uint32_t input_num                    = 0;
	uint32_t wave_size                    = 64;
	uint32_t ps_system_input_base         = 0;
	uint32_t custom_interpolation_mask    = 0;
	uint32_t ps_perspective_center_vgpr   = UINT32_MAX;
	uint32_t ps_perspective_centroid_vgpr = UINT32_MAX;
	// First VGPR of the other SPI_PS_INPUT I/J pairs, UINT32_MAX when not enabled.
	uint32_t ps_perspective_sample_vgpr = UINT32_MAX;
	uint32_t ps_linear_sample_vgpr      = UINT32_MAX;
	uint32_t ps_linear_center_vgpr      = UINT32_MAX;
	uint32_t ps_linear_centroid_vgpr    = UINT32_MAX;
	uint8_t  target_output_mode[8]      = {};
	// CB_SHADER_MASK with KYTY_CB_SHADER_MASK_EXPORTS=1 (MRT export i goes to the i-th target
	// with a non-zero nibble); UINT32_MAX otherwise (export i goes to target i).
	uint32_t                                       target_shader_mask           = UINT32_MAX;
	std::array<Prospero::ColorComponentMapping, 8> target_export_mapping        = {};
	uint32_t                                       scratch_size_dwords          = 0;
	bool                                           ps_pos_x                     = false;
	bool                                           ps_pos_y                     = false;
	bool                                           ps_pos_z                     = false;
	bool                                           ps_pos_w                     = false;
	bool                                           ps_front_face                = false;
	bool                                           ps_ancillary                 = false;
	bool                                           ps_no_perspective            = false;
	bool                                           ps_pixel_kill_enable         = false;
	bool                                           ps_depth_export_enable       = false;
	bool                                           ps_sample_mask_export_enable = false;
	bool                                           ps_sample_shading            = false;
	bool                                           dual_source_blending         = false;
	// Export logical alpha or per-channel source factors through MRT1 after channel swizzling.
	ShaderAlphaBlendSource alpha_blend_source = ShaderAlphaBlendSource::None;
	bool                   ps_early_z         = false;
	bool                   ps_execute_on_noop = false;
	ShaderStageRuntime     stage;

	bool HasPositionInput() const { return ps_pos_x || ps_pos_y || ps_pos_z || ps_pos_w; }
};

// Vertex records end immediately before the aliases, not at the padded struct size.
inline constexpr size_t VertexStoredBytes =
    offsetof(VertexInputInfo, start_instance_sgpr) + sizeof(int32_t);
static_assert(sizeof(void*) == 8);
static_assert(VertexStoredBytes == 10152);
static_assert(sizeof(PixelInputInfo) == 224);
static_assert(sizeof(ShaderComputeInputInfo) == 96);
static_assert(sizeof(ShaderStageRuntime) == 16);
static_assert(sizeof(ShaderWorkgroupInputInfo) == 28);
static_assert(sizeof(ShaderMeshInputInfo) == 56);
static_assert(sizeof(ShaderClipSpaceTransform) == 28);
static_assert(sizeof(ShaderVertexInputBuffer) == 280);
static_assert(sizeof(ShaderVertexDestination) == 16);
static_assert(sizeof(TessellationInputInfo) == 28);
static_assert(std::is_trivially_copyable_v<VertexInputInfo>);
static_assert(std::is_trivially_copyable_v<PixelInputInfo>);

} // namespace Libs::Graphics::ShaderJournalMigration::Int20
#endif
