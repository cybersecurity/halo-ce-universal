/*
RASTERIZER_XBOX_VERTEX_SHADERS_INITIALIZE.C

symbols in this file:
00168070 0020:
	_code_00168070 (0000)
00168090 0010:
	_code_00168090 (0000)
001680A0 0260:
	_rasterizer_vertex_shaders_initialize (0000)
00168300 0050:
	_rasterizer_vertex_shaders_dispose (0000)
0029BF94 00e0:
	_rdata_0029bf94 (0000)
0029C074 0036:
	??_C@_0DG@DJPOAFHM@?$CD?$CD?$CD?5ERROR?5rasterizer_vertex_shad@ (0000)
0029C0B0 00e9:
	??_C@_0OJ@IGEKCPEI@IDirect3DDevice8_CreateVertexSha@ (0000)
0029C19C 002e:
	??_C@_0CO@JPPOFKGN@vertex_shader_table?$FLvertex_shade@ (0000)
0029C1CC 0035:
	??_C@_0DF@NLIMIMDJ@vertex_shader_table?$FLvertex_shade@ (0000)
0029C208 004b:
	??_C@_0EL@FLPDNHEI@c?3?2halo?2SOURCE?2rasterizer?2xbox?2r@ (0000)
0029C254 0033:
	??_C@_0DD@EOLJNOFA@?$CD?$CD?$CD?5ERROR?5rasterizer_vertex_shad@ (0000)
0029C288 006f:
	??_C@_0GP@DNJKFNGM@IDirect3DDevice8_DeleteVertexSha@ (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "rasterizer_xbox_vertex_shaders.h"
/* The January object retains exact out-of-line copies of the two D3D wrappers. */
#define D3DINLINE static
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

void rasterizer_error(
	long error_result,
	char const *format,
	...);

/* ---------- globals */

static unsigned long const vertex_shader_declarations[] =
{
	0x20000000, 0x40320000, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40210004, 0xFFFFFFFF,
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0x20000001, 0x4024000B, 0xFFFFFFFF,
	0x20000000, 0x40220000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0x20000001, 0x40160007, 0x40210008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};

/* ---------- public code */

/* Exact: the declaration table, the SDK wrappers and this initializer match January (an
earlier note here recorded a table-address load-schedule difference that no longer exists). */
boolean rasterizer_vertex_shaders_initialize(
	void)
{
	boolean success;
	long result;
	short vertex_shader_index;

	success = TRUE;
	for (vertex_shader_index = 0; vertex_shader_index < 67; vertex_shader_index++)
		vertex_shader_table[vertex_shader_index].declaration = NULL;

	vertex_shader_table[4].declaration = &vertex_shader_declarations[0x64 / sizeof(unsigned long)];
	vertex_shader_table[3].declaration = &vertex_shader_declarations[0x64 / sizeof(unsigned long)];
	vertex_shader_table[38].declaration = &vertex_shader_declarations[0x64 / sizeof(unsigned long)];
	vertex_shader_table[33].declaration = &vertex_shader_declarations[0x20 / sizeof(unsigned long)];
	vertex_shader_table[65].declaration = &vertex_shader_declarations[0x34 / sizeof(unsigned long)];
	vertex_shader_table[2].declaration = &vertex_shader_declarations[0x34 / sizeof(unsigned long)];
	vertex_shader_table[12].declaration = &vertex_shader_declarations[0x34 / sizeof(unsigned long)];
	vertex_shader_table[56].declaration = &vertex_shader_declarations[0x34 / sizeof(unsigned long)];
	vertex_shader_table[0].declaration = &vertex_shader_declarations[0x00 / sizeof(unsigned long)];
	vertex_shader_table[1].declaration = &vertex_shader_declarations[0x10 / sizeof(unsigned long)];
	vertex_shader_table[66].declaration = &vertex_shader_declarations[0x48 / sizeof(unsigned long)];
	vertex_shader_table[16].declaration = &vertex_shader_declarations[0x9C / sizeof(unsigned long)];
	vertex_shader_table[49].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[29].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[40].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[21].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[41].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[59].declaration = &vertex_shader_declarations[0x9C / sizeof(unsigned long)];
	vertex_shader_table[58].declaration = &vertex_shader_declarations[0x9C / sizeof(unsigned long)];
	vertex_shader_table[26].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[42].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[44].declaration = &vertex_shader_declarations[0x9C / sizeof(unsigned long)];
	vertex_shader_table[51].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[6].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[8].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[10].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[11].declaration = &vertex_shader_declarations[0x20 / sizeof(unsigned long)];
	vertex_shader_table[9].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[27].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[17].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[64].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[39].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[13].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[5].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[24].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[48].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[34].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[19].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[35].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[47].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[31].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[60].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[57].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[45].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[62].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[46].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[28].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[43].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[61].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[25].declaration = &vertex_shader_declarations[0x9C / sizeof(unsigned long)];
	vertex_shader_table[30].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[63].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[36].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[50].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[20].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[23].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[18].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[14].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[22].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[32].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[15].declaration = &vertex_shader_declarations[0x78 / sizeof(unsigned long)];
	vertex_shader_table[37].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[7].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[54].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[55].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[53].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];
	vertex_shader_table[52].declaration = &vertex_shader_declarations[0xC4 / sizeof(unsigned long)];

	for (vertex_shader_index = 0; vertex_shader_index < 67; vertex_shader_index++)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_vertex_shaders_initialize.c",
			242,
			vertex_shader_table[vertex_shader_index].declaration);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_vertex_shaders_initialize.c",
			243,
			vertex_shader_table[vertex_shader_index].code);
		result = IDirect3DDevice8_CreateVertexShader(
			global_d3d_device,
			vertex_shader_table[vertex_shader_index].declaration,
			vertex_shader_table[vertex_shader_index].code,
			&vertex_shader_table[vertex_shader_index].handle,
			0);
		if (success && result >= 0)
		{
			success = TRUE;
		}
		else
		{
			success = FALSE;
			rasterizer_error(
				result,
				"IDirect3DDevice8_CreateVertexShader(global_d3d_device, (DWORD*)vertex_shader_table[vertex_shader_index].declaration, (DWORD*)vertex_shader_table[vertex_shader_index].code, (DWORD*)&vertex_shader_table[vertex_shader_index].handle, 0)");
		}
	}

	if (!success)
		error(2, "### ERROR rasterizer_vertex_shaders_initialize failed");

	/* port: the model lighting programs, which can be lit for each pixel
	(d3d8_gl.c); 10 and 17 also light by the two point lights. 27, for
	single-node models, is laid out otherwise and stays lit for each vertex */
	if (success)
	{
		halo_vertex_shader_lighting(vertex_shader_table[9].handle, FALSE);
		halo_vertex_shader_lighting(vertex_shader_table[10].handle, TRUE);
		halo_vertex_shader_lighting(vertex_shader_table[17].handle, TRUE);
	}

	return success;
}

void rasterizer_vertex_shaders_dispose(
	void)
{
	boolean success;
	short vertex_shader_index;

	success = TRUE;
	for (vertex_shader_index = 0; vertex_shader_index < 67; vertex_shader_index++)
	{
		if (IDirect3DDevice8_DeleteVertexShader(
			global_d3d_device,
			vertex_shader_table[vertex_shader_index].handle) >= 0 && success)
		{
			success = TRUE;
		}
		else
		{
			success = FALSE;
			rasterizer_error(
				0,
				"IDirect3DDevice8_DeleteVertexShader(global_d3d_device, (DWORD)vertex_shader_table[vertex_shader_index].handle)");
		}
	}

	if (!success)
	{
		error(2, "### ERROR rasterizer_vertex_shaders_dispose failed");
	}
	return;
}

/* ---------- private code */
