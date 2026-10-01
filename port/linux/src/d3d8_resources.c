/*
D3D8_RESOURCES.C

Xbox Direct3D resources for the Linux build: creation, locking,
registration and release of textures, surfaces, vertex and index buffers
and palettes, plus the few D3DX helpers the game uses.

Resources keep the Xbox memory model: their data lives in the contiguous
window and their Data field holds its physical address (index buffers,
which the GPU never reads directly, hold a virtual one). Texel layout
follows xbox_textures.c, so locks and uploads agree.
*/

#include "xgpu.h"

#include <stdlib.h>
#include <string.h>

static void *resource_data(DWORD data)
{
	return data ? PLATFORM_PHYSICAL_TO_VIRTUAL(data) : NULL;
}

static void *allocate_resource_memory(unsigned long size)
{
	void *memory = platform_contiguous_alloc(size, D3DTEXTURE_ALIGNMENT,
		PLATFORM_ANY_PHYSICAL_ADDRESS, PAGE_READWRITE);

	if (!memory)
		platform_log("Direct3D: out of contiguous memory for a %lu byte resource", size);
	return memory;
}

static unsigned long floor_log2_unsigned(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

static BOOL format_is_linear(D3DFORMAT format)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT), 0, &description);
	return description.linear;
}

static unsigned long bytes_per_texel(D3DFORMAT format)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT), 0, &description);
	return description.pitch; /* width 1 */
}

/* ---------- surfaces used as the device's own targets */

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	unsigned long pitch = (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);
	void *memory = allocate_resource_memory(pitch * height);

	memset(surface, 0, sizeof(*surface));
	/* owned by the device: without D3DCOMMON_D3DCREATED, Release never frees it */
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = memory ? PLATFORM_VIRTUAL_TO_PHYSICAL(memory) : 0;
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

/* changes the size a surface describes, keeping its memory, which must
hold the new size (the back buffer, when the screen's width changes) */
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	unsigned long pitch = (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

/* ---------- registration and release */

void WINAPI D3DResource_Register(D3DResource *resource, void *base)
{
	/* D3DResource is opaque in C; every resource starts Common, Data, Lock */
	DWORD *fields = (DWORD *)resource;

#ifdef HALO_64BIT
	/* Data is an offset from base, or with no base already an Xbox virtual
	address; either way it becomes a physical address */
	fields[1] = ((base ? xbox_address(base) : 0) + fields[1]) & ~PLATFORM_CONTIGUOUS_BASE;
#else
	fields[1] = PLATFORM_VIRTUAL_TO_PHYSICAL((unsigned long)base + fields[1]);
#endif
}

ULONG WINAPI D3DResource_Release(D3DResource *resource)
{
	DWORD *fields = (DWORD *)resource;
	ULONG count = fields[0] & D3DCOMMON_REFCOUNT_MASK;

	if (count)
	{
		count--;
		fields[0] = (fields[0] & ~D3DCOMMON_REFCOUNT_MASK) | count;
	}
	/* resources the game built itself (not D3DCOMMON_D3DCREATED) are never freed here */
	if (!count && (fields[0] & D3DCOMMON_D3DCREATED))
	{
		if ((fields[0] & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_INDEXBUFFER)
#ifdef HALO_64BIT
			platform_contiguous_free(xbox_pointer(fields[1]));
#else
			free((void *)fields[1]);
#endif
		else if (fields[1])
			platform_contiguous_free(PLATFORM_PHYSICAL_TO_VIRTUAL(fields[1]));
#ifdef HALO_64BIT
		xbox_heap_free(resource);
#else
		free(resource);
#endif
	}
	return count;
}

BOOL WINAPI D3DResource_IsBusy(D3DResource *resource)
{
	(void)resource;
	return FALSE;
}

void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *resource)
{
	(void)resource;
}

/* ---------- textures */

static HRESULT create_texture(unsigned long width, unsigned long height, unsigned long depth, unsigned long levels,
	D3DFORMAT format, BOOL cube_map, D3DBaseTexture **result)
{
#ifdef HALO_64BIT
	D3DBaseTexture *texture = xbox_heap_allocate(sizeof(*texture), TRUE);
#else
	D3DBaseTexture *texture = calloc(1, sizeof(*texture));
#endif
	struct xgpu_texture_description description;
	unsigned long maximum_levels = floor_log2_unsigned(width > height ? width : height) + 1;
	void *memory;

	if (!texture)
		return E_OUTOFMEMORY;
	if (!levels || levels > maximum_levels)
		levels = maximum_levels;
	texture->Common = D3DCOMMON_TYPE_TEXTURE | D3DCOMMON_D3DCREATED | 1;
	if (format_is_linear(format))
	{
		unsigned long pitch = (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
			~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

		texture->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
		texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
	}
	else
	{
		texture->Format = (floor_log2_unsigned(depth) << D3DFORMAT_PSIZE_SHIFT) |
			(floor_log2_unsigned(height) << D3DFORMAT_VSIZE_SHIFT) |
			(floor_log2_unsigned(width) << D3DFORMAT_USIZE_SHIFT) |
			((DWORD)format << D3DFORMAT_FORMAT_SHIFT) |
			(levels << D3DFORMAT_MIPMAP_SHIFT) |
			((depth > 1 ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
			(cube_map ? D3DFORMAT_CUBEMAP : 0) |
			D3DFORMAT_DMACHANNEL_A;
		texture->Size = 0;
	}
	xgpu_texture_describe(texture->Format, texture->Size, &description);
	memory = allocate_resource_memory(xgpu_texture_face_size(&description) * (cube_map ? 6 : 1));
	if (!memory)
	{
#ifdef HALO_64BIT
		xbox_heap_free(texture);
#else
		free(texture);
#endif
		return E_OUTOFMEMORY;
	}
	texture->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(memory);
	*result = texture;
	return S_OK;
}

HRESULT WINAPI D3DDevice_CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, 1, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, D3DVolumeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, depth, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DCubeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(edge_length, edge_length, 1, levels, format, TRUE, (D3DBaseTexture **)texture);
}

static void lock_level(const DWORD *resource, unsigned long face, unsigned long level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle)
{
	struct xgpu_texture_description description;
	unsigned long pitch;
	char *bits;

	xgpu_texture_describe(resource[3], resource[4], &description);
	pitch = xgpu_texture_level_pitch(&description, level);
	bits = (char *)resource_data(resource[1]);
	if (bits)
		bits += face * xgpu_texture_face_size(&description) + xgpu_texture_level_offset(&description, level);
	if (rectangle && bits)
	{
		/* swizzled textures cannot be addressed by rectangle; only linear
		and compressed layouts have a meaningful row pitch */
		unsigned long bytes = pitch / level_dimension(description.width, level);

		if (description.compressed)
			bits += (rectangle->top / 4) * pitch + (rectangle->left / 4) * (pitch / ((level_dimension(description.width, level) + 3) / 4));
		else
			bits += rectangle->top * pitch + rectangle->left * bytes;
	}
	locked->Pitch = (INT)pitch;
	locked->pBits = bits;
}

void WINAPI D3DTexture_LockRect(D3DTexture *texture, UINT level, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, 0, level, locked, rectangle);
}

void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *texture, D3DCUBEMAP_FACES face, UINT level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)texture, (unsigned long)face, level, locked, rectangle);
}

void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *texture, UINT level, D3DLOCKED_BOX *locked,
	CONST D3DBOX *box, DWORD flags)
{
	const DWORD *resource = (const DWORD *)texture;
	struct xgpu_texture_description description;
	unsigned long row_pitch, slice;
	char *bits;

	(void)flags;
	xgpu_texture_describe(resource[3], resource[4], &description);
	row_pitch = xgpu_texture_level_pitch(&description, level);
	slice = row_pitch * level_dimension(description.height, level);
	bits = (char *)resource_data(resource[1]);
	if (bits)
		bits += xgpu_texture_level_offset(&description, level);
	if (box && bits)
		bits += box->Front * slice + box->Top * row_pitch + box->Left * (row_pitch / level_dimension(description.width, level));
	locked->RowPitch = (INT)row_pitch;
	locked->SlicePitch = (INT)slice;
	locked->pBits = bits;
}

static void describe_level(const DWORD *resource, unsigned long level, D3DSURFACE_DESC *description)
{
	struct xgpu_texture_description texture;
	unsigned long width, height;

	xgpu_texture_describe(resource[3], resource[4], &texture);
	width = level_dimension(texture.width, level);
	height = level_dimension(texture.height, level);
	memset(description, 0, sizeof(*description));
	description->Format = (D3DFORMAT)texture.format;
	description->Type = D3DRTYPE_SURFACE;
	description->Width = width;
	description->Height = height;
	description->Size = xgpu_texture_level_pitch(&texture, level) * (texture.compressed ? (height + 3) / 4 : height);
	description->MultiSampleType = D3DMULTISAMPLE_NONE;
}

void WINAPI D3DTexture_GetLevelDesc(D3DTexture *texture, UINT level, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)texture, level, description);
}

HRESULT WINAPI D3DTexture_GetSurfaceLevel(D3DTexture *texture, UINT level, D3DSurface **result)
{
	D3DBaseTexture *base = (D3DBaseTexture *)texture;
	struct xgpu_texture_description description;
#ifdef HALO_64BIT
	D3DSurface *surface = xbox_heap_allocate(sizeof(*surface), TRUE);
	unsigned int width, height;
#else
	D3DSurface *surface = calloc(1, sizeof(*surface));
	unsigned long width, height;
#endif

	if (!surface)
		return E_OUTOFMEMORY;
	xgpu_texture_describe(base->Format, base->Size, &description);
	width = level_dimension(description.width, level);
	height = level_dimension(description.height, level);
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = base->Data + xgpu_texture_level_offset(&description, level);
	surface->Format = (base->Format & ~(D3DFORMAT_USIZE_MASK | D3DFORMAT_VSIZE_MASK | D3DFORMAT_PSIZE_MASK | D3DFORMAT_MIPMAP_MASK)) |
		(floor_log2_unsigned(width) << D3DFORMAT_USIZE_SHIFT) |
		(floor_log2_unsigned(height) << D3DFORMAT_VSIZE_SHIFT) |
		(1 << D3DFORMAT_MIPMAP_SHIFT);
	surface->Size = base->Size;
	surface->Parent = base;
	*result = surface;
	return S_OK;
}

void WINAPI D3DSurface_GetDesc(D3DSurface *surface, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)surface, 0, description);
}

void WINAPI D3DSurface_LockRect(D3DSurface *surface, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((const DWORD *)surface, 0, 0, locked, rectangle);
}

/* ---------- vertex and index buffers */

HRESULT WINAPI D3DDevice_CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, D3DVertexBuffer **result)
{
#ifdef HALO_64BIT
	D3DVertexBuffer *buffer = xbox_heap_allocate(sizeof(*buffer), TRUE);
#else
	D3DVertexBuffer *buffer = calloc(1, sizeof(*buffer));
#endif
	void *memory;

	(void)usage;
	(void)fvf;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = allocate_resource_memory(length ? length : 1);
	if (!memory)
	{
#ifdef HALO_64BIT
		xbox_heap_free(buffer);
#else
		free(buffer);
#endif
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(memory);
	*result = buffer;
	return S_OK;
}

void WINAPI D3DVertexBuffer_Lock(D3DVertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags)
{
	(void)size;
	(void)flags;
	*data = buffer->Data ? (BYTE *)resource_data(buffer->Data) + offset : NULL;
}

HRESULT WINAPI D3DDevice_CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, D3DIndexBuffer **result)
{
#ifdef HALO_64BIT
	D3DIndexBuffer *buffer = xbox_heap_allocate(sizeof(*buffer), TRUE);
#else
	D3DIndexBuffer *buffer = calloc(1, sizeof(*buffer));
#endif
	void *memory;

	(void)usage;
	(void)format;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
#ifdef HALO_64BIT
	/* index data stays in ordinary memory on the Xbox; Data is virtual */
	memory = allocate_resource_memory(length ? length : 1);
#else
	/* index data stays in ordinary memory on the Xbox too; Data is virtual */
	memory = calloc(1, length ? length : 1);
#endif
	if (!memory)
	{
#ifdef HALO_64BIT
		xbox_heap_free(buffer);
#else
		free(buffer);
#endif
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED | 1;
#ifdef HALO_64BIT
	buffer->Data = xbox_address(memory);
#else
	buffer->Data = (DWORD)memory;
#endif
	*result = buffer;
	return S_OK;
}

/* ---------- palettes */

static unsigned long palette_entry_count(D3DPALETTESIZE size)
{
	switch (size)
	{
	case D3DPALETTE_128: return 128;
	case D3DPALETTE_64: return 64;
	case D3DPALETTE_32: return 32;
	default: return 256;
	}
}

HRESULT WINAPI D3DDevice_CreatePalette(D3DPALETTESIZE size, D3DPalette **result)
{
#ifdef HALO_64BIT
	D3DPalette *palette = xbox_heap_allocate(sizeof(*palette), TRUE);
#else
	D3DPalette *palette = calloc(1, sizeof(*palette));
#endif
	void *memory;

	if (!palette)
		return E_OUTOFMEMORY;
	/* always room for 256 entries, which the texture decoder may read */
	memory = allocate_resource_memory(256 * sizeof(D3DCOLOR));
	(void)palette_entry_count(size);
	if (!memory)
	{
#ifdef HALO_64BIT
		xbox_heap_free(palette);
#else
		free(palette);
#endif
		return E_OUTOFMEMORY;
	}
	palette->Common = D3DCOMMON_TYPE_PALETTE | D3DCOMMON_D3DCREATED | 1 | ((DWORD)size << D3DPALETTE_COMMON_PALETTESIZE_SHIFT);
	palette->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(memory);
	*result = palette;
	return S_OK;
}

void WINAPI D3DPalette_Lock(D3DPalette *palette, D3DCOLOR **colors, DWORD flags)
{
	(void)flags;
	*colors = (D3DCOLOR *)resource_data(palette->Data);
}

/* ---------- D3DX */

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f * near_plane / width;
	out->_22 = 2.0f * near_plane / height;
	out->_33 = far_plane / (far_plane - near_plane);
	out->_34 = 1.0f;
	out->_43 = near_plane * far_plane / (near_plane - far_plane);
	return out;
}

D3DXMATRIX *WINAPI D3DXMatrixOrthoLH(D3DXMATRIX *out, FLOAT width, FLOAT height, FLOAT near_plane, FLOAT far_plane)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f / width;
	out->_22 = 2.0f / height;
	out->_33 = 1.0f / (far_plane - near_plane);
	out->_43 = near_plane / (near_plane - far_plane);
	out->_44 = 1.0f;
	return out;
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *out, CONST D3DXVECTOR4 *vector, CONST D3DXMATRIX *matrix)
{
	D3DXVECTOR4 result;

	result.x = vector->x * matrix->_11 + vector->y * matrix->_21 + vector->z * matrix->_31 + vector->w * matrix->_41;
	result.y = vector->x * matrix->_12 + vector->y * matrix->_22 + vector->z * matrix->_32 + vector->w * matrix->_42;
	result.z = vector->x * matrix->_13 + vector->y * matrix->_23 + vector->z * matrix->_33 + vector->w * matrix->_43;
	result.w = vector->x * matrix->_14 + vector->y * matrix->_24 + vector->z * matrix->_34 + vector->w * matrix->_44;
	*out = result;
	return out;
}

HRESULT WINAPI D3DXGetErrorStringA(HRESULT error, LPSTR buffer, UINT buffer_length)
{
	if (buffer && buffer_length)
	{
		const char *text = error == S_OK ? "S_OK" :
			error == E_OUTOFMEMORY ? "E_OUTOFMEMORY" :
			error == E_FAIL ? "E_FAIL" : "Direct3D error";

		strncpy(buffer, text, buffer_length - 1);
		buffer[buffer_length - 1] = '\0';
	}
	return S_OK;
}
