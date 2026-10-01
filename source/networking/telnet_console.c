/*
TELNET_CONSOLE.C

symbols in this file:
001201D0 00e0:
	_telnet_console_initialize (0000)
001202B0 0050:
	_telnet_console_dispose (0000)
00120300 00c0:
	_telnet_console_print (0000)
001203C0 01f0:
	_code_001203c0 (0000)
001205B0 0160:
	_telnet_console_process (0000)
00288D04 003e:
	??_C@_0DO@PCBIDEOI@create_transport_endpoint?$CI?$CJ?5fail@ (0000)
00288D44 0032:
	??_C@_0DC@CJAJBFKG@bind_endpoint?$CI?$CJ?5failed?5on?5telnet@ (0000)
00288D78 0034:
	??_C@_0DE@MBCCFOEL@listen_endpoint?$CI?$CJ?5failed?5on?5teln@ (0000)
00288DAC 0021:
	??_C@_0CB@NCDEMBCM@connection?5lost?5to?5telnet?5client@ (0000)
00288DD0 002f:
	??_C@_0CP@LBGDAMPB@?$AN?6overflowed?5client?5buffer?$DL?5rese@ (0000)
00288E00 0028:
	??_C@_0CI@BALEJICP@failed?5to?5write?5to?5telnet?5client@ (0000)
00288E28 000d:
	??_C@_0N@FIIHEHGK@?$AN?6goodbye?$CB?$AN?6?$AA@ (0000)
00288E38 0028:
	??_C@_0CI@BMDHLBAG@connection?5lost?5to?5telnet?5client@ (0000)
00288E60 001f:
	??_C@_0BP@BBNCCABM@error?5processing?5telnet?5client?$AA@ (0000)
00288E80 0048:
	??_C@_0EI@HONFGAEA@sorry?5?9?5the?5maximum?5number?5of?5cl@ (0000)
00288EC8 0021:
	??_C@_0CB@MOGHMNHN@Would?5you?5like?5to?5play?5a?5game?$DP?$AN?6@ (0000)
00456D00 008c:
	_telnet_console_globals (0000)
*/


/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_address_constants.h"
#include "bungie_net/network/transport_endpoint.h"
#include "hs/hs.h"
#include "networking/telnet_console.h"
#ifdef HALO_64BIT
#include "cseries/errors.h"
#endif

/* ---------- constants */

enum
{
	MAXIMUM_TELNET_CLIENTS = 1,
	TELNET_CLIENT_BUFFER_SIZE = 128,
	/* the Xbox's was 23 (telnet), which a program that is not the
	administrator cannot listen on (Linux, Android): the native builds' is
	debug.telnet_console_port */
	TELNET_CONSOLE_DEFAULT_PORT = 2323,
	_transport_endpoint_type_telnet = 0x12
};

/* ---------- macros */

/* ---------- structures */

struct telnet_client
{
	struct transport_endpoint *endpoint;
	char buffer[TELNET_CLIENT_BUFFER_SIZE];
};

struct telnet_console_globals
{
	struct transport_endpoint *listening_endpoint;
	struct telnet_client clients[MAXIMUM_TELNET_CLIENTS];
	boolean initialized;
};

/* ---------- prototypes */

/* the platform layer's (port/linux/src/port_config.c) */
int config_boolean(const char *name);
long config_integer(const char *name);

static boolean telnet_client_write(
	struct telnet_client *client,
	char const *string,
	long length);
static void telnet_client_disconnect(
	struct telnet_client *client);
static boolean process_telnet_client_buffer(
	char *buffer,
	long size,
	struct telnet_client *client);

/* ---------- globals */

static struct telnet_console_globals telnet_console_globals = {0};

/* ---------- public code */

void telnet_console_initialize(
	void)
{
	csmemset(&telnet_console_globals, 0, sizeof(telnet_console_globals));

	/* the native builds' console runs any script it is sent, with no
	password: only when asked for (debug.telnet_console), and only from
	this machine */
	if (!config_boolean("debug.telnet_console"))
		return;

	telnet_console_globals.listening_endpoint = create_transport_endpoint(_transport_endpoint_type_telnet);
	if (telnet_console_globals.listening_endpoint)
	{
		struct transport_address address = {{0}};

		address.address_length = IPV4_ADDRESS_LENGTH;
		address.address.long_words[0] = IPV4_LOOPBACK_ADDRESS;
		address.port = (word)config_integer("debug.telnet_console_port");
		if (!address.port)
			address.port = TELNET_CONSOLE_DEFAULT_PORT;

		if (bind_endpoint(telnet_console_globals.listening_endpoint, &address)==_transport_error_none)
		{
			if (listen_endpoint(telnet_console_globals.listening_endpoint)==_transport_error_none)
			{
				telnet_console_globals.initialized = TRUE;
			}
			else
			{
				error(2, "listen_endpoint() failed on telnet console endpoint");
				delete_transport_endpoint(telnet_console_globals.listening_endpoint);
				telnet_console_globals.listening_endpoint = NULL;
			}
		}
		else
		{
			error(2, "bind_endpoint() failed on telnet console endpoint");
			delete_transport_endpoint(telnet_console_globals.listening_endpoint);
			telnet_console_globals.listening_endpoint = NULL;
		}
	}
	else
	{
		error(2, "create_transport_endpoint() failed on telnet console endpoint");
	}

	return;
}

void telnet_console_dispose(
	void)
{
	if (telnet_console_globals.initialized)
	{
		if (telnet_console_globals.listening_endpoint)
			delete_transport_endpoint(telnet_console_globals.listening_endpoint);
		telnet_client_disconnect(telnet_console_globals.clients);
	}

	csmemset(&telnet_console_globals, 0, sizeof(telnet_console_globals));

	return;
}

void telnet_console_print(
	char *string)
{
	struct telnet_client *client = &telnet_console_globals.clients[0];

	/* (port: a failed write drops the client before it says so, since
	error() prints here again; a client that is not reading loses the line:
	telnet_client_write) */
	if (telnet_console_globals.initialized && string && string[0] && client->endpoint)
	{
		if (telnet_client_write(client, "\r\n", 2) &&
			telnet_client_write(client, string, csstrlen(string)) &&
			client->buffer[0])
		{
			telnet_client_write(client, client->buffer, csstrlen(client->buffer));
		}
	}

	return;
}

void telnet_console_process(
	void)
{
	if (telnet_console_globals.initialized)
	{
		char buffer[32];
		long count;

		if (endpoint_readable(telnet_console_globals.listening_endpoint, 0))
		{
			struct transport_endpoint *endpoint = accept_endpoint(telnet_console_globals.listening_endpoint);

			/* (a client that stops reading does not stall the game) */
			if (endpoint && set_endpoint_blocking(endpoint, FALSE) != _transport_error_none)
			{
				delete_transport_endpoint(endpoint);
				endpoint = NULL;
			}
			if (endpoint)
			{
				long client_index;

				for (client_index = 0; client_index<MAXIMUM_TELNET_CLIENTS; client_index++)
				{
					if (!telnet_console_globals.clients[client_index].endpoint)
					{
						if (write_endpoint(
							endpoint,
							"Would you like to play a game?\r\n",
							csstrlen("Would you like to play a game?\r\n"))<=0)
						{
							delete_transport_endpoint(endpoint);
						}
						else
						{
							telnet_console_globals.clients[client_index].endpoint = endpoint;
							telnet_console_globals.clients[client_index].buffer[0] = 0;
						}

						break;
					}
				}

				if (client_index==MAXIMUM_TELNET_CLIENTS)
				{
					write_endpoint(
						endpoint,
						"sorry - the maximum number of clients are already connected. goodbye!\r\n",
						csstrlen("sorry - the maximum number of clients are already connected. goodbye!\r\n"));
					delete_transport_endpoint(endpoint);
				}
			}
		}

		if (telnet_console_globals.clients[0].endpoint &&
			endpoint_readable(telnet_console_globals.clients[0].endpoint, 0))
		{
			count = read_endpoint(telnet_console_globals.clients[0].endpoint, buffer, sizeof(buffer));
			if (count>0)
			{
				if (!process_telnet_client_buffer(buffer, count, telnet_console_globals.clients))
				{
					telnet_client_disconnect(telnet_console_globals.clients);
					error(2, "error processing telnet client");
				}
			}
			else if (count!=_transport_result_operation_would_block)
			{
				/* (the client is dropped before error() prints to it) */
				telnet_client_disconnect(telnet_console_globals.clients);
				error(2, "connection lost to telnet client ('%s')", transport_error_to_string((short)count));
			}
		}
	}

	return;
}

/* ---------- private code */

/* port: the client's socket does not block (a client that stops reading
does not stall the game). What it cannot take now is dropped (a line of
text), a write sent in part goes on with the rest, and a failed write drops
the client before error() says so: error() prints to the console, and so
here again. TRUE when all of it was sent. */
static boolean telnet_client_write(
	struct telnet_client *client,
	char const *string,
	long length)
{
	long written = 0;

	while (client->endpoint && written<length)
	{
		long result = write_endpoint(client->endpoint, string+written, length-written);

		if (result>0)
		{
			written += result;
		}
		else if (result==_transport_result_operation_would_block)
		{
			return FALSE;
		}
		else
		{
			telnet_client_disconnect(client);
			error(2, "failed to write to telnet client ('%s')",
				transport_error_to_string((short)result));
			return FALSE;
		}
	}

	return client->endpoint && written==length;
}

/* (the endpoint is let go of before it is deleted: anything the deletion
prints finds no client) */
static void telnet_client_disconnect(
	struct telnet_client *client)
{
	struct transport_endpoint *endpoint = client->endpoint;

	client->endpoint = NULL;
	client->buffer[0] = 0;
	if (endpoint)
		delete_transport_endpoint(endpoint);

	return;
}

/* FALSE when the client was lost (it is then dropped) */
static boolean process_telnet_client_buffer(
	char *buffer,
	long size,
	struct telnet_client *client)
{
	long index;

	/* (a script run, or an error, prints to the client, which may lose it) */
	for (index = 0; client->endpoint && index<size; index++)
	{
		char *character = buffer+index;
		long length;

		if ((unsigned char)*character>0x7f)
			continue;

		if (isalnum(*character) || ispunct(*character) || *character==' ')
		{
			length = csstrlen(client->buffer)+1;
			if (length>=TELNET_CLIENT_BUFFER_SIZE)
			{
				client->buffer[0] = 0;
				telnet_client_write(
					client,
					"\r\noverflowed client buffer; resetting buffer\r\n",
					csstrlen("\r\noverflowed client buffer; resetting buffer\r\n"));

				return client->endpoint!=NULL;
			}

			client->buffer[length-1] = *character;
			client->buffer[length] = 0;
		}
		else
		{
			switch (*character)
			{
			case 10:
			case 13:
				if (client->buffer[0])
				{
					char expression[TELNET_CLIENT_BUFFER_SIZE];

					csstrncpy(expression, client->buffer, TELNET_CLIENT_BUFFER_SIZE-1);
					expression[TELNET_CLIENT_BUFFER_SIZE-1] = 0;
					client->buffer[0] = 0;

					if (hs_compile_and_evaluate(expression))
					{
						telnet_client_write(client, "\r\n", 2);
					}
				}
				continue;

			case 8:
				if (client->buffer[0])
				{
					length = csstrlen(client->buffer);
					if (length>0)
						client->buffer[length-1] = 0;
				}
				break;

			case 4:
				telnet_client_write(
					client,
					"\r\ngoodbye!\r\n",
					csstrlen("\r\ngoodbye!\r\n"));
				telnet_client_disconnect(client);

				return TRUE;

			default:
				continue;
			}
		}

		telnet_client_write(client, character, 1);
	}

	return client->endpoint!=NULL;
}
