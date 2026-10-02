CC ?= cc
OUTPUT ?= halo-relay-worker
ROOT := ../..
CFLAGS := -m32 -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -pthread -D_GNU_SOURCE -DHALO_RELAY -I. -I../linux/src -I../third_party/kcp
SOURCES := relay_worker.c relay_shim.c ../linux/src/p2p.c ../linux/src/p2p_signal.c ../linux/src/p2p_crypto.c ../linux/src/posix_net.c ../third_party/kcp/ikcp.c
.PHONY: all test clean
all: $(OUTPUT)
$(OUTPUT): $(SOURCES) relay_platform.h
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(SOURCES) -o $@
test: halo-relay-worker-test policy-test
	./policy-test
	python3 test_relay.py ./halo-relay-worker-test
halo-relay-worker-test: $(SOURCES) relay_platform.h
	$(CC) $(CFLAGS) -DHALO_RELAY_TEST $(SOURCES) -o $@
clean:
	rm -f halo-relay-worker halo-relay-worker-test policy-test

# Check public-address policy using production shims, without test overrides.
policy-test: test_policy.c relay_shim.c
	$(CC) $(CFLAGS) test_policy.c relay_shim.c -o $@
