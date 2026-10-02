/* Thread-safe quick-play control; the engine consumes requests on its own thread. */
#ifdef HALO_QUICK_PLAY_TEST
#define EMSCRIPTEN_KEEPALIVE
double emscripten_get_now(void);
#else
#include <emscripten.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>

void web_js_post(int kind, const char *text);
int web_multiplayer_active(void);
static int cancel_requested;
static uint64_t migration_requested;
static uint64_t reconnect_requested;
static unsigned int local_address;
static unsigned int initial_epoch;
static int hold_requested;
static int background_active;
/* Give deferred network/menu teardown a few frames after a terminal event. */
static double background_drain_until;

EMSCRIPTEN_KEEPALIVE void web_quick_play_cancel(void)
{
	__atomic_store_n(&migration_requested, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&reconnect_requested, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&hold_requested, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cancel_requested, 1, __ATOMIC_RELEASE);
}

/* The page elects; only the game thread adopts or reconnects a live session.
   Role, epoch and target are one atomic request, never torn across elections. */
EMSCRIPTEN_KEEPALIVE void web_quick_play_migrate(int mode, unsigned int target, unsigned int epoch)
{
	if ((mode != 1 && mode != 2) || !epoch || epoch > 0x7fffffffU)
		return;
	__atomic_store_n(&migration_requested,
		((uint64_t)((epoch << 1) | (mode == 1)) << 32) | target, __ATOMIC_RELEASE);
}

/* Repair one client's transport within the current authority, including epoch 0. */
EMSCRIPTEN_KEEPALIVE void web_quick_play_reconnect(unsigned int target, unsigned int epoch)
{
	if (!target || epoch > 0x7fffffffU) return;
	__atomic_store_n(&reconnect_requested,
		((uint64_t)(epoch + 1U) << 32) | target, __ATOMIC_RELEASE);
}

static unsigned int quick_play_engine_address(unsigned int address)
{
	return ((address & 0xffU) << 24) | ((address & 0xff00U) << 8) |
		((address >> 8) & 0xff00U) | ((address >> 24) & 0xffU);
}

EMSCRIPTEN_KEEPALIVE void web_quick_play_set_address(unsigned int address)
{
	__atomic_store_n(&local_address, quick_play_engine_address(address), __ATOMIC_RELEASE);
}

unsigned long web_quick_play_address(void)
{
	return __atomic_load_n(&local_address, __ATOMIC_ACQUIRE);
}

EMSCRIPTEN_KEEPALIVE void web_quick_play_set_epoch(unsigned int epoch)
{
	if (epoch <= 0x7fffffffU) __atomic_store_n(&initial_epoch, epoch, __ATOMIC_RELEASE);
}

unsigned long web_quick_play_initial_epoch(void)
{
	return __atomic_load_n(&initial_epoch, __ATOMIC_ACQUIRE);
}

/* 1 freezes the existing world; 2 releases a brief connection-loss hold. */
EMSCRIPTEN_KEEPALIVE void web_quick_play_hold(int hold)
{
	__atomic_store_n(&hold_requested, hold ? 1 : 2, __ATOMIC_RELEASE);
}

int web_quick_play_take_hold(void)
{
	return __atomic_exchange_n(&hold_requested, 0, __ATOMIC_ACQ_REL);
}

int web_quick_play_take_migrate(int *host, unsigned long *target, unsigned int *epoch)
{
	uint64_t request = __atomic_exchange_n(&migration_requested, 0, __ATOMIC_ACQ_REL);
	unsigned int address = (unsigned int)request;
	unsigned int control = (unsigned int)(request >> 32);
	if (!request)
		return 0;
	*host = control & 1U;
	*target = quick_play_engine_address(address);
	*epoch = control >> 1;
	return 1;
}

int web_quick_play_take_cancel(void)
{
	return __atomic_exchange_n(&cancel_requested, 0, __ATOMIC_ACQ_REL);
}

int web_quick_play_take_reconnect(unsigned long *target, unsigned int *epoch)
{
	uint64_t request = __atomic_exchange_n(&reconnect_requested, 0, __ATOMIC_ACQ_REL);
	if (!request) return 0;
	*target = quick_play_engine_address((unsigned int)request);
	*epoch = (unsigned int)(request >> 32) - 1U;
	return 1;
}

int web_quick_play_background_active(void)
{
	/* A cancel must also wake a game that was waiting for a hidden page. */
	return web_multiplayer_active() ||
		__atomic_load_n(&background_active, __ATOMIC_ACQUIRE) ||
		__atomic_load_n(&cancel_requested, __ATOMIC_ACQUIRE) ||
		__atomic_load_n(&migration_requested, __ATOMIC_ACQUIRE) ||
		__atomic_load_n(&reconnect_requested, __ATOMIC_ACQUIRE) ||
		__atomic_load_n(&hold_requested, __ATOMIC_ACQUIRE) ||
		emscripten_get_now() < background_drain_until;
}

/* Reports an atomically received checkpoint; selection never elects a tab
   which merely rendered the map but has no recoverable authority state. */
void web_quick_play_checkpoint(unsigned int epoch, long tick, unsigned long seed)
{
	char json[160];
	snprintf(json, sizeof(json), "{\"phase\":\"checkpoint\",\"epoch\":%u,\"tick\":%ld,\"matchId\":%lu}", epoch, tick, seed);
	web_js_post(6, json);
}

void web_quick_play_report(const char *phase, const char *message)
{
	char json[512];
	int active = strcmp(phase, "menu") && strcmp(phase, "error");
	if (!active)
		background_drain_until = emscripten_get_now() + 1000.0;
	__atomic_store_n(&background_active, active, __ATOMIC_RELEASE);
	/* Both strings are fixed engine literals, with no user text or quotes. */
	snprintf(json, sizeof(json), "{\"phase\":\"%s\",\"message\":\"%s\"}", phase, message);
	web_js_post(6, json);
}
