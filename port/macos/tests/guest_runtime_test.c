/*
GUEST_RUNTIME_TEST.C

A guest image that tests the macOS port's runtime without the game or its
data: musl in the guest (formatting, memory, threads, files, time, maths),
the host's system calls, the thunks between guest and host (pointers, the
directory handles), the rebased code of the native build
(tools/macos_arm64_rebase.py: function pointers, stack addresses, atomics)
and the socket layer. port/macos/tests/run_guest_tests.sh builds and runs
it; it prints each check and exits with the number that failed.
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"

static int failures, checks;

static void check(int condition, const char *format, ...)
{
	va_list arguments;

	checks++;
	if (!condition)
		failures++;
	printf("%s ", condition ? "PASS" : "FAIL");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
	fflush(stdout);
}

/* ---------- formatting and parsing */

static void test_formatting(void)
{
	char buffer[256];
	double value;

	snprintf(buffer, sizeof(buffer), "%d %u %lld %s %.3f %e %x %c %5.1f", -42, 42u, -1234567890123LL, "text",
		3.14159, 1.0e-10, 0xbeef, 'Z', 2.25);
	check(!strcmp(buffer, "-42 42 -1234567890123 text 3.142 1.000000e-10 beef Z   2.2"), "snprintf: %s", buffer);
	value = strtod("123.456e2", NULL);
	check(fabs(value - 12345.6) < 1e-9, "strtod: %f", value);
	{
		int a = 0;
		float b = 0;
		char word[16] = "";

		check(sscanf("17 2.5 hello", "%d %f %15s", &a, &b, word) == 3 && a == 17 && b == 2.5f && !strcmp(word, "hello"),
			"sscanf: %d %g %s", a, b, word);
	}
}

/* ---------- memory */

static void test_memory(void)
{
	void *blocks[512];
	unsigned sizes[512];
	unsigned seed = 12345, index, round;
	int good = 1;

	for (round = 0; round < 8; round++)
	{
		for (index = 0; index < 512; index++)
		{
			seed = seed * 1103515245u + 12345u;
			sizes[index] = (seed >> 8) % (index % 64 == 0 ? 4u * 1024 * 1024 : 4096u) + 1;
			blocks[index] = malloc(sizes[index]);
			if (!blocks[index])
			{
				good = 0;
				continue;
			}
			memset(blocks[index], (int)(index & 0xff), sizes[index]);
		}
		for (index = 0; index < 512; index++)
		{
			unsigned char *bytes = blocks[index];

			if (bytes && (bytes[0] != (index & 0xff) || bytes[sizes[index] - 1] != (index & 0xff)))
				good = 0;
			free(blocks[index]);
		}
	}
	check(good, "malloc/free: 4096 blocks up to 4 MB, contents intact");
	{
		char *big = calloc(64, 1024 * 1024);

		check(big && big[0] == 0 && big[64 * 1024 * 1024 - 1] == 0, "calloc 64 MB");
		free(big);
	}
}

/* ---------- threads, locks, thread-local storage */

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static long counter;
static volatile int atomic_counter;
static int turn;
static __thread int thread_value;

static void *count_thread(void *argument)
{
	int index;

	thread_value = (int)(long)argument;
	for (index = 0; index < 20000; index++)
	{
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
		__atomic_fetch_add(&atomic_counter, 1, __ATOMIC_SEQ_CST);
	}
	return (void *)(long)(thread_value * 2);
}

static void *ping_thread(void *argument)
{
	int index;

	(void)argument;
	for (index = 0; index < 200; index++)
	{
		pthread_mutex_lock(&counter_lock);
		while (turn != 1)
			pthread_cond_wait(&turn_changed, &counter_lock);
		turn = 0;
		pthread_cond_signal(&turn_changed);
		pthread_mutex_unlock(&counter_lock);
	}
	return NULL;
}

static void test_threads(void)
{
	pthread_t threads[8];
	long index;
	int returns_good = 1;

	thread_value = 99;
	for (index = 0; index < 8; index++)
		pthread_create(&threads[index], NULL, count_thread, (void *)(index + 1));
	for (index = 0; index < 8; index++)
	{
		void *result = NULL;

		pthread_join(threads[index], &result);
		if ((long)result != (index + 1) * 2)
			returns_good = 0;
	}
	check(counter == 8 * 20000 && atomic_counter == 8 * 20000, "8 threads: mutex count %ld, atomic count %d",
		counter, atomic_counter);
	check(returns_good, "pthread_join returns each thread's result");
	check(thread_value == 99, "thread-local storage kept per thread (%d)", thread_value);
	{
		pthread_t ping;

		pthread_create(&ping, NULL, ping_thread, NULL);
		for (index = 0; index < 200; index++)
		{
			pthread_mutex_lock(&counter_lock);
			turn = 1;
			pthread_cond_signal(&turn_changed);
			while (turn != 0)
				pthread_cond_wait(&turn_changed, &counter_lock);
			pthread_mutex_unlock(&counter_lock);
		}
		pthread_join(ping, NULL);
		check(1, "condition variable ping-pong, 200 rounds");
	}
	{
		struct timespec until;
		int result;

		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 50 * 1000000L;
		if (until.tv_nsec >= 1000000000L)
		{
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		pthread_mutex_lock(&counter_lock);
		result = pthread_cond_timedwait(&turn_changed, &counter_lock, &until);
		pthread_mutex_unlock(&counter_lock);
		check(result == ETIMEDOUT, "pthread_cond_timedwait times out (%d)", result);
	}
}

/* ---------- files and directories */

static void test_files(void)
{
	const char *directory = "guest_test_files";
	char path[256];
	FILE *file;
	struct stat information;
	char buffer[64];
	int found = 0;
	DIR *listing;
	struct dirent *entry;

	mkdir(directory, 0755);
	snprintf(path, sizeof(path), "%s/data.bin", directory);
	file = fopen(path, "wb");
	check(file != NULL, "fopen for writing (%s)", strerror(errno));
	if (!file)
		return;
	fwrite("0123456789abcdef", 1, 16, file);
	fclose(file);
	check(stat(path, &information) == 0 && information.st_size == 16 && S_ISREG(information.st_mode),
		"stat: size %lld", (long long)information.st_size);
	file = fopen(path, "rb");
	fseek(file, 10, SEEK_SET);
	memset(buffer, 0, sizeof(buffer));
	fread(buffer, 1, 6, file);
	check(!strcmp(buffer, "abcdef") && ftell(file) == 16, "fseek/fread/ftell: %s", buffer);
	fclose(file);
	listing = opendir(directory);
	while (listing && (entry = readdir(listing)))
		found += !strcmp(entry->d_name, "data.bin");
	if (listing)
		closedir(listing);
	check(found == 1, "opendir/readdir finds the file");
	{
		char renamed[256];

		snprintf(renamed, sizeof(renamed), "%s/renamed.bin", directory);
		check(rename(path, renamed) == 0 && access(renamed, F_OK) == 0 && access(path, F_OK) != 0, "rename");
		check(unlink(renamed) == 0 && errno == errno, "unlink");
		errno = 0;
		check(open(renamed, O_RDONLY) < 0 && errno == ENOENT, "a missing file is ENOENT (%d)", errno);
	}
	/* the host's directory reading, through a handle */
	{
		void *handle;
		char name[128];
		struct posix_file_information host_information;

		snprintf(path, sizeof(path), "%s/second.txt", directory);
		file = fopen(path, "w");
		fputs("x", file);
		fclose(file);
		handle = posix_directory_open(directory);
		found = 0;
		while (handle && posix_directory_next(handle, name, sizeof(name)))
			found += !strcmp(name, "second.txt");
		if (handle)
			posix_directory_close(handle);
		check(handle != NULL && found == 1, "posix_directory_* through a handle");
		check(posix_stat(path, &host_information) == 0 && host_information.size_low == 1, "posix_stat");
		unlink(path);
	}
	check(rmdir(directory) == 0, "rmdir");
	{
		char executable[512];
		ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);

		if (length > 0)
			executable[length] = 0;
		check(length > 0 && strstr(executable, "/halo"), "/proc/self/exe: %s", length > 0 ? executable : "");
	}
	check(getenv("HALO_BASE_PATH") != NULL, "HALO_BASE_PATH: %s", getenv("HALO_BASE_PATH"));
}

/* ---------- time */

static void test_time(void)
{
	struct timespec before, after, pause = { 0, 20 * 1000000L };
	long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &before);
	nanosleep(&pause, NULL);
	clock_gettime(CLOCK_MONOTONIC, &after);
	elapsed = (after.tv_sec - before.tv_sec) * 1000L + (after.tv_nsec - before.tv_nsec) / 1000000L;
	check(elapsed >= 19 && elapsed < 500, "nanosleep 20 ms took %ld ms", elapsed);
	check(time(NULL) > 1700000000, "time() is %ld", (long)time(NULL));
}

/* ---------- code: function pointers, the stack, maths */

static int compare_ints(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int recurse(int depth, volatile int *parent)
{
	volatile int frame[256];

	frame[0] = parent ? parent[0] + 1 : 0;
	return depth ? recurse(depth - 1, frame) : frame[0];
}

static void test_code(void)
{
	int values[1000], index, sorted = 1, key = 777;
	unsigned seed = 7;
	int *found;
	double (*functions[3])(double) = { sin, cos, sqrt };
	double results[3];

	for (index = 0; index < 1000; index++)
	{
		seed = seed * 1103515245u + 12345u;
		values[index] = (int)((seed >> 8) % 100000);
	}
	values[500] = key;
	qsort(values, 1000, sizeof(values[0]), compare_ints);
	for (index = 1; index < 1000; index++)
		sorted &= values[index - 1] <= values[index];
	found = bsearch(&key, values, 1000, sizeof(values[0]), compare_ints);
	check(sorted && found && *found == key, "qsort/bsearch with a comparator (function pointer calls)");
	for (index = 0; index < 3; index++)
		results[index] = functions[index](2.0);
	check(fabs(results[0] - 0.9092974268) < 1e-9 && fabs(results[1] + 0.4161468365) < 1e-9 &&
		fabs(results[2] - 1.4142135624) < 1e-9, "sin/cos/sqrt through a table: %f %f %f",
		results[0], results[1], results[2]);
	check(fabs(pow(2.0, 10.5) - 1448.1546878700) < 1e-6 && fabsf(atan2f(1.0f, 1.0f) - 0.7853982f) < 1e-6f,
		"pow/atan2f");
	check(recurse(2000, NULL) == 2000, "2000 nested frames of 1 KB on the guest stack");
	{
		/* a pointer to the stack passed around as a 32-bit value */
		char local[32];
		char *copy;
		unsigned int as_integer = (unsigned int)(unsigned long)local;

		copy = (char *)(unsigned long)as_integer;
		strcpy(copy, "stack");
		check(!strcmp(local, "stack"), "a stack address kept as a 32-bit integer");
	}
}

/* ---------- sockets (the host's, with Winsock-style addresses) */

struct test_address
{
	unsigned short family;
	unsigned short port;
	unsigned int address;
	unsigned char zero[8];
};

static void test_sockets(void)
{
	int receiver = posix_socket(2, 2, 17), sender = posix_socket(2, 2, 17);
	struct test_address bound = { 2, 0, 0, { 0 } }, to, from;
	int length = sizeof(bound), received, from_length = sizeof(from);
	char buffer[64];

	bound.address = 0x0100007f; /* 127.0.0.1 */
	check(receiver >= 0 && sender >= 0, "UDP sockets");
	check(posix_socket_bind(receiver, &bound, sizeof(bound)) == 0, "bind 127.0.0.1:0 (error %d)",
		posix_socket_last_error());
	check(posix_socket_getsockname(receiver, &bound, &length) == 0 && bound.family == 2 && bound.port != 0,
		"getsockname: family %u port %u", bound.family, (unsigned)((bound.port >> 8) | ((bound.port & 255) << 8)));
	to = bound;
	check(posix_socket_sendto(sender, "hello", 5, 0, &to, sizeof(to)) == 5, "sendto");
	memset(buffer, 0, sizeof(buffer));
	received = posix_socket_recvfrom(receiver, buffer, sizeof(buffer), 0, &from, &from_length);
	check(received == 5 && !strcmp(buffer, "hello") && from.family == 2 && from.address == 0x0100007f,
		"recvfrom: \"%s\" from family %u", buffer, from.family);
	posix_socket_close(receiver);
	posix_socket_close(sender);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("guest runtime test (%s)\n", sizeof(void *) == 4 ? "32-bit pointers" : "?");
	test_formatting();
	test_memory();
	test_threads();
	test_files();
	test_time();
	test_code();
	test_sockets();
	printf("%d of %d checks passed\n", checks - failures, checks);
	return failures;
}
