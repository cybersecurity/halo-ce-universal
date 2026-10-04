/*
NV2A_PSH.C

Translation of Xbox pixel shaders - the NV2A texture shader stages and
register combiners, as held in the pixel shader render states - into GLSL.

A pixel shader runs in three parts:
- texture stages 0-3 (PSTextureModes) fetch t0-t3, some of them using the
  result of an earlier stage (dot product and bump environment modes);
- up to eight general combiner stages (PSRGBInputs/Outputs and
  PSAlphaInputs/Outputs) that each compute A*B, C*D and their sum or mux
  on the registers and write them back;
- the final combiner (PSFinalCombinerInputsABCD/EFG):
  rgb = A*B + (1-A)*C + D, alpha = G.
Register values are clamped to [-1, 1] between stages, as on the hardware.
*/

#include "xgpu.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>

enum
{
	_register_zero = 0,
	_register_c0 = 1,
	_register_c1 = 2,
	_register_fog = 3,
	_register_v0 = 4,
	_register_v1 = 5,
	_register_t0 = 8,
	_register_t1 = 9,
	_register_t2 = 10,
	_register_t3 = 11,
	_register_r0 = 12,
	_register_r1 = 13,
	_register_v1r0_sum = 14,
	_register_ef_product = 15,
};

enum
{
	_mode_none = 0x00,
	_mode_project2d = 0x01,
	_mode_project3d = 0x02,
	_mode_cubemap = 0x03,
	_mode_passthru = 0x04,
	_mode_clipplane = 0x05,
	_mode_bumpenvmap = 0x06,
	_mode_bumpenvmap_luminance = 0x07,
	_mode_brdf = 0x08,
	_mode_dot_st = 0x09,
	_mode_dot_zw = 0x0a,
	_mode_dot_reflect_diffuse = 0x0b,
	_mode_dot_reflect_specular = 0x0c,
	_mode_dot_str_3d = 0x0d,
	_mode_dot_str_cube = 0x0e,
	_mode_dependent_ar = 0x0f,
	_mode_dependent_gb = 0x10,
	_mode_dot_product = 0x11,
	_mode_dot_reflect_specular_constant = 0x12,
};

/* ---------- combiner inputs */

static const char *register_expression(unsigned long reg, int stage, BOOL unique_c0, BOOL unique_c1)
{
	static char buffer[4][32];
	static int next = 0;
	char *result = buffer[next++ & 3];

	switch (reg)
	{
	case _register_c0:
		if (stage < 0)
			return "ps_final_c0";
		snprintf(result, sizeof(buffer[0]), "ps_c0[%d]", unique_c0 ? stage : 0);
		return result;
	case _register_c1:
		if (stage < 0)
			return "ps_final_c1";
		snprintf(result, sizeof(buffer[0]), "ps_c1[%d]", unique_c1 ? stage : 0);
		return result;
	case _register_fog: return "fog";
	case _register_v0: return "v0";
	case _register_v1: return "v1";
	case _register_t0: return "t0";
	case _register_t1: return "t1";
	case _register_t2: return "t2";
	case _register_t3: return "t3";
	case _register_r0: return "r0";
	case _register_r1: return "r1";
	case _register_v1r0_sum: return stage < 0 ? "v1r0_sum" : "vec4(0.0)";
	case _register_ef_product: return stage < 0 ? "ef_product" : "vec4(0.0)";
	default: return "vec4(0.0)";
	}
}

/* one combiner input byte as a vec3 (rgb) or float (alpha) expression */
static void combiner_input(struct xgpu_text *text, unsigned long input, BOOL alpha_portion, int stage,
	BOOL unique_c0, BOOL unique_c1)
{
	unsigned long reg = input & 0x0f;
	BOOL alpha_channel = (input & 0x10) != 0;
	unsigned long mapping = input & 0xe0;
	const char *source = register_expression(reg, stage, unique_c0, unique_c1);
	char value[64];

	if (alpha_portion)
		snprintf(value, sizeof(value), "%s.%s", source, alpha_channel ? "a" : "b");
	else if (alpha_channel)
		snprintf(value, sizeof(value), "vec3(%s.a)", source);
	else
		snprintf(value, sizeof(value), "%s.rgb", source);

	switch (mapping)
	{
	case 0x00: xgpu_text_append(text, "max(%s, 0.0)", value); break;
	case 0x20: xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value); break;
	case 0x40: xgpu_text_append(text, "(2.0 * max(%s, 0.0) - 1.0)", value); break;
	case 0x60: xgpu_text_append(text, "(1.0 - 2.0 * max(%s, 0.0))", value); break;
	case 0x80: xgpu_text_append(text, "(max(%s, 0.0) - 0.5)", value); break;
	case 0xa0: xgpu_text_append(text, "(0.5 - max(%s, 0.0))", value); break;
	case 0xc0: xgpu_text_append(text, "(%s)", value); break;
	default: xgpu_text_append(text, "(-%s)", value); break;
	}
}

/* final combiner inputs only have the unsigned identity and invert mappings */
static void final_input(struct xgpu_text *text, unsigned long input, BOOL alpha_portion)
{
	unsigned long reg = input & 0x0f;
	BOOL alpha_channel = (input & 0x10) != 0;
	const char *source = register_expression(reg, -1, FALSE, FALSE);
	char value[64];

	if (alpha_portion)
		snprintf(value, sizeof(value), "%s.%s", source, alpha_channel ? "a" : "b");
	else if (alpha_channel)
		snprintf(value, sizeof(value), "vec3(%s.a)", source);
	else
		snprintf(value, sizeof(value), "%s.rgb", source);
	if (input & 0x20)
		xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value);
	else
		xgpu_text_append(text, "clamp(%s, 0.0, 1.0)", value);
}

static const char *destination_name(unsigned long reg)
{
	switch (reg)
	{
	case _register_v0: return "v0";
	case _register_v1: return "v1";
	case _register_t0: return "t0";
	case _register_t1: return "t1";
	case _register_t2: return "t2";
	case _register_t3: return "t3";
	case _register_r0: return "r0";
	case _register_r1: return "r1";
	default: return NULL;
	}
}

static const char *output_mapping(unsigned long flags)
{
	switch (flags & 0x38)
	{
	case 0x08: return "(%s - 0.5)";
	case 0x10: return "(%s * 2.0)";
	case 0x18: return "((%s - 0.5) * 2.0)";
	case 0x20: return "(%s * 4.0)";
	case 0x30: return "(%s * 0.5)";
	default: return "(%s)";
	}
}

/* ---------- one general combiner stage */

static void combiner_stage(struct xgpu_text *text, const DWORD *state, int stage)
{
	DWORD combiner_count = state[D3DRS_PSCOMBINERCOUNT];
	BOOL unique_c0 = (combiner_count & 0x1000) != 0;
	BOOL unique_c1 = (combiner_count & 0x10000) != 0;
	BOOL mux_msb = (combiner_count & 0x100) != 0;
	int portion;

	xgpu_text_append(text, "\t/* combiner stage %d */\n\t{\n", stage);
	for (portion = 0; portion < 2; portion++)
	{
		BOOL alpha = portion == 1;
		DWORD inputs = alpha ? state[D3DRS_PSALPHAINPUTS0 + stage] : state[D3DRS_PSRGBINPUTS0 + stage];
		DWORD outputs = alpha ? state[D3DRS_PSALPHAOUTPUTS0 + stage] : state[D3DRS_PSRGBOUTPUTS0 + stage];
		unsigned long flags = outputs >> 12;
		const char *type = alpha ? "float" : "vec3";
		const char *prefix = alpha ? "a" : "c";
		const char *mapping = output_mapping(flags);
		char mapped[64];

		xgpu_text_append(text, "\t\t%s %sA = ", type, prefix);
		combiner_input(text, (inputs >> 24) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sB = ", type, prefix);
		combiner_input(text, (inputs >> 16) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sC = ", type, prefix);
		combiner_input(text, (inputs >> 8) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sD = ", type, prefix);
		combiner_input(text, inputs & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n");

		if (!alpha && (flags & 0x02))
			xgpu_text_append(text, "\t\tvec3 cAB = vec3(dot(cA, cB));\n");
		else
			xgpu_text_append(text, "\t\t%s %sAB = %sA * %sB;\n", type, prefix, prefix, prefix);
		if (!alpha && (flags & 0x01))
			xgpu_text_append(text, "\t\tvec3 cCD = vec3(dot(cC, cD));\n");
		else
			xgpu_text_append(text, "\t\t%s %sCD = %sC * %sD;\n", type, prefix, prefix, prefix);
		if (flags & 0x04)
		{
			if (mux_msb)
				xgpu_text_append(text, "\t\t%s %sSUM = r0.a >= 0.5 ? %sCD : %sAB;\n", type, prefix, prefix, prefix);
			else
				xgpu_text_append(text, "\t\t%s %sSUM = (int(r0.a * 255.0 + 0.5) & 1) != 0 ? %sCD : %sAB;\n",
					type, prefix, prefix, prefix);
		}
		else
		{
			xgpu_text_append(text, "\t\t%s %sSUM = %sAB + %sCD;\n", type, prefix, prefix, prefix);
		}
		snprintf(mapped, sizeof(mapped), mapping, "%sAB");
		xgpu_text_append(text, "\t\t%sAB = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sCD");
		xgpu_text_append(text, "\t\t%sCD = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sSUM");
		xgpu_text_append(text, "\t\t%sSUM = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		BOOL alpha = portion == 1;
		DWORD outputs = alpha ? state[D3DRS_PSALPHAOUTPUTS0 + stage] : state[D3DRS_PSRGBOUTPUTS0 + stage];
		unsigned long flags = outputs >> 12;
		const char *prefix = alpha ? "a" : "c";
		const char *component = alpha ? "a" : "rgb";
		const char *ab = destination_name((outputs >> 4) & 0x0f);
		const char *cd = destination_name(outputs & 0x0f);
		const char *sum = destination_name((outputs >> 8) & 0x0f);

		if (ab)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sAB;\n", ab, component, prefix);
			if (!alpha && (flags & 0x80))
				xgpu_text_append(text, "\t\t%s.a = cAB.b;\n", ab);
		}
		if (cd)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sCD;\n", cd, component, prefix);
			if (!alpha && (flags & 0x40))
				xgpu_text_append(text, "\t\t%s.a = cCD.b;\n", cd);
		}
		if (sum)
			xgpu_text_append(text, "\t\t%s.%s = %sSUM;\n", sum, component, prefix);
	}
	xgpu_text_append(text, "\t}\n");
}

/* ---------- texture stages */

static const char *sampler_declaration(unsigned char type)
{
	switch (type)
	{
	case _xgpu_sampler_3d: return "sampler3D";
	case _xgpu_sampler_cube: return "samplerCube";
	default: return "sampler2D";
	}
}

static unsigned long stage_mode(const struct nv2a_pixel_shader_key *key, int stage)
{
	return (key->texture_modes >> (5 * stage)) & 0x1f;
}

static int stage_input(const DWORD *state, int stage)
{
	switch (stage)
	{
	case 2: return (state[D3DRS_PSINPUTTEXTURE] >> 16) & 1;
	case 3: return (state[D3DRS_PSINPUTTEXTURE] >> 20) & 3;
	default: return 0;
	}
}

/* the input texel of a dot product stage, mapped per PSDotMapping */
static void dot_input(struct xgpu_text *text, const DWORD *state, int stage)
{
	unsigned long mapping = (state[D3DRS_PSDOTMAPPING] >> ((stage - 1) * 4)) & 7;
	int input = stage_input(state, stage);

	switch (mapping)
	{
	case 0: xgpu_text_append(text, "t%d.rgb", input); break;
	case 1: xgpu_text_append(text, "((t%d.rgb * 255.0 - 128.0) / 127.0)", input); break;
	case 3: xgpu_text_append(text, "signed_bytes(t%d.rgb)", input); break;
	default: xgpu_text_append(text, "(t%d.rgb * 2.0 - 1.0)", input); break;
	}
}

#ifdef HALO_GLES
/* ES samplers have no LOD bias: pass D3DTSS_MIPMAPLODBIAS to the lookup */
#define SAMPLE_BIAS ", texture_lod_bias[%d]"
#define SHADER_VERSION \
	"precision highp float;\n" \
	"precision highp int;\n" \
	"precision highp sampler2D;\n" \
	"precision highp sampler3D;\n" \
	"precision highp samplerCube;\n"
#else
#define SAMPLE_BIAS ""
#define SHADER_VERSION "#version 450 core\n"
#endif

static void sample(struct xgpu_text *text, const struct nv2a_pixel_shader_key *key, int stage, const char *coordinates)
{
	switch (key->sampler_type[stage])
	{
	case _xgpu_sampler_3d:
		xgpu_text_append(text, "texture(tex%d, (%s).xyz" SAMPLE_BIAS ")", stage, coordinates
#ifdef HALO_GLES
			, stage
#endif
			);
		break;
	case _xgpu_sampler_cube:
		xgpu_text_append(text, "texture(tex%d, (%s).xyz" SAMPLE_BIAS ")", stage, coordinates
#ifdef HALO_GLES
			, stage
#endif
			);
		break;
	default:
		xgpu_text_append(text, "texture(tex%d, (%s).xy * texture_scale[%d].xy" SAMPLE_BIAS ")", stage, coordinates, stage
#ifdef HALO_GLES
			, stage
#endif
			);
		break;
	}
}

static void texture_stage(struct xgpu_text *text, const struct nv2a_pixel_shader_key *key, int stage)
{
	const DWORD *state = key->combiner_state;
	unsigned long mode = stage_mode(key, stage);
	char coordinates[96];

	xgpu_text_append(text, "\t/* texture stage %d, mode %lu */\n", stage, mode);
	if (key->sampler_type[stage] == _xgpu_sampler_none &&
		mode != _mode_passthru && mode != _mode_clipplane && mode != _mode_dot_product && mode != _mode_dot_zw)
	{
		mode = _mode_none;
	}
	switch (mode)
	{
	case _mode_project2d:
	case _mode_project3d:
		snprintf(coordinates, sizeof(coordinates), "vec4(xT%d.xyz / (xT%d.w != 0.0 ? xT%d.w : 1.0), 1.0)", stage, stage, stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_cubemap:
		snprintf(coordinates, sizeof(coordinates), "xT%d", stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_passthru:
		xgpu_text_append(text, "\tt%d = clamp(xT%d, 0.0, 1.0);\n", stage, stage);
		break;
	case _mode_clipplane:
	{
		unsigned long compare = (state[D3DRS_PSCOMPAREMODE] >> (4 * stage)) & 0xf;
		static const char components[] = "xyzw";
		int component;

		for (component = 0; component < 4; component++)
		{
			xgpu_text_append(text, "\tif (xT%d.%c %s 0.0) discard;\n", stage, components[component],
				(compare & (1 << component)) ? ">=" : "<");
		}
		xgpu_text_append(text, "\tt%d = vec4(0.0);\n", stage);
		break;
	}
	case _mode_bumpenvmap:
	case _mode_bumpenvmap_luminance:
	{
		int input = stage - 1;

		xgpu_text_append(text, "\t{\n\t\tvec2 d = signed_bytes(t%d.rgb).rg;\n", input);
		xgpu_text_append(text, "\t\tvec2 coordinates = xT%d.xy + vec2(bump_matrix[%d].x * d.x + bump_matrix[%d].z * d.y,"
			" bump_matrix[%d].y * d.x + bump_matrix[%d].w * d.y);\n", stage, stage, stage, stage, stage);
		xgpu_text_append(text, "\t\tt%d = ", stage);
		sample(text, key, stage, "vec4(coordinates, 0.0, 1.0)");
		xgpu_text_append(text, ";\n");
		if (mode == _mode_bumpenvmap_luminance)
		{
			xgpu_text_append(text, "\t\tt%d.rgb *= clamp(bump_luminance[%d].x * t%d.b + bump_luminance[%d].y, 0.0, 1.0);\n",
				stage, stage, input, stage);
		}
		xgpu_text_append(text, "\t}\n");
		break;
	}
	case _mode_dot_product:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = vec4(0.0);\n", stage);
		break;
	case _mode_dot_st:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		snprintf(coordinates, sizeof(coordinates), "vec4(dot%d, dot%d, 0.0, 1.0)", stage - 1, stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dot_zw:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = vec4(0.0);\n", stage);
		break;
	case _mode_dot_reflect_diffuse:
		/* the normal takes its third component from stage 3's dot product */
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tdot3 = dot(xT3.xyz, ");
		dot_input(text, state, 3);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, key, stage, "vec4(dot1, dot2, dot3, 1.0)");
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dot_reflect_specular:
	case _mode_dot_reflect_specular_constant:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\t{\n\t\tvec3 n = vec3(dot1, dot2, dot3);\n");
		if (mode == _mode_dot_reflect_specular)
			xgpu_text_append(text, "\t\tvec3 e = vec3(xT1.w, xT2.w, xT3.w);\n");
		else
			xgpu_text_append(text, "\t\tvec3 e = ps_c0[0].xyz;\n");
		xgpu_text_append(text, "\t\tvec3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1.0e-20) - e;\n\t\tt%d = ", stage);
		sample(text, key, stage, "vec4(r, 1.0)");
		xgpu_text_append(text, ";\n\t}\n");
		break;
	case _mode_dot_str_3d:
	case _mode_dot_str_cube:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, key, stage, "vec4(dot1, dot2, dot3, 1.0)");
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dependent_ar:
		snprintf(coordinates, sizeof(coordinates), "vec4(t%d.a, t%d.r, 0.0, 1.0)", stage_input(state, stage), stage_input(state, stage));
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dependent_gb:
		snprintf(coordinates, sizeof(coordinates), "vec4(t%d.g, t%d.b, 0.0, 1.0)", stage_input(state, stage), stage_input(state, stage));
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	default:
		xgpu_text_append(text, "\tt%d = vec4(0.0);\n", stage);
		break;
	}

	/* channels the application marked signed (D3DTSS_COLORSIGN) */
	if (key->color_sign[stage] && mode != _mode_none)
	{
		static const char channels[] = "argb";
		int bit;

		for (bit = 0; bit < 4; bit++)
		{
			if (key->color_sign[stage] & (1 << bit))
				xgpu_text_append(text, "\tt%d.%c = signed_byte(t%d.%c);\n", stage, channels[bit], stage, channels[bit]);
		}
	}
	if (key->alpha_kill[stage] && mode != _mode_none)
		xgpu_text_append(text, "\tif (t%d.a == 0.0) discard;\n", stage);
}

/* ---------- the whole shader */

static const char *comparison_operator(unsigned long function)
{
	switch (function)
	{
	case D3DCMP_NEVER: return NULL;
	case D3DCMP_LESS: return "<";
	case D3DCMP_EQUAL: return "==";
	case D3DCMP_LESSEQUAL: return "<=";
	case D3DCMP_GREATER: return ">";
	case D3DCMP_NOTEQUAL: return "!=";
	case D3DCMP_GREATEREQUAL: return ">=";
	default: return "";
	}
}

char *nv2a_pixel_shader_to_glsl(const struct nv2a_pixel_shader_key *key)
{
	const DWORD *state = key->combiner_state;
	struct xgpu_text text = { 0 };
	unsigned long combiner_count = state[D3DRS_PSCOMBINERCOUNT] & 0xff;
	DWORD final_abcd = state[D3DRS_PSFINALCOMBINERINPUTSABCD];
	DWORD final_efg = state[D3DRS_PSFINALCOMBINERINPUTSEFG];
	int stage;

	if (combiner_count > 8)
		combiner_count = 8;

#ifdef HALO_GLES
	xgpu_text_append(&text, "#version %s\n", xgpu_capabilities.shading_language);
	if (key->count_samples)
	{
		/* samples that pass the depth and stencil tests, as the NV2A's
		occlusion counter */
		xgpu_text_append(&text,
			"layout(early_fragment_tests) in;\n"
			"layout(binding = 0, offset = 0) uniform atomic_uint visible_samples;\n");
	}
#endif
	xgpu_text_append(&text,
		SHADER_VERSION
		"in vec4 xD0;\n"
		"in vec4 xD1;\n"
		"in vec4 xB0;\n"
		"in vec4 xB1;\n"
		"in vec4 xT0;\n"
		"in vec4 xT1;\n"
		"in vec4 xT2;\n"
		"in vec4 xT3;\n"
		"in float xFog;\n"
		"layout(location = 0) out vec4 fragment_color;\n"
		XGPU_PIXEL_UNIFORMS);
	for (stage = 0; stage < 4; stage++)
		xgpu_text_append(&text, "uniform %s tex%d;\n", sampler_declaration(key->sampler_type[stage]), stage);
	xgpu_text_append(&text,
		"float signed_byte(float x)\n"
		"{\n"
		"	float b = floor(x * 255.0 + 0.5);\n"
		"	return (b >= 128.0 ? b - 256.0 : b) / 127.0;\n"
		"}\n"
		"vec3 signed_bytes(vec3 x)\n"
		"{\n"
		"	return vec3(signed_byte(x.r), signed_byte(x.g), signed_byte(x.b));\n"
		"}\n"
		"void main()\n"
		"{\n"
		"\tvec4 v0 = xD0;\n"
		"\tvec4 v1 = xD1;\n"
		"\tvec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n"
		"\tfloat dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n");

	for (stage = 0; stage < 4; stage++)
		texture_stage(&text, key, stage);

	/* the fog register: rgb is the fog color, alpha the fog factor */
	if (key->fog_enable)
	{
		switch (key->fog_table_mode)
		{
		case D3DFOG_EXP:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-fog_parameters.z * xFog);\n");
			break;
		case D3DFOG_EXP2:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-(fog_parameters.z * xFog) * (fog_parameters.z * xFog));\n");
			break;
		case D3DFOG_LINEAR:
			xgpu_text_append(&text, "\tfloat fog_factor = (fog_parameters.y - xFog) / max(fog_parameters.y - fog_parameters.x, 1.0e-6);\n");
			break;
		default:
			xgpu_text_append(&text, "\tfloat fog_factor = xFog;\n");
			break;
		}
	}
	else
	{
		xgpu_text_append(&text, "\tfloat fog_factor = 1.0;\n");
	}
	xgpu_text_append(&text,
		"\tvec4 fog = vec4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));\n"
		"\tvec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);\n"
		"\tvec4 r1 = vec4(0.0);\n");

	for (stage = 0; stage < (int)combiner_count; stage++)
		combiner_stage(&text, state, stage);

	if (final_abcd == 0 && final_efg == 0)
	{
		xgpu_text_append(&text, "\tvec4 result = r0;\n");
	}
	else
	{
		unsigned long settings = final_efg & 0xff;

		xgpu_text_append(&text, "\tvec4 ef_product = vec4(");
		final_input(&text, (final_efg >> 24) & 0xff, FALSE);
		xgpu_text_append(&text, " * ");
		final_input(&text, (final_efg >> 16) & 0xff, FALSE);
		xgpu_text_append(&text, ", 0.0);\n");
		xgpu_text_append(&text, "\tvec4 v1r0_sum = vec4(%s + %s, 0.0);\n",
			(settings & 0x40) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "clamp(v1.rgb, 0.0, 1.0)",
			(settings & 0x20) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "clamp(r0.rgb, 0.0, 1.0)");
		if (settings & 0x80)
			xgpu_text_append(&text, "\tv1r0_sum = clamp(v1r0_sum, 0.0, 1.0);\n");
		xgpu_text_append(&text, "\tvec3 fA = ");
		final_input(&text, (final_abcd >> 24) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fB = ");
		final_input(&text, (final_abcd >> 16) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fC = ");
		final_input(&text, (final_abcd >> 8) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fD = ");
		final_input(&text, final_abcd & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tfloat fG = ");
		final_input(&text, (final_efg >> 8) & 0xff, TRUE);
		xgpu_text_append(&text, ";\n\tvec4 result = vec4(fA * fB + (1.0 - fA) * fC + fD, fG);\n");
	}

	if (key->coverage_alpha)
		xgpu_text_append(&text, "\tresult.a = mix(1.0, result.a, t0.g);\n");
	if (key->alpha_test_function)
	{
		const char *comparison = comparison_operator(key->alpha_test_function);

		if (!comparison)
			xgpu_text_append(&text, "\tdiscard;\n");
		else if (*comparison)
			xgpu_text_append(&text, "\tif (!(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5) %s alpha_reference)) discard;\n", comparison);
	}
	if (*config_string("debug.gpu_debug_expression"))
		xgpu_text_append(&text, "\tresult = vec4(vec3(%s), 1.0);\n", config_string("debug.gpu_debug_expression"));
	if (config_boolean("debug.gpu_debug_texture0"))
		xgpu_text_append(&text, "\tresult = vec4(t0.rgb, 1.0);\n");
	if (config_boolean("debug.gpu_debug_flat"))
		xgpu_text_append(&text, "\tresult = xD0.a > 0.0 ? vec4(xD0.rgb, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);\n");
#ifdef HALO_GLES
	if (key->count_samples)
		xgpu_text_append(&text, "\tatomicCounterIncrement(visible_samples);\n");
#endif
	xgpu_text_append(&text, "\tfragment_color = clamp(result, 0.0, 1.0);\n}\n");
	return text.buffer;
}
