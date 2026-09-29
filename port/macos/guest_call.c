#include "guest_call.h"

#include "guest_address.h"
#include <string.h>

int mac_guest_read(uint32_t guest_address, void *destination, size_t size)
{
	const void *source;

	if (!destination || !(source = mac_guest_address_resolve(guest_address, size)))
		return -1;
	memcpy(destination, source, size);
	return 0;
}

int mac_guest_write(uint32_t guest_address, const void *source, size_t size)
{
	void *destination;

	if (!source || !(destination = mac_guest_address_resolve(guest_address, size)))
		return -1;
	memcpy(destination, source, size);
	return 0;
}

int mac_guest_stack_return(uint32_t guest_esp, uint32_t *return_va_out)
{
	return mac_guest_read(guest_esp, return_va_out, sizeof(*return_va_out));
}

int mac_guest_stack_arg_span(uint32_t guest_esp, uint32_t argument_index,
	size_t size, void **span_out)
{
	uint64_t address = (uint64_t)guest_esp + 4u + (uint64_t)argument_index * 4u;

	if (!span_out || address > UINT32_MAX)
		return -1;
	*span_out = mac_guest_address_resolve((uint32_t)address, size);
	return *span_out ? 0 : -1;
}

int mac_guest_stack_arg32(uint32_t guest_esp, uint32_t argument_index,
	uint32_t *value_out)
{
	void *span;

	if (!value_out || mac_guest_stack_arg_span(guest_esp, argument_index,
		sizeof(*value_out), &span) != 0)
		return -1;
	memcpy(value_out, span, sizeof(*value_out));
	return 0;
}

int mac_guest_stack_arg64(uint32_t guest_esp, uint32_t argument_index,
	uint64_t *value_out)
{
	uint64_t address = (uint64_t)guest_esp + 4 + (uint64_t)argument_index * 8;
	if (!value_out || address > UINT32_MAX)
		return -1;
	return mac_guest_read((uint32_t)address, value_out, sizeof(*value_out));
}

int mac_guest_stack_after_return(uint32_t guest_esp, uint32_t callee_bytes,
	uint32_t *new_esp_out)
{
	uint64_t result = (uint64_t)guest_esp + 4u + callee_bytes;

	if (!new_esp_out || result > UINT32_MAX)
		return -1;
	*new_esp_out = (uint32_t)result;
	return 0;
}
