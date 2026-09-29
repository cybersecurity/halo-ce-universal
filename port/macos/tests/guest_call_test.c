#include "../guest_address.h"
#include "../guest_call.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
	uint32_t stack[8] = { 0x00123456u, 0x11223344u, 0xAABBCCDDu, 0 };
	uint32_t value, new_esp;
	uint64_t value64;
	void *span;

	mac_guest_address_reset();
	assert(mac_guest_address_register(0x1000u, stack, sizeof(stack)) == 0);
	assert(mac_guest_stack_return(0x1000u, &value) == 0 && value == 0x00123456u);
	assert(mac_guest_stack_arg32(0x1000u, 0, &value) == 0 && value == 0x11223344u);
	assert(mac_guest_stack_arg32(0x1000u, 1, &value) == 0 && value == 0xAABBCCDDu);
	assert(mac_guest_stack_arg64(0x1000u, 0, &value64) == 0 &&
		value64 == UINT64_C(0xAABBCCDD11223344));
	assert(mac_guest_stack_arg_span(0x1000u, 1, sizeof(value), &span) == 0);
	assert(span == &stack[2]);
	assert(mac_guest_stack_arg32(0xFFFFFFFCu, 0, &value) == -1);
	assert(mac_guest_stack_after_return(0x1000u, 0, &new_esp) == 0 && new_esp == 0x1004u);
	assert(mac_guest_stack_after_return(0x1000u, 8, &new_esp) == 0 && new_esp == 0x100Cu);
	assert(mac_guest_stack_after_return(0xFFFFFFFCu, 0, &new_esp) == -1);
	assert(mac_guest_write(0x100Cu, &value, sizeof(value)) == 0);
	assert(stack[3] == 0xAABBCCDDu);
	assert(mac_guest_read(0x100Cu, &value, sizeof(value)) == 0 && value == stack[3]);
	assert(mac_guest_read(0x101Du, &value, sizeof(value)) == -1);
	mac_guest_address_reset();
	return 0;
}
