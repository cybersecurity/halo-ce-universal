#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
int relay_candidate_allowed(unsigned long address, unsigned short port);
int main(void) {
    const uint32_t blocked[] = {0, 0x0a010203, 0x7f000001, 0x64400001, 0xa9fea9fe,
        0xac100001, 0xc0a80001, 0xc0000201, 0xc6336401, 0xcb007101, 0xe0000001, 0xffffffff};
    for (unsigned i = 0; i < sizeof(blocked)/sizeof(blocked[0]); i++)
        assert(!relay_candidate_allowed(htonl(blocked[i]), htons(1234)));
    assert(relay_candidate_allowed(inet_addr("8.8.8.8"), htons(1234)));
    assert(!relay_candidate_allowed(inet_addr("8.8.8.8"), 0));
    return 0;
}
