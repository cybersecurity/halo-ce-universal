#include <assert.h>
#include <stdio.h>
#include "../../port/linux/game/network_checkpoint.h"

static int typed_payload_valid(void const *buffer, unsigned long size)
{
	return size == 6 && ((unsigned char const *)buffer)[0] == 9;
}

int main(void)
{
	unsigned char staging[32], complete[32];
	unsigned char first[] = { 1, 2, 3, 4, 5, 6 };
	unsigned char second[] = { 9, 8, 7, 6, 5, 4 };
	struct network_checkpoint_store store;
	struct network_checkpoint_manifest a = { 2, 1234, sizeof(first), 0 };
	struct network_checkpoint_manifest b = { 2, 1250, sizeof(second), 0 };
	memset(&store, 0, sizeof(store));
	store.staging = staging;
	store.complete = complete;
	store.capacity = sizeof(staging);
	a.checksum = network_checkpoint_checksum(first, sizeof(first));
	b.checksum = network_checkpoint_checksum(second, sizeof(second));
	/* Reordered, wrong-epoch, truncated and oversized transfers are refused. */
	assert(!network_checkpoint_receive(&store, &a, 2, 3, first + 3, 3));
	assert(!network_checkpoint_receive(&store, &a, 3, 0, first, 3));
	assert(!network_checkpoint_receive(&store, &a, 2, 0, first, 3));
	assert(!store.valid);
	assert(network_checkpoint_receive(&store, &a, 2, 3, first + 3, 3));
	assert(store.valid && store.committed.tick == 1234);
	assert(!memcmp(complete, first, sizeof(first)));
	/* A host dies halfway through the next snapshot: the complete one survives. */
	assert(!network_checkpoint_receive(&store, &b, 2, 0, second, 3));
	assert(store.committed.tick == 1234 && !memcmp(complete, first, sizeof(first)));
	/* Bad contents never commit; later full retry works. */
	assert(!network_checkpoint_receive(&store, &b, 2, 3, first + 3, 3));
	assert(store.committed.tick == 1234 && !memcmp(complete, first, sizeof(first)));
	assert(network_checkpoint_receive(&store, &b, 2, 0, second, sizeof(second)));
	assert(store.committed.tick == 1250 && !memcmp(complete, second, sizeof(second)));
	/* A checksummed but semantically invalid payload cannot evict the valid
one: shape/type validation is part of committing, not an afterthought. */
	store.validate = typed_payload_valid;
	a.tick = 1260;
	assert(!network_checkpoint_receive(&store, &a, 2, 0, first, sizeof(first)));
	assert(store.committed.tick == 1250 && !memcmp(complete, second, sizeof(second)));
	a.tick = 1234;
	assert(!network_checkpoint_receive(&store, &a, 2, 0, first, sizeof(first)));
	b.epoch = 1;
	assert(!network_checkpoint_receive(&store, &b, 2, 0, second, sizeof(second)));
	b.epoch = 3;
	b.tick = 1251;
	b.size = sizeof(staging) + 1;
	assert(!network_checkpoint_receive(&store, &b, 3, 0, second, sizeof(second)));
	puts("atomic checkpoint assembly passed");
	return 0;
}
