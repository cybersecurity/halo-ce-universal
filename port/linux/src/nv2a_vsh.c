/*
NV2A_VSH.C

Translation of NV2A vertex programs (Xbox vertex shader microcode) into
GLSL.

Each instruction is four little-endian words. Word 1 to 3 hold a MAC
(vector) operation and an ILU (scalar) operation that execute in parallel
on the same three operands A, B and C, and the destinations of both; the
bit positions of every field are spelled out in operand() and in
nv2a_vertex_shader_to_glsl(). When both units run, the ILU result goes to
temporary r1, whatever the instruction's temporary register field says.

Xbox vertex programs finish by converting their clip-space position to
screen space with the viewport constants c[-38] and c[-37], which Direct3D
maintains. The generated shader inverts that transform to hand OpenGL a
clip-space position again.
*/

#include "xgpu.h"

#include <stdlib.h>

/* ---------- instruction fields */

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* output register addresses (o[]) */
static const char *output_name(unsigned long address)
{
	switch (address)
	{
	case 0: return "oPos";
	case 3: return "oD0";
	case 4: return "oD1";
	case 5: return "oFog";
	case 6: return "oPts";
	case 7: return "oB0";
	case 8: return "oB1";
	case 9: return "oT0";
	case 10: return "oT1";
	case 11: return "oT2";
	case 12: return "oT3";
	default: return "oUnused";
	}
}

static void write_mask(unsigned long mask, char *out)
{
	/* bit 3 is x */
	int count = 0;

	if (mask & 8) out[count++] = 'x';
	if (mask & 4) out[count++] = 'y';
	if (mask & 2) out[count++] = 'z';
	if (mask & 1) out[count++] = 'w';
	out[count] = 0;
}

static void operand(struct xgpu_text *text, const DWORD *instruction, char which, int relative)
{
	static const char swizzle_names[] = "xyzw";
	unsigned long negate, swizzle[4], index, mux;

	switch (which)
	{
	case 'A':
		negate = field(instruction, 1, 8, 1);
		swizzle[0] = field(instruction, 1, 6, 2);
		swizzle[1] = field(instruction, 1, 4, 2);
		swizzle[2] = field(instruction, 1, 2, 2);
		swizzle[3] = field(instruction, 1, 0, 2);
		index = field(instruction, 2, 28, 4);
		mux = field(instruction, 2, 26, 2);
		break;
	case 'B':
		negate = field(instruction, 2, 25, 1);
		swizzle[0] = field(instruction, 2, 23, 2);
		swizzle[1] = field(instruction, 2, 21, 2);
		swizzle[2] = field(instruction, 2, 19, 2);
		swizzle[3] = field(instruction, 2, 17, 2);
		index = field(instruction, 2, 13, 4);
		mux = field(instruction, 2, 11, 2);
		break;
	default:
		negate = field(instruction, 2, 10, 1);
		swizzle[0] = field(instruction, 2, 8, 2);
		swizzle[1] = field(instruction, 2, 6, 2);
		swizzle[2] = field(instruction, 2, 4, 2);
		swizzle[3] = field(instruction, 2, 2, 2);
		index = (field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2);
		mux = field(instruction, 3, 28, 2);
		break;
	}

	xgpu_text_append(text, "%s", negate ? "-" : "");
	switch (mux)
	{
	case _mux_temporary:
		/* r12 reads back the position output */
		if (index == 12)
			xgpu_text_append(text, "oPos");
		else
			xgpu_text_append(text, "r%lu", index);
		break;
	case _mux_input:
		xgpu_text_append(text, "v%lu", field(instruction, 1, 9, 4));
		break;
	case _mux_constant:
		if (relative)
			xgpu_text_append(text, "c[clamp(a0 + %lu, 0, %d)]", field(instruction, 1, 13, 8), XGPU_VERTEX_CONSTANT_COUNT - 1);
		else
			xgpu_text_append(text, "c[%lu]", field(instruction, 1, 13, 8));
		break;
	default:
		xgpu_text_append(text, "vec4(0.0)");
		break;
	}
	xgpu_text_append(text, ".%c%c%c%c",
		swizzle_names[swizzle[0]], swizzle_names[swizzle[1]], swizzle_names[swizzle[2]], swizzle_names[swizzle[3]]);
}

static const char shader_prologue[] =
#ifdef HALO_GLES
	/* the #version line comes first, from the context's capabilities */
	"precision highp float;\n"
	"precision highp int;\n"
#else
	"#version 450 core\n"
#endif
	"uniform vec4 c[192];\n"
	"uniform vec4 viewport_scale;\n"
	"uniform vec4 viewport_offset;\n"
	"uniform float point_size;\n"
	/* columns the menus shift by to center on a wide screen (d3d8_gl.c) */
	"uniform float screen_offset;\n"
	"out vec4 xD0;\n"
	"out vec4 xD1;\n"
	"out vec4 xB0;\n"
	"out vec4 xB1;\n"
	"out vec4 xT0;\n"
	"out vec4 xT1;\n"
	"out vec4 xT2;\n"
	"out vec4 xT3;\n"
	"out float xFog;\n"
	"invariant gl_Position;\n"
	"vec4 unpack_normpacked3(uint p)\n"
	"{\n"
	"	int x = int(p << 21) >> 21;\n"
	"	int y = int(p << 10) >> 21;\n"
	"	int z = int(p) >> 22;\n"
	"	return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);\n"
	"}\n"
	"vec4 nv2a_rcc(float x)\n"
	"{\n"
	"	float r = 1.0 / x;\n"
	"	if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);\n"
	"	else r = clamp(r, -1.884467e+19, -5.42101e-20);\n"
	"	return vec4(r);\n"
	"}\n"
	"vec4 nv2a_exp(float x)\n"
	"{\n"
	"	return vec4(exp2(floor(x)), fract(x), exp2(x), 1.0);\n"
	"}\n"
	"vec4 nv2a_log(float x)\n"
	"{\n"
	"	x = abs(x);\n"
	"	if (x == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);\n"
	"	float e = floor(log2(x));\n"
	"	return vec4(e, x / exp2(e), log2(x), 1.0);\n"
	"}\n"
	"vec4 nv2a_lit(vec4 s)\n"
	"{\n"
	"	float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
	"	return vec4(1.0, max(s.x, 0.0), specular, 1.0);\n"
	"}\n";

char *nv2a_vertex_shader_to_glsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask)
{
	struct xgpu_text text = { 0 };
	unsigned long index;

#ifdef HALO_GLES
	xgpu_text_append(&text, "#version %s\n", xgpu_capabilities.shading_language);
#endif
	xgpu_text_append(&text, "%s", shader_prologue);
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "layout(location = %lu) in uint v%lu_packed;\n", index, index);
		else
			xgpu_text_append(&text, "layout(location = %lu) in vec4 v%lu_in;\n", index, index);
	}

	xgpu_text_append(&text, "void main()\n{\n");
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "\tvec4 v%lu = unpack_normpacked3(v%lu_packed);\n", index, index);
		else
			xgpu_text_append(&text, "\tvec4 v%lu = v%lu_in;\n", index, index);
	}
	xgpu_text_append(&text,
		"\tvec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);\n"
		"\tvec4 r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0), r7 = vec4(0.0);\n"
		"\tvec4 r8 = vec4(0.0), r9 = vec4(0.0), r10 = vec4(0.0), r11 = vec4(0.0);\n"
		"\tvec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oFog = vec4(1.0), oPts = vec4(point_size), oUnused = vec4(0.0);\n"
		"\tint a0 = 0;\n"
		"\tvec4 A, B, C, mac, ilu;\n");
#ifdef HALO_GLES
	xgpu_text_append(&text, "\tvec4 clip_position = vec4(0.0);\n\tbool clip_captured = false;\n");
#endif

	for (index = 0; index < instruction_count; index++)
	{
		const DWORD *instruction = instructions + index * 4;
		unsigned long mac = field(instruction, 1, 21, 4);
		unsigned long ilu = field(instruction, 1, 25, 3);
		unsigned long mac_mask = field(instruction, 3, 24, 4);
		unsigned long temporary = field(instruction, 3, 20, 4);
		unsigned long ilu_mask = field(instruction, 3, 16, 4);
		unsigned long output_mask = field(instruction, 3, 12, 4);
		unsigned long output_is_register = field(instruction, 3, 11, 1);
		unsigned long output_address = field(instruction, 3, 3, 8);
		unsigned long output_from_ilu = field(instruction, 3, 2, 1);
		int relative = (int)field(instruction, 3, 1, 1);
		char mask[5];

		xgpu_text_append(&text, "\t/* %lu */\n", index);
		xgpu_text_append(&text, "\tA = "); operand(&text, instruction, 'A', relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tB = "); operand(&text, instruction, 'B', relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tC = "); operand(&text, instruction, 'C', relative); xgpu_text_append(&text, ";\n");

		switch (mac)
		{
		case _mac_nop: break;
		case _mac_mov: xgpu_text_append(&text, "\tmac = A;\n"); break;
		case _mac_mul: xgpu_text_append(&text, "\tmac = A * B;\n"); break;
		case _mac_add: xgpu_text_append(&text, "\tmac = A + C;\n"); break;
		case _mac_mad: xgpu_text_append(&text, "\tmac = A * B + C;\n"); break;
		case _mac_dp3: xgpu_text_append(&text, "\tmac = vec4(dot(A.xyz, B.xyz));\n"); break;
		case _mac_dph: xgpu_text_append(&text, "\tmac = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
		case _mac_dp4: xgpu_text_append(&text, "\tmac = vec4(dot(A, B));\n"); break;
		case _mac_dst: xgpu_text_append(&text, "\tmac = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
		case _mac_min: xgpu_text_append(&text, "\tmac = min(A, B);\n"); break;
		case _mac_max: xgpu_text_append(&text, "\tmac = max(A, B);\n"); break;
		case _mac_slt: xgpu_text_append(&text, "\tmac = vec4(lessThan(A, B));\n"); break;
		case _mac_sge: xgpu_text_append(&text, "\tmac = vec4(greaterThanEqual(A, B));\n"); break;
		case _mac_arl: xgpu_text_append(&text, "\tmac = A;\n"); break;
		default: xgpu_text_append(&text, "\tmac = vec4(0.0);\n"); break;
		}
		switch (ilu)
		{
		case _ilu_nop: break;
		case _ilu_mov: xgpu_text_append(&text, "\tilu = C;\n"); break;
		case _ilu_rcp: xgpu_text_append(&text, "\tilu = vec4(1.0 / C.x);\n"); break;
		case _ilu_rcc: xgpu_text_append(&text, "\tilu = nv2a_rcc(C.x);\n"); break;
		case _ilu_rsq: xgpu_text_append(&text, "\tilu = vec4(inversesqrt(abs(C.x)));\n"); break;
		case _ilu_exp: xgpu_text_append(&text, "\tilu = nv2a_exp(C.x);\n"); break;
		case _ilu_log: xgpu_text_append(&text, "\tilu = nv2a_log(C.x);\n"); break;
		case _ilu_lit: xgpu_text_append(&text, "\tilu = nv2a_lit(C);\n"); break;
		default: xgpu_text_append(&text, "\tilu = vec4(0.0);\n"); break;
		}
#ifdef HALO_GLES
		/* the screen-space conversion takes the reciprocal of the clip-space
		position's w (rcc of r12.w); keep the position it converts */
		if (ilu == _ilu_rcc && field(instruction, 3, 28, 2) == _mux_temporary &&
			((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2)) == 12)
		{
			xgpu_text_append(&text, "\tclip_position = oPos;\n\tclip_captured = true;\n");
		}
#endif

		/* results are written only after both units have read their inputs */
		if (mac == _mac_arl)
		{
			xgpu_text_append(&text, "\ta0 = int(floor(mac.x + 0.001));\n");
		}
		else if (mac != _mac_nop && mac_mask)
		{
			write_mask(mac_mask, mask);
			if (temporary == 12)
				xgpu_text_append(&text, "\toPos.%s = mac.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = mac.%s;\n", temporary, mask, mask);
		}
		if (ilu != _ilu_nop && ilu_mask)
		{
			unsigned long ilu_temporary = mac != _mac_nop ? 1 : temporary;

			write_mask(ilu_mask, mask);
			if (ilu_temporary == 12)
				xgpu_text_append(&text, "\toPos.%s = ilu.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = ilu.%s;\n", ilu_temporary, mask, mask);
		}
		if (output_mask && (output_from_ilu ? ilu : mac) != 0)
		{
			const char *source = output_from_ilu ? "ilu" : "mac";

			write_mask(output_mask, mask);
			if (output_is_register)
				xgpu_text_append(&text, "\t%s.%s = %s.%s;\n", output_name(output_address), mask, source, mask);
			/* writes to constant memory are not used by Halo's shaders */
		}
		if (field(instruction, 3, 0, 1))
			break;
	}

	xgpu_text_append(&text,
		"\t/* undo the screen-space conversion done with c[-38] and c[-37] */\n"
		"\tvec3 scale = vec3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,\n"
		"\t\tviewport_scale.y != 0.0 ? viewport_scale.y : 1.0,\n"
		"\t\tviewport_scale.z != 0.0 ? viewport_scale.z : 1.0);\n"
		/* Direct3D 8 puts pixel centres on integer screen coordinates (the
		game offsets its screen-space quads by -0.5 to match), OpenGL on
		half-integers */
#ifdef HALO_GLES
		/* The conversion is screen = clip * c[-38] * rcc(w) + c[-37]; undoing
		it by multiplying by w again is lossy near the camera plane, where
		rcc clamps and 1/w rounds differently on each GPU (Mali put vertices
		of the first-person weapon at the vanishing point). Where the clip
		position was kept, the same result is computed without dividing. */
		"\tif (clip_captured)\n"
		"\t\tgl_Position = vec4((clip_position.xyz * c[%d].xyz + (c[%d].xyz + vec3(0.5 + screen_offset, 0.5, 0.0)\n"
		"\t\t\t- viewport_offset.xyz) * clip_position.w) / scale, clip_position.w);\n"
		"\telse\n"
		"\t\tgl_Position = vec4((vec3(oPos.xy + vec2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale * oPos.w, oPos.w);\n"
#else
		"\tvec3 ndc = (vec3(oPos.xy + vec2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale;\n"
		"\tgl_Position = vec4(ndc * oPos.w, oPos.w);\n"
#endif
#ifdef HALO_GLES
		/* what glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) does on desktop
		GL: rows from the top, depth 0..1 */
		"\tgl_Position.y = -gl_Position.y;\n"
		"\tgl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
#endif
		"\tgl_PointSize = oPts.x;\n"
		"\txD0 = clamp(oD0, 0.0, 1.0);\n"
		"\txD1 = clamp(oD1, 0.0, 1.0);\n"
		"\txB0 = clamp(oB0, 0.0, 1.0);\n"
		"\txB1 = clamp(oB1, 0.0, 1.0);\n"
		"\txT0 = oT0;\n"
		"\txT1 = oT1;\n"
		"\txT2 = oT2;\n"
		"\txT3 = oT3;\n"
		"\txFog = oFog.x;\n"
		"}\n"
#ifdef HALO_GLES
		, XGPU_VERTEX_CONSTANT_BIAS - 38, XGPU_VERTEX_CONSTANT_BIAS - 37
#endif
		);
	return text.buffer;
}
