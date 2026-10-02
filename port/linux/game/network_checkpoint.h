/* Atomic, bounded assembly of a migration checkpoint. A partial transfer never
replaces the previous complete checkpoint. Transport authenticates its sender. */
#ifndef __NETWORK_CHECKPOINT_H
#define __NETWORK_CHECKPOINT_H

#include <string.h>

struct network_checkpoint_manifest
{
	unsigned long epoch;
	long tick;
	unsigned long size;
	unsigned long checksum;
};

struct network_checkpoint_store
{
	unsigned char *staging;
	unsigned char *complete;
	unsigned long capacity;
	unsigned long received;
	struct network_checkpoint_manifest pending;
	struct network_checkpoint_manifest committed;
	int valid;
	int (*validate)(void const *, unsigned long);
};

static unsigned long network_checkpoint_checksum(void const *buffer, unsigned long size)
{
	unsigned char const *bytes = (unsigned char const *)buffer;
	unsigned long checksum = 2166136261UL;
	unsigned long index;
	for (index = 0; index < size; index++)
		checksum = ((checksum ^ bytes[index]) * 16777619UL) & 0xffffffffUL;
	return checksum;
}

/* Returns 1 only when this fragment completes a verified checkpoint. The
strict offset order is intentional: the enclosing channel is reliable. */
static int network_checkpoint_receive(struct network_checkpoint_store *store,
	struct network_checkpoint_manifest const *manifest, unsigned long epoch,
	unsigned long offset, void const *bytes, unsigned long size)
{
	if (manifest->epoch != epoch || manifest->tick < 0 || !manifest->size ||
		manifest->size > store->capacity || !size || offset > manifest->size ||
		size > manifest->size - offset || (store->valid &&
		(manifest->epoch < store->committed.epoch || (manifest->epoch == store->committed.epoch &&
		manifest->tick <= store->committed.tick))))
		return 0;
	if (!offset)
	{
		store->pending = *manifest;
		store->received = 0;
	}
	if (store->pending.epoch != manifest->epoch || store->pending.tick != manifest->tick ||
		store->pending.size != manifest->size || store->pending.checksum != manifest->checksum ||
		offset != store->received)
		return 0;
	memcpy(store->staging + offset, bytes, size);
	store->received += size;
	if (store->received != manifest->size)
		return 0;
	store->received = 0;
	if (network_checkpoint_checksum(store->staging, manifest->size) != manifest->checksum)
		return 0;
	if (store->validate && !store->validate(store->staging, manifest->size))
		return 0;
	memcpy(store->complete, store->staging, manifest->size);
	store->committed = *manifest;
	store->valid = 1;
	return 1;
}

#endif
