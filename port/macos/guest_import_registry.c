#include "guest_import_registry.h"

#include "host_services.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct import_entry
{
	char *name;
	mac_guest_import_bridge bridge;
};

static struct import_entry entries[MAC_GUEST_IMPORT_CAPACITY];
static pthread_mutex_t entries_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t entry_count;

static void unsupported_import_bridge(void)
{
	mac_host_abort("unsupported generated host import invoked");
}

static void dispatch_slot(size_t slot)
{
	mac_guest_import_bridge bridge = NULL;
	const char *name = NULL;
	pthread_mutex_lock(&entries_mutex);
	if (slot < entry_count)
	{
		bridge = entries[slot].bridge;
		name = entries[slot].name;
	}
	pthread_mutex_unlock(&entries_mutex);
	if (bridge == unsupported_import_bridge)
	{
		mac_host_logf(3, "unsupported generated host import: %s", name ? name : "<unknown>");
		mac_host_abort("unsupported generated host import invoked");
	}
	if (!bridge)
	{
		mac_host_logf(3, "unregistered AOT import token slot %zu", slot);
		mac_host_abort("unregistered AOT import token invoked");
	}
	bridge();
}

void mac_guest_import_dispatch_token(uint32_t token)
{
	uint32_t delta;
	if (token < MAC_GUEST_IMPORT_TOKEN_BASE)
	{
		mac_host_abort("invalid host-import token below reserved range");
	}
	delta = token - MAC_GUEST_IMPORT_TOKEN_BASE;
	if (delta % MAC_GUEST_IMPORT_TOKEN_STRIDE != 0)
	{
		mac_host_abort("unaligned host-import token");
	}
	dispatch_slot(delta / MAC_GUEST_IMPORT_TOKEN_STRIDE);
}

static void import_wrapper_0(void) { dispatch_slot(0); }
static void import_wrapper_1(void) { dispatch_slot(1); }
static void import_wrapper_2(void) { dispatch_slot(2); }
static void import_wrapper_3(void) { dispatch_slot(3); }
static void import_wrapper_4(void) { dispatch_slot(4); }
static void import_wrapper_5(void) { dispatch_slot(5); }
static void import_wrapper_6(void) { dispatch_slot(6); }
static void import_wrapper_7(void) { dispatch_slot(7); }
static void import_wrapper_8(void) { dispatch_slot(8); }
static void import_wrapper_9(void) { dispatch_slot(9); }
static void import_wrapper_10(void) { dispatch_slot(10); }
static void import_wrapper_11(void) { dispatch_slot(11); }
static void import_wrapper_12(void) { dispatch_slot(12); }
static void import_wrapper_13(void) { dispatch_slot(13); }
static void import_wrapper_14(void) { dispatch_slot(14); }
static void import_wrapper_15(void) { dispatch_slot(15); }
static void import_wrapper_16(void) { dispatch_slot(16); }
static void import_wrapper_17(void) { dispatch_slot(17); }
static void import_wrapper_18(void) { dispatch_slot(18); }
static void import_wrapper_19(void) { dispatch_slot(19); }
static void import_wrapper_20(void) { dispatch_slot(20); }
static void import_wrapper_21(void) { dispatch_slot(21); }
static void import_wrapper_22(void) { dispatch_slot(22); }
static void import_wrapper_23(void) { dispatch_slot(23); }
static void import_wrapper_24(void) { dispatch_slot(24); }
static void import_wrapper_25(void) { dispatch_slot(25); }
static void import_wrapper_26(void) { dispatch_slot(26); }
static void import_wrapper_27(void) { dispatch_slot(27); }
static void import_wrapper_28(void) { dispatch_slot(28); }
static void import_wrapper_29(void) { dispatch_slot(29); }
static void import_wrapper_30(void) { dispatch_slot(30); }
static void import_wrapper_31(void) { dispatch_slot(31); }
static void import_wrapper_32(void) { dispatch_slot(32); }
static void import_wrapper_33(void) { dispatch_slot(33); }
static void import_wrapper_34(void) { dispatch_slot(34); }
static void import_wrapper_35(void) { dispatch_slot(35); }
static void import_wrapper_36(void) { dispatch_slot(36); }
static void import_wrapper_37(void) { dispatch_slot(37); }
static void import_wrapper_38(void) { dispatch_slot(38); }
static void import_wrapper_39(void) { dispatch_slot(39); }
static void import_wrapper_40(void) { dispatch_slot(40); }
static void import_wrapper_41(void) { dispatch_slot(41); }
static void import_wrapper_42(void) { dispatch_slot(42); }
static void import_wrapper_43(void) { dispatch_slot(43); }
static void import_wrapper_44(void) { dispatch_slot(44); }
static void import_wrapper_45(void) { dispatch_slot(45); }
static void import_wrapper_46(void) { dispatch_slot(46); }
static void import_wrapper_47(void) { dispatch_slot(47); }
static void import_wrapper_48(void) { dispatch_slot(48); }
static void import_wrapper_49(void) { dispatch_slot(49); }
static void import_wrapper_50(void) { dispatch_slot(50); }
static void import_wrapper_51(void) { dispatch_slot(51); }
static void import_wrapper_52(void) { dispatch_slot(52); }
static void import_wrapper_53(void) { dispatch_slot(53); }
static void import_wrapper_54(void) { dispatch_slot(54); }
static void import_wrapper_55(void) { dispatch_slot(55); }
static void import_wrapper_56(void) { dispatch_slot(56); }
static void import_wrapper_57(void) { dispatch_slot(57); }
static void import_wrapper_58(void) { dispatch_slot(58); }
static void import_wrapper_59(void) { dispatch_slot(59); }
static void import_wrapper_60(void) { dispatch_slot(60); }
static void import_wrapper_61(void) { dispatch_slot(61); }
static void import_wrapper_62(void) { dispatch_slot(62); }
static void import_wrapper_63(void) { dispatch_slot(63); }
static void import_wrapper_64(void) { dispatch_slot(64); }
static void import_wrapper_65(void) { dispatch_slot(65); }
static void import_wrapper_66(void) { dispatch_slot(66); }
static void import_wrapper_67(void) { dispatch_slot(67); }
static void import_wrapper_68(void) { dispatch_slot(68); }
static void import_wrapper_69(void) { dispatch_slot(69); }
static void import_wrapper_70(void) { dispatch_slot(70); }
static void import_wrapper_71(void) { dispatch_slot(71); }
static void import_wrapper_72(void) { dispatch_slot(72); }
static void import_wrapper_73(void) { dispatch_slot(73); }
static void import_wrapper_74(void) { dispatch_slot(74); }
static void import_wrapper_75(void) { dispatch_slot(75); }
static void import_wrapper_76(void) { dispatch_slot(76); }
static void import_wrapper_77(void) { dispatch_slot(77); }
static void import_wrapper_78(void) { dispatch_slot(78); }
static void import_wrapper_79(void) { dispatch_slot(79); }
static void import_wrapper_80(void) { dispatch_slot(80); }
static void import_wrapper_81(void) { dispatch_slot(81); }
static void import_wrapper_82(void) { dispatch_slot(82); }
static void import_wrapper_83(void) { dispatch_slot(83); }
static void import_wrapper_84(void) { dispatch_slot(84); }
static void import_wrapper_85(void) { dispatch_slot(85); }
static void import_wrapper_86(void) { dispatch_slot(86); }
static void import_wrapper_87(void) { dispatch_slot(87); }
static void import_wrapper_88(void) { dispatch_slot(88); }
static void import_wrapper_89(void) { dispatch_slot(89); }
static void import_wrapper_90(void) { dispatch_slot(90); }
static void import_wrapper_91(void) { dispatch_slot(91); }
static void import_wrapper_92(void) { dispatch_slot(92); }
static void import_wrapper_93(void) { dispatch_slot(93); }
static void import_wrapper_94(void) { dispatch_slot(94); }
static void import_wrapper_95(void) { dispatch_slot(95); }
static void import_wrapper_96(void) { dispatch_slot(96); }
static void import_wrapper_97(void) { dispatch_slot(97); }
static void import_wrapper_98(void) { dispatch_slot(98); }
static void import_wrapper_99(void) { dispatch_slot(99); }
static void import_wrapper_100(void) { dispatch_slot(100); }
static void import_wrapper_101(void) { dispatch_slot(101); }
static void import_wrapper_102(void) { dispatch_slot(102); }
static void import_wrapper_103(void) { dispatch_slot(103); }
static void import_wrapper_104(void) { dispatch_slot(104); }
static void import_wrapper_105(void) { dispatch_slot(105); }
static void import_wrapper_106(void) { dispatch_slot(106); }
static void import_wrapper_107(void) { dispatch_slot(107); }
static void import_wrapper_108(void) { dispatch_slot(108); }
static void import_wrapper_109(void) { dispatch_slot(109); }
static void import_wrapper_110(void) { dispatch_slot(110); }
static void import_wrapper_111(void) { dispatch_slot(111); }
static void import_wrapper_112(void) { dispatch_slot(112); }
static void import_wrapper_113(void) { dispatch_slot(113); }
static void import_wrapper_114(void) { dispatch_slot(114); }
static void import_wrapper_115(void) { dispatch_slot(115); }
static void import_wrapper_116(void) { dispatch_slot(116); }
static void import_wrapper_117(void) { dispatch_slot(117); }
static void import_wrapper_118(void) { dispatch_slot(118); }
static void import_wrapper_119(void) { dispatch_slot(119); }
static void import_wrapper_120(void) { dispatch_slot(120); }
static void import_wrapper_121(void) { dispatch_slot(121); }
static void import_wrapper_122(void) { dispatch_slot(122); }
static void import_wrapper_123(void) { dispatch_slot(123); }
static void import_wrapper_124(void) { dispatch_slot(124); }
static void import_wrapper_125(void) { dispatch_slot(125); }
static void import_wrapper_126(void) { dispatch_slot(126); }
static void import_wrapper_127(void) { dispatch_slot(127); }

static mac_guest_function const wrappers[MAC_GUEST_IMPORT_CAPACITY] =
{
	import_wrapper_0,
	import_wrapper_1,
	import_wrapper_2,
	import_wrapper_3,
	import_wrapper_4,
	import_wrapper_5,
	import_wrapper_6,
	import_wrapper_7,
	import_wrapper_8,
	import_wrapper_9,
	import_wrapper_10,
	import_wrapper_11,
	import_wrapper_12,
	import_wrapper_13,
	import_wrapper_14,
	import_wrapper_15,
	import_wrapper_16,
	import_wrapper_17,
	import_wrapper_18,
	import_wrapper_19,
	import_wrapper_20,
	import_wrapper_21,
	import_wrapper_22,
	import_wrapper_23,
	import_wrapper_24,
	import_wrapper_25,
	import_wrapper_26,
	import_wrapper_27,
	import_wrapper_28,
	import_wrapper_29,
	import_wrapper_30,
	import_wrapper_31,
	import_wrapper_32,
	import_wrapper_33,
	import_wrapper_34,
	import_wrapper_35,
	import_wrapper_36,
	import_wrapper_37,
	import_wrapper_38,
	import_wrapper_39,
	import_wrapper_40,
	import_wrapper_41,
	import_wrapper_42,
	import_wrapper_43,
	import_wrapper_44,
	import_wrapper_45,
	import_wrapper_46,
	import_wrapper_47,
	import_wrapper_48,
	import_wrapper_49,
	import_wrapper_50,
	import_wrapper_51,
	import_wrapper_52,
	import_wrapper_53,
	import_wrapper_54,
	import_wrapper_55,
	import_wrapper_56,
	import_wrapper_57,
	import_wrapper_58,
	import_wrapper_59,
	import_wrapper_60,
	import_wrapper_61,
	import_wrapper_62,
	import_wrapper_63,
	import_wrapper_64,
	import_wrapper_65,
	import_wrapper_66,
	import_wrapper_67,
	import_wrapper_68,
	import_wrapper_69,
	import_wrapper_70,
	import_wrapper_71,
	import_wrapper_72,
	import_wrapper_73,
	import_wrapper_74,
	import_wrapper_75,
	import_wrapper_76,
	import_wrapper_77,
	import_wrapper_78,
	import_wrapper_79,
	import_wrapper_80,
	import_wrapper_81,
	import_wrapper_82,
	import_wrapper_83,
	import_wrapper_84,
	import_wrapper_85,
	import_wrapper_86,
	import_wrapper_87,
	import_wrapper_88,
	import_wrapper_89,
	import_wrapper_90,
	import_wrapper_91,
	import_wrapper_92,
	import_wrapper_93,
	import_wrapper_94,
	import_wrapper_95,
	import_wrapper_96,
	import_wrapper_97,
	import_wrapper_98,
	import_wrapper_99,
	import_wrapper_100,
	import_wrapper_101,
	import_wrapper_102,
	import_wrapper_103,
	import_wrapper_104,
	import_wrapper_105,
	import_wrapper_106,
	import_wrapper_107,
	import_wrapper_108,
	import_wrapper_109,
	import_wrapper_110,
	import_wrapper_111,
	import_wrapper_112,
	import_wrapper_113,
	import_wrapper_114,
	import_wrapper_115,
	import_wrapper_116,
	import_wrapper_117,
	import_wrapper_118,
	import_wrapper_119,
	import_wrapper_120,
	import_wrapper_121,
	import_wrapper_122,
	import_wrapper_123,
	import_wrapper_124,
	import_wrapper_125,
	import_wrapper_126,
	import_wrapper_127
};

int mac_guest_import_register(const char *name, mac_guest_import_bridge bridge,
	uint32_t *token_out)
{
	size_t index;
	char *copy;
	if (!name || !*name || !bridge)
		return -1;
	pthread_mutex_lock(&entries_mutex);
	for (index = 0; index < entry_count; ++index)
	{
		if (strcmp(entries[index].name, name) == 0)
		{
			if (entries[index].bridge != bridge)
			{
				pthread_mutex_unlock(&entries_mutex);
				return -1;
			}
			if (token_out)
				*token_out = MAC_GUEST_IMPORT_TOKEN_BASE + (uint32_t)(index * MAC_GUEST_IMPORT_TOKEN_STRIDE);
			pthread_mutex_unlock(&entries_mutex);
			return 0;
		}
	}
	if (entry_count == MAC_GUEST_IMPORT_CAPACITY)
	{
		pthread_mutex_unlock(&entries_mutex);
		return -1;
	}
	copy = malloc(strlen(name) + 1);
	if (!copy)
	{
		pthread_mutex_unlock(&entries_mutex);
		return -1;
	}
	strcpy(copy, name);
	entries[entry_count].name = copy;
	entries[entry_count].bridge = bridge;
	if (token_out)
		*token_out = MAC_GUEST_IMPORT_TOKEN_BASE + (uint32_t)(entry_count * MAC_GUEST_IMPORT_TOKEN_STRIDE);
	++entry_count;
	pthread_mutex_unlock(&entries_mutex);
	return 0;
}

int mac_guest_import_register_manifest(const char *const *names, size_t count,
	const mac_guest_import_provider *providers, size_t provider_count,
	mac_guest_import_bridge unsupported_fallback)
{
	mac_guest_import_bridge resolved[MAC_GUEST_IMPORT_CAPACITY];
	size_t index;

	if ((!names && count) || (!providers && provider_count) ||
		count > MAC_GUEST_IMPORT_CAPACITY)
		return -1;
	pthread_mutex_lock(&entries_mutex);
	if (entry_count != 0)
	{
		pthread_mutex_unlock(&entries_mutex);
		return -1;
	}
	pthread_mutex_unlock(&entries_mutex);

	for (index = 0; index < count; ++index)
	{
		if (!names[index] || !*names[index] ||
			(index && strcmp(names[index - 1], names[index]) >= 0))
			return -1;
		resolved[index] = NULL;
		for (size_t provider = 0; provider < provider_count; ++provider)
		{
			if (providers[provider] && (resolved[index] = providers[provider](names[index])))
				break;
		}
		if (!resolved[index])
			resolved[index] = unsupported_fallback ? unsupported_fallback : unsupported_import_bridge;
	}
	for (index = 0; index < count; ++index)
		if (mac_guest_import_register(names[index], resolved[index], NULL) != 0)
			return -1;
	return 0;
}

uint32_t mac_guest_import_token(const char *name)
{
	size_t index;
	uint32_t token = 0;
	if (!name)
		return 0;
	pthread_mutex_lock(&entries_mutex);
	for (index = 0; index < entry_count; ++index)
	{
		if (strcmp(entries[index].name, name) == 0)
		{
			token = MAC_GUEST_IMPORT_TOKEN_BASE + (uint32_t)(index * MAC_GUEST_IMPORT_TOKEN_STRIDE);
			break;
		}
	}
	pthread_mutex_unlock(&entries_mutex);
	return token;
}

mac_guest_function mac_guest_import_resolve(uint32_t token)
{
	uint32_t offset;
	size_t slot;
	mac_guest_function function = NULL;
	if (token < MAC_GUEST_IMPORT_TOKEN_BASE)
		return NULL;
	offset = token - MAC_GUEST_IMPORT_TOKEN_BASE;
	if ((offset % MAC_GUEST_IMPORT_TOKEN_STRIDE) != 0)
		return NULL;
	slot = offset / MAC_GUEST_IMPORT_TOKEN_STRIDE;
	pthread_mutex_lock(&entries_mutex);
	if (slot < entry_count)
		function = wrappers[slot];
	pthread_mutex_unlock(&entries_mutex);
	return function;
}

size_t mac_guest_import_count(void)
{
	size_t count;
	pthread_mutex_lock(&entries_mutex);
	count = entry_count;
	pthread_mutex_unlock(&entries_mutex);
	return count;
}

void mac_guest_import_reset(void)
{
	size_t index;
	pthread_mutex_lock(&entries_mutex);
	for (index = 0; index < entry_count; ++index)
	{
		free(entries[index].name);
		entries[index] = (struct import_entry){ 0 };
	}
	entry_count = 0;
	pthread_mutex_unlock(&entries_mutex);
}
