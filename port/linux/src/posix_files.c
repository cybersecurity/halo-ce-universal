/*
POSIX_FILES.C

glibc file system helpers for the platform layer (see posix.h). Built with
the host ABI and _FILE_OFFSET_BITS=64.
*/

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix.h"

static void split64(unsigned long long value, posix_ulong *low, posix_ulong *high)
{
	*low = (posix_ulong)(value & 0xffffffffULL);
	*high = (posix_ulong)(value >> 32);
}

static void fill_information(const struct stat *st, struct posix_file_information *information)
{
	memset(information, 0, sizeof(*information));
	if (S_ISDIR(st->st_mode))
		information->flags |= _posix_file_is_directory;
	if (!(st->st_mode & S_IWUSR))
		information->flags |= _posix_file_is_read_only;
	split64((unsigned long long)st->st_size, &information->size_low, &information->size_high);
	/* Darwin exposes birth time; Linux uses the closest portable change time. */
#ifdef HALO_MACOS
	information->modification_seconds = (posix_ulong)st->st_mtimespec.tv_sec;
	information->modification_nanoseconds = (posix_ulong)st->st_mtimespec.tv_nsec;
	information->access_seconds = (posix_ulong)st->st_atimespec.tv_sec;
	information->access_nanoseconds = (posix_ulong)st->st_atimespec.tv_nsec;
	information->creation_seconds = (posix_ulong)st->st_birthtimespec.tv_sec;
	information->creation_nanoseconds = (posix_ulong)st->st_birthtimespec.tv_nsec;
#else
	information->modification_seconds = (posix_ulong)st->st_mtim.tv_sec;
	information->modification_nanoseconds = (posix_ulong)st->st_mtim.tv_nsec;
	information->access_seconds = (posix_ulong)st->st_atim.tv_sec;
	information->access_nanoseconds = (posix_ulong)st->st_atim.tv_nsec;
	/* Linux has no portable creation time; the change time is the closest */
	information->creation_seconds = (posix_ulong)st->st_ctim.tv_sec;
	information->creation_nanoseconds = (posix_ulong)st->st_ctim.tv_nsec;
#endif
}

int posix_stat(const char *path, struct posix_file_information *information)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_fstat(int descriptor, struct posix_file_information *information)
{
	struct stat st;

	if (fstat(descriptor, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds)
{
	struct timespec times[2];

	times[0].tv_sec = (time_t)access_seconds;
	times[0].tv_nsec = access_seconds ? (long)access_nanoseconds : UTIME_OMIT;
	times[1].tv_sec = (time_t)modification_seconds;
	times[1].tv_nsec = modification_seconds ? (long)modification_nanoseconds : UTIME_OMIT;
	return utimensat(AT_FDCWD, path, times, 0);
}

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	off_t offset = (off_t)(((unsigned long long)(posix_ulong)offset_high << 32) | (posix_ulong)offset_low);
	off_t result = lseek(descriptor, offset, whence);

	if (result == (off_t)-1)
		return -1;
	split64((unsigned long long)result, position_low, position_high);
	return 0;
}

int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	return ftruncate(descriptor, (off_t)(((unsigned long long)size_high << 32) | size_low));
}

int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high)
{
	struct statvfs st;

	if (statvfs(path, &st) != 0)
		return -1;
	split64((unsigned long long)st.f_bavail * st.f_frsize, free_low, free_high);
	split64((unsigned long long)st.f_blocks * st.f_frsize, total_low, total_high);
	return 0;
}

int posix_set_read_only(const char *path, int read_only)
{
	struct stat st;
	mode_t mode;

	if (stat(path, &st) != 0)
		return -1;
	mode = st.st_mode & 07777;
	mode = read_only ? (mode & ~(mode_t)0222) : (mode | S_IWUSR);
	return chmod(path, mode);
}

int posix_make_directory(const char *path)
{
#ifdef __ANDROID__
	/* readable by the shell user (adb), for managing saves in the app's
	external storage (port/android/host/host_main.c) */
	if (mkdir(path, 0775) != 0)
		return -1;
	chmod(path, 02775);
	return 0;
#else
	return mkdir(path, 0755);
#endif
}

#ifdef __LP64__
/* The Android port calls this file from 32-bit guest code, which cannot
hold a 64-bit DIR pointer: directory streams are small handles there. */
#include <pthread.h>

#define DIRECTORY_HANDLE_COUNT 64

static DIR *directory_handles[DIRECTORY_HANDLE_COUNT];
static pthread_mutex_t directory_handle_lock = PTHREAD_MUTEX_INITIALIZER;

static void *directory_handle_new(DIR *directory)
{
	unsigned long index;

	if (!directory)
		return NULL;
	pthread_mutex_lock(&directory_handle_lock);
	for (index = 0; index < DIRECTORY_HANDLE_COUNT; index++)
	{
		if (!directory_handles[index])
		{
			directory_handles[index] = directory;
			pthread_mutex_unlock(&directory_handle_lock);
			return (void *)(index + 1);
		}
	}
	pthread_mutex_unlock(&directory_handle_lock);
	closedir(directory);
	return NULL;
}

static DIR *directory_from_handle(void *handle, int release)
{
	unsigned long index = (unsigned long)handle - 1;
	DIR *directory = NULL;

	if (index >= DIRECTORY_HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&directory_handle_lock);
	directory = directory_handles[index];
	if (release)
		directory_handles[index] = NULL;
	pthread_mutex_unlock(&directory_handle_lock);
	return directory;
}
#else
#define directory_handle_new(directory) ((void *)(directory))
#define directory_from_handle(handle, release) ((DIR *)(handle))
#endif

void *posix_directory_open(const char *path)
{
	return directory_handle_new(opendir(path));
}

int posix_directory_next(void *directory, char *name, posix_ulong name_size)
{
	DIR *stream = directory_from_handle(directory, 0);
	struct dirent *entry;

	if (!stream)
		return 0;
	while ((entry = readdir(stream)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (strlen(entry->d_name) + 1 > name_size)
			continue;
		strcpy(name, entry->d_name);
		return 1;
	}
	return 0;
}

void posix_directory_close(void *directory)
{
	DIR *stream = directory_from_handle(directory, 1);

	if (stream)
		closedir(stream);
}

int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size)
{
	DIR *handle = opendir(*directory ? directory : ".");
	struct dirent *entry;
	int found = 0;

	if (!handle)
		return 0;
	while ((entry = readdir(handle)) != NULL)
	{
		if (!strcasecmp(entry->d_name, name) && strlen(entry->d_name) + 1 <= result_size)
		{
			strcpy(result, entry->d_name);
			found = 1;
			break;
		}
	}
	closedir(handle);
	return found;
}
