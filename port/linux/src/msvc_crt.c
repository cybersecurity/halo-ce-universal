/*
MSVC_CRT.C

Microsoft C runtime functions the game (and its copies of libtiff and zlib)
call that glibc does not provide under the same names, plus the Xbox-path
aware fopen/open family that the game's headers redirect to (see
port/linux/include/stdio.h).
*/

#include "platform.h"
#include "posix.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

size_t malloc_usable_size(void *pointer);

/* the shim stdio.h maps these onto the MSVC names defined below */
#undef fdopen
#undef fileno

/* ---------- strings */

int _stricmp(const char *string1, const char *string2)
{
	for (;; string1++, string2++)
	{
		int c1 = tolower((unsigned char)*string1);
		int c2 = tolower((unsigned char)*string2);

		if (c1 != c2 || !c1)
			return c1 - c2;
	}
}

int _strnicmp(const char *string1, const char *string2, size_t count)
{
	for (; count; count--, string1++, string2++)
	{
		int c1 = tolower((unsigned char)*string1);
		int c2 = tolower((unsigned char)*string2);

		if (c1 != c2 || !c1)
			return c1 - c2;
	}
	return 0;
}

char *_strdup(const char *string)
{
	size_t length = strlen(string) + 1;
	char *copy = malloc(length);

	if (copy)
		memcpy(copy, string, length);
	return copy;
}

char *_strlwr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)tolower((unsigned char)*cursor);
	return string;
}

char *_strupr(char *string)
{
	char *cursor;

	for (cursor = string; *cursor; cursor++)
		*cursor = (char)toupper((unsigned char)*cursor);
	return string;
}

static char *unsigned_to_string(unsigned long value, char *string, int radix, int negative)
{
	char digits[36];
	int count = 0;
	char *cursor = string;

	if (radix < 2 || radix > 36)
	{
		*string = '\0';
		return string;
	}
	do
	{
		int digit = (int)(value % (unsigned long)radix);

		digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		value /= (unsigned long)radix;
	} while (value);
	if (negative)
		*cursor++ = '-';
	while (count)
		*cursor++ = digits[--count];
	*cursor = '\0';
	return string;
}

char *_ltoa(long value, char *string, int radix)
{
	/* MSVC only treats values as signed in base 10 */
	if (radix == 10 && value < 0)
		return unsigned_to_string(0UL - (unsigned long)value, string, radix, 1);
	return unsigned_to_string((unsigned long)value, string, radix, 0);
}

char *_itoa(int value, char *string, int radix)
{
	return _ltoa(value, string, radix);
}

char *_ultoa(unsigned long value, char *string, int radix)
{
	return unsigned_to_string(value, string, radix, 0);
}

/* _rotl, _rotr, _lrotl and _lrotr are clang builtins under -fms-extensions */

/* ---------- paths */

void _splitpath(const char *path, char *drive, char *directory, char *name, char *extension)
{
	const char *cursor = path;
	const char *last_separator = NULL;
	const char *last_dot = NULL;
	const char *scan;

	if (path[0] && path[1] == ':')
	{
		if (drive)
		{
			drive[0] = path[0];
			drive[1] = ':';
			drive[2] = '\0';
		}
		cursor += 2;
	}
	else if (drive)
	{
		drive[0] = '\0';
	}
	for (scan = cursor; *scan; scan++)
	{
		if (*scan == '\\' || *scan == '/')
			last_separator = scan;
		else if (*scan == '.')
			last_dot = scan;
	}
	if (last_dot && last_separator && last_dot < last_separator)
		last_dot = NULL;
	if (directory)
	{
		size_t length = last_separator ? (size_t)(last_separator - cursor + 1) : 0;

		if (length >= _MAX_DIR)
			length = _MAX_DIR - 1;
		memcpy(directory, cursor, length);
		directory[length] = '\0';
	}
	cursor = last_separator ? last_separator + 1 : cursor;
	if (name)
	{
		size_t length = last_dot ? (size_t)(last_dot - cursor) : strlen(cursor);

		if (length >= _MAX_FNAME)
			length = _MAX_FNAME - 1;
		memcpy(name, cursor, length);
		name[length] = '\0';
	}
	if (extension)
	{
		strncpy(extension, last_dot ? last_dot : "", _MAX_EXT - 1);
		extension[_MAX_EXT - 1] = '\0';
	}
}

void _makepath(char *path, const char *drive, const char *directory, const char *name, const char *extension)
{
	path[0] = '\0';
	if (drive && *drive)
	{
		strncat(path, drive, 1);
		strcat(path, ":");
	}
	if (directory && *directory)
	{
		size_t length;

		strcat(path, directory);
		length = strlen(path);
		if (path[length - 1] != '\\' && path[length - 1] != '/')
			strcat(path, "\\");
	}
	if (name)
		strcat(path, name);
	if (extension && *extension)
	{
		if (*extension != '.')
			strcat(path, ".");
		strcat(path, extension);
	}
}

char *_fullpath(char *absolute_path, const char *relative_path, size_t maximum_length)
{
	/* Xbox paths are already absolute */
	if (!absolute_path)
	{
		maximum_length = _MAX_PATH;
		absolute_path = malloc(maximum_length);
		if (!absolute_path)
			return NULL;
	}
	if (strlen(relative_path) + 1 > maximum_length)
	{
		errno = ERANGE;
		return NULL;
	}
	strcpy(absolute_path, relative_path);
	return absolute_path;
}

/* ---------- x87 floating point control */

/* MSVC abstract control bits <-> x87 control word bits */
static const struct
{
	unsigned int msvc;
	unsigned short x87;
} exception_masks[] =
{
	{ _EM_INVALID, 0x0001 },
	{ _EM_DENORMAL, 0x0002 },
	{ _EM_ZERODIVIDE, 0x0004 },
	{ _EM_OVERFLOW, 0x0008 },
	{ _EM_UNDERFLOW, 0x0010 },
	{ _EM_INEXACT, 0x0020 },
};

static unsigned int control_word_to_msvc(unsigned short word)
{
	unsigned int result = 0;
	unsigned int index;

	for (index = 0; index < sizeof(exception_masks) / sizeof(exception_masks[0]); index++)
	{
		if (word & exception_masks[index].x87)
			result |= exception_masks[index].msvc;
	}
	switch ((word >> 8) & 3)
	{
	case 0: result |= _PC_24; break;
	case 2: result |= _PC_53; break;
	default: result |= _PC_64; break;
	}
	result |= (unsigned int)((word >> 10) & 3) << 8; /* _RC_* share the encoding */
	if (word & 0x1000)
		result |= _IC_AFFINE;
	return result;
}

static unsigned short msvc_to_control_word(unsigned int value, unsigned short word)
{
	unsigned int index;

	word &= (unsigned short)~0x1f3f;
	for (index = 0; index < sizeof(exception_masks) / sizeof(exception_masks[0]); index++)
	{
		if (value & exception_masks[index].msvc)
			word |= exception_masks[index].x87;
	}
	switch (value & _MCW_PC)
	{
	case _PC_24: break;
	case _PC_53: word |= 2 << 8; break;
	default: word |= 3 << 8; break;
	}
	word |= (unsigned short)(((value & _MCW_RC) >> 8) << 10);
	if (value & _IC_AFFINE)
		word |= 0x1000;
	return word;
}

#ifdef HALO_WEB
/* WebAssembly always rounds to nearest and has no exception flags: the MSVC
control word is only remembered */
static unsigned int msvc_control_word = CW_DEFAULT;

unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	if (mask)
		msvc_control_word = (msvc_control_word & ~mask) | (new_value & mask);
	return msvc_control_word;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	return _control87(new_value, mask & ~_EM_DENORMAL);
}

unsigned int _statusfp(void)
{
	return 0;
}

unsigned int _clearfp(void)
{
	return 0;
}
#elif defined(HALO_ANDROID)
/* AArch64: the rounding mode lives in FPCR.RMode, the sticky exception
flags in FPSR. Precision control and exception unmasking have no
equivalent; the rest of the MSVC control word is only remembered. */
static unsigned int msvc_control_word = CW_DEFAULT;

unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	unsigned long long fpcr;

	if (mask)
	{
		msvc_control_word = (msvc_control_word & ~mask) | (new_value & mask);
		fpcr = __builtin_arm_rsr64("fpcr");
		fpcr &= ~(3ULL << 22);
		switch (msvc_control_word & _MCW_RC)
		{
		case _RC_UP: fpcr |= 1ULL << 22; break;
		case _RC_DOWN: fpcr |= 2ULL << 22; break;
		case _RC_CHOP: fpcr |= 3ULL << 22; break;
		default: break;
		}
		__builtin_arm_wsr64("fpcr", fpcr);
	}
	return msvc_control_word;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	/* _controlfp ignores the denormal mask */
	return _control87(new_value, mask & ~_EM_DENORMAL);
}

unsigned int _statusfp(void)
{
	unsigned long long fpsr = __builtin_arm_rsr64("fpsr");
	unsigned int result = 0;

	/* as the x87 status word's low bits: invalid, denormal, zero divide,
	overflow, underflow, precision */
	if (fpsr & 0x01) result |= 0x01;
	if (fpsr & 0x80) result |= 0x02;
	if (fpsr & 0x02) result |= 0x04;
	if (fpsr & 0x04) result |= 0x08;
	if (fpsr & 0x08) result |= 0x10;
	if (fpsr & 0x10) result |= 0x20;
	return result;
}

unsigned int _clearfp(void)
{
	unsigned int status = _statusfp();

	__builtin_arm_wsr64("fpsr", __builtin_arm_rsr64("fpsr") & ~0x9fULL);
	return status;
}
#else
/* x87: glibc's floating-point environment holds the control and status
words (fegetenv and fesetenv save and load the whole x87 environment, and
keep the SSE unit's rounding and masks in step) */
unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	fenv_t environment;
	unsigned int current;

	fegetenv(&environment);
	current = control_word_to_msvc(environment.__control_word);
	if (mask)
	{
		current = (current & ~mask) | (new_value & mask);
		environment.__control_word = msvc_to_control_word(current, environment.__control_word);
		fesetenv(&environment);
	}
	return current;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	/* _controlfp ignores the denormal mask */
	return _control87(new_value, mask & ~_EM_DENORMAL);
}

unsigned int _statusfp(void)
{
	fenv_t environment;

	fegetenv(&environment);
	return environment.__status_word & 0x3f;
}

unsigned int _clearfp(void)
{
	fenv_t environment;
	unsigned int status;

	fegetenv(&environment);
	status = environment.__status_word & 0x3f;
	/* the exception flags, and the summary and stack fault bits with them */
	environment.__status_word &= (unsigned short)~0xff;
	fesetenv(&environment);
	return status;
}

#endif

int _isnan(double value)
{
	return isnan(value);
}

int _finite(double value)
{
	return isfinite(value);
}

double _hypot(double x, double y)
{
	return hypot(x, y);
}

double _copysign(double x, double y)
{
	return copysign(x, y);
}

/* ---------- memory */

size_t _msize(void *pointer)
{
	return malloc_usable_size(pointer);
}

int *_errno(void)
{
	return &errno;
}

/* ---------- printf with MSVC length modifiers */

/* rewrite %I64 as %ll and drop %I32; everything else is shared with glibc */
static const char *translate_format(const char *format, char *buffer, size_t size)
{
	const char *cursor;
	size_t length = 0;

	if (!strstr(format, "I64") && !strstr(format, "I32"))
		return format;
	for (cursor = format; *cursor && length + 3 < size; cursor++)
	{
		buffer[length++] = *cursor;
		if (*cursor != '%')
			continue;
		if (cursor[1] == '%')
		{
			buffer[length++] = *++cursor;
			continue;
		}
		/* copy flags, width and precision */
		while (cursor[1] && strchr("-+ #0123456789.*", cursor[1]) && length + 3 < size)
			buffer[length++] = *++cursor;
		if (cursor[1] == 'I' && cursor[2] == '6' && cursor[3] == '4')
		{
			buffer[length++] = 'l';
			buffer[length++] = 'l';
			cursor += 3;
		}
		else if (cursor[1] == 'I' && cursor[2] == '3' && cursor[3] == '2')
		{
			cursor += 3;
		}
	}
	buffer[length] = '\0';
	return buffer;
}

int halo_linux_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vsnprintf(buffer, count, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vsprintf(char *buffer, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vsprintf(buffer, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vfprintf(stream, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vprintf(const char *format, va_list arguments)
{
	return halo_linux_vfprintf(stdout, format, arguments);
}

int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

/* ---------- files by Xbox path */

static const char *translated(const char *path, char *buffer, size_t size)
{
	platform_translate_path(path, buffer, size);
	return buffer;
}

FILE *halo_linux_fopen(const char *path, const char *mode)
{
	char host_path[1024];

	return fopen(translated(path, host_path, sizeof(host_path)), mode);
}

FILE *halo_linux_freopen(const char *path, const char *mode, FILE *stream)
{
	char host_path[1024];

	return freopen(path ? translated(path, host_path, sizeof(host_path)) : NULL, mode, stream);
}

int halo_linux_remove(const char *path)
{
	char host_path[1024];

	return remove(translated(path, host_path, sizeof(host_path)));
}

int halo_linux_rename(const char *old_path, const char *new_path)
{
	char host_old_path[1024], host_new_path[1024];

	return rename(translated(old_path, host_old_path, sizeof(host_old_path)),
		translated(new_path, host_new_path, sizeof(host_new_path)));
}

int halo_linux_open(const char *path, int flags, ...)
{
	char host_path[1024];
	int mode = 0;

	if (flags & O_CREAT)
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, int);
		va_end(arguments);
	}
	return open(translated(path, host_path, sizeof(host_path)), flags | O_CLOEXEC, mode ? mode : 0644);
}

int _mkdir(const char *path)
{
	char host_path[1024];

	return posix_make_directory(translated(path, host_path, sizeof(host_path)));
}

int _rmdir(const char *path)
{
	char host_path[1024];

	return rmdir(translated(path, host_path, sizeof(host_path)));
}

int _chdir(const char *path)
{
	char host_path[1024];

	return chdir(translated(path, host_path, sizeof(host_path)));
}

char *_getcwd(char *buffer, int maximum_length)
{
	return getcwd(buffer, (size_t)maximum_length);
}

static void stat_from_information(const struct posix_file_information *information, struct _stat *buffer)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->st_mode = (unsigned short)((information->flags & _posix_file_is_directory) ? _S_IFDIR : _S_IFREG);
	buffer->st_mode |= _S_IREAD;
	if (!(information->flags & _posix_file_is_read_only))
		buffer->st_mode |= _S_IWRITE;
	buffer->st_nlink = 1;
	buffer->st_size = information->size_high ? LONG_MAX : (long)information->size_low;
	buffer->st_atime = (time_t)information->access_seconds;
	buffer->st_mtime = (time_t)information->modification_seconds;
	buffer->st_ctime = (time_t)information->creation_seconds;
}

int _stat(const char *path, struct _stat *buffer)
{
	char host_path[1024];
	struct posix_file_information information;

	if (posix_stat(translated(path, host_path, sizeof(host_path)), &information) != 0)
		return -1;
	stat_from_information(&information, buffer);
	return 0;
}

int _fstat(int handle, struct _stat *buffer)
{
	struct posix_file_information information;

	if (posix_fstat(handle, &information) != 0)
		return -1;
	stat_from_information(&information, buffer);
	return 0;
}

long _filelength(int handle)
{
	struct posix_file_information information;

	if (posix_fstat(handle, &information) != 0)
		return -1;
	return information.size_high ? LONG_MAX : (long)information.size_low;
}

int _chsize(int handle, long size)
{
	return posix_truncate(handle, (unsigned long)size, 0);
}

FILE *_fdopen(int handle, const char *mode)
{
	return fdopen(handle, mode);
}

int _fileno(FILE *stream)
{
	return fileno(stream);
}

/* libtiff maps its POSIX calls onto the MSVC underscore names itself */

int _open(const char *path, int flags, ...)
{
	int mode = 0;

	if (flags & O_CREAT)
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, int);
		va_end(arguments);
	}
	return halo_linux_open(path, flags, mode);
}

int _close(int handle)
{
	return close(handle);
}

int _read(int handle, void *buffer, unsigned int count)
{
	return (int)read(handle, buffer, count);
}

int _write(int handle, const void *buffer, unsigned int count)
{
	return (int)write(handle, buffer, count);
}

long _lseek(int handle, long offset, int origin)
{
	return (long)lseek(handle, offset, origin);
}
