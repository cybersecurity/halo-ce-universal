/*
WEB_STUBS.C

What the platform layer links against that a browser does not have: the
router (UPnP) and Discord for internet play, which stays off in the web
build (web_main.c), a C library function Emscripten names differently and
an MSVC intrinsic clang knows only on x86 and ARM.
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef unsigned int posix_ulong;

/* ---------- internet play (p2p.c) */

int posix_upnp_forward_udp(unsigned short port, posix_ulong *external_address, unsigned short *external_port,
	char *error, int error_size)
{
	(void)port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
	{
		strncpy(error, "UPnP is not available in a browser", (size_t)error_size - 1);
		error[error_size - 1] = '\0';
	}
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}

void p2p_discord_update(void)
{
}

void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count)
{
	(void)secret;
	(void)player_count;
	(void)maximum_player_count;
}

/* ---------- the C library (posix_net.c) */

ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
	unsigned char *cursor = buffer;
	size_t left = size;

	(void)flags;
	while (left)
	{
		/* getentropy gives at most 256 bytes at a time */
		size_t chunk = left > 256 ? 256 : left;

		if (getentropy(cursor, chunk) != 0)
			return size - left ? (ssize_t)(size - left) : -1;
		cursor += chunk;
		left -= chunk;
	}
	return (ssize_t)size;
}

/* ---------- MSVC (scenario.c): a compiler barrier */

void _ReadWriteBarrier(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}
