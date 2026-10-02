/*
NETWORK_SERVER_MANAGER.C

symbols in this file:
0011B5D0 0030:
	_countdown_timer_update (0000)
0011B600 0060:
	_countdown_timer_get_time_remaining (0000)
0011B660 0090:
	_countdown_timer_increment (0000)
0011B6F0 0090:
	_countdown_timer_decrement (0000)
0011B780 0040:
	_countdown_timer_set_time_remaining (0000)
0011B7C0 0070:
	_network_game_server_set_game_name (0000)
0011B830 0040:
	_network_game_server_get_game_name (0000)
0011B870 0040:
	_network_game_server_get_state (0000)
0011B8B0 0050:
	_network_game_server_open_game (0000)
0011B950 0060:
	_network_game_server_game_is_open (0000)
0011B9B0 0060:
	_network_game_server_game_is_valid (0000)
0011BA10 00d0:
	_network_game_server_send_player_quit_messages_ingame (0000)
0011BAE0 00e0:
	_network_game_server_start_network_game (0000)
0011BBC0 00a0:
	_network_game_server_switch_to_postgame (0000)
0011BC60 00f0:
	_network_game_server_graceful_shutdown (0000)
0011BD50 0060:
	_network_game_server_client_machine_is_joined_to_game (0000)
0011BDB0 0130:
	_network_game_server_accept_client_machine_into_game (0000)
0011BEE0 0040:
	_player_name_is_unique (0000)
0011BF20 0080:
	_get_unique_random_name (0000)
0011BFA0 0090:
	_get_unique_random_color (0000)
0011C030 0140:
	_network_game_server_add_player_to_game (0000)
0011C170 00e0:
	_network_game_server_remove_player_from_game (0000)
0011C250 00a0:
	_network_game_server_adjust_machine_settings (0000)
0011C2F0 0080:
	_network_game_server_all_machines_have_loaded (0000)
0011C370 00c0:
	_network_game_server_client_machine_game_loading_complete (0000)
0011C430 0030:
	_network_game_server_client_machine_is_precached (0000)
0011C460 0150:
	_network_game_server_handle_client_update_packet (0000)
0011C5B0 0050:
	_network_game_server_switch_machine_from_postgame_to_pregame (0000)
0011C600 01b0:
	_network_game_server_update_ticks (0000)
0011C7B0 0070:
	_network_game_server_queue_player_for_addition (0000)
0011C820 0070:
	_network_game_server_begin_game_start_countdown (0000)
0011C890 0080:
	_server_needs_more_teams (0000)
0011C910 0090:
	_server_has_a_player_on_each_machine (0000)
0011C9A0 0070:
	_server_has_enough_machines (0000)
0011CA10 0050:
	_server_ok_to_countdown (0000)
0011CA60 0040:
	_network_game_server_invalidate_network_machine (0000)
0011CAA0 00a0:
	_network_game_generate_join_game_token (0000)
0011CB40 0090:
	_network_game_server_get_client_machine (0000)
0011CBD0 0030:
	_network_game_server_get_connection (0000)
0011CC00 0020:
	_network_game_server_get_client_connection (0000)
0011CC20 0080:
	_network_game_server_get_machine_connection (0000)
0011CCA0 0050:
	_network_game_server_get_client_machine_at_index (0000)
0011CCF0 00d0:
	_network_game_server_get_client_machine_at_address (0000)
0011CDC0 0040:
	_network_game_server_get_game (0000)
0011CE90 0050:
	_network_game_server_game_can_start (0000)
0011CEE0 0060:
	_network_game_server_pause_countdown (0000)
0011CF40 0100:
	_network_game_server_change_map_name (0000)
0011D040 0090:
	_network_game_server_change_game_variant (0000)
0011D0D0 0170:
	_code_0011d0d0 (0000)
0011D240 00a0:
	_code_0011d240 (0000)
0011D2E0 00a0:
	_network_game_server_send_rejection_message (0000)
0011D380 0030:
	_network_game_server_reject_connection_game_is_full (0000)
0011D3B0 0050:
	_code_0011d3b0 (0000)
0011D400 0070:
	_code_0011d400 (0000)
0011D470 00d0:
	_code_0011d470 (0000)
0011D540 0030:
	_network_game_server_get_client_machine_count (0000)
0011D570 0100:
	_dump_network_game_data (0000)
0011D670 0130:
	_network_game_server_dump (0000)
0011D7A0 0140:
	_network_game_server_remove_client_machine_from_game (0000)
0011D8E0 0140:
	_network_game_server_remove_machine_from_game (0000)
0011DBF0 01e0:
	_network_game_server_update_countdown (0000)
0011DDD0 01d0:
	_code_0011ddd0 (0000)
0011DFA0 02b0:
	_code_0011dfa0 (0000)
0011E250 0120:
	_network_game_server_dispose (0000)
0011E370 0180:
	_network_game_server_idle (0000)
0011E4F0 0250:
	_network_game_server_reset_to_pregame (0000)
0011E740 0150:
	_network_game_server_create (0000)
00285104 001b:
	??_C@_0BL@DEKPBLAE@timer?9?$DOtime_remaining?5?$DO?$DN?50?$AA@ (0000)
00285120 0033:
	??_C@_0DD@CFCGIJJL@c?3?2halo?2SOURCE?2networking?2networ@ (0000)
00285154 0010:
	??_C@_0BA@LLGNDOEI@adjustment?5?$DO?$DN?50?$AA@ (0000)
00285164 0007:
	??_C@_06HJHJCKIO@server?$AA@ (0000)
0028516C 000d:
	??_C@_0N@JPPODHNK@opening?5game?$AA@ (0000)
0028517C 000d:
	??_C@_0N@DMKJHMIA@closing?5game?$AA@ (0000)
0028518C 0032:
	??_C@_0DC@GCAPAPEJ@?$CITRUE?5?$DN?$DN?5game_is_open?$CJ?5?$HM?$HM?5?$CIFALSE@ (0000)
002851C0 0034:
	??_C@_0DE@CFAOOLOB@?$CITRUE?5?$DN?$DN?5game_is_valid?$CJ?5?$HM?$HM?5?$CIFALS@ (0000)
002851F8 0086:
	??_C@_0IG@MNBBLADP@network_game_server_send_message@ (0000)
00285280 0024:
	??_C@_0CE@IKCKJLGP@sending?5quit?5out?5of?5game?0?5time?5?$DN@ (0000)
002852A4 0033:
	??_C@_0DD@OJCFEAMH@_network_game_server_state_ingam@ (0000)
002852D8 0043:
	??_C@_0ED@MDKIICLM@failed?5to?5signal?5client?5machines@ (0000)
0028531C 003d:
	??_C@_0DN@JMGGBBEG@signalling?5client?5machines?5to?5be@ (0000)
0028535C 003a:
	??_C@_0DK@JICNHAFB@failed?5to?5create?5a?5_message_type@ (0000)
00285398 003b:
	??_C@_0DL@ICMNGBFO@failed?5to?5signal?5all?5client?5mach@ (0000)
002853D4 002d:
	??_C@_0CN@DJJDFOFP@server?5sent?5message_game_over?5to@ (0000)
00285408 0045:
	??_C@_0EF@JHLALKAD@server?5going?5down?0?5but?5failed?5to@ (0000)
00285450 0040:
	??_C@_0EA@EJEKFFFL@server?5closing?5down?$DL?5all?5client?5@ (0000)
00285490 003d:
	??_C@_0DN@KLHIBLCI@failed?5to?5create?5a?5message_serve@ (0000)
002854D0 003e:
	??_C@_0DO@IAODKJJI@failed?5to?5create?5a?5message_serve@ (0000)
00285510 0008:
	??_C@_07MHDNFCJE@machine?$AA@ (0000)
00285518 006e:
	??_C@_0GO@BGKBFJIE@network_game_server_accept_clien@ (0000)
00285588 005b:
	??_C@_0FL@EIIOKBIG@network_game_add_machine?$CI?$CJ?5faile@ (0000)
002855E4 003b:
	??_C@_0DL@MGNGFOBG@server?5added?5machine?5?$EA?5?$CFs?5to?5the@ (0000)
00285620 004c:
	??_C@_0EM@PJMBGAIG@client?5machine?5tried?5to?5add?5a?5pl@ (0000)
00285670 004d:
	??_C@_0EN@DNDECHNF@network_game_add_player?$CI?$CJ?5failed@ (0000)
002856C0 0049:
	??_C@_0EJ@LCGCJMBE@server?5added?5player?5from?5machine@ (0000)
00285710 004f:
	??_C@_0EP@COKGKJNJ@client?5machine?5tried?5to?5remove?5a@ (0000)
00285760 0055:
	??_C@_0FF@BKKCLGKI@network_game_remove_player?$CI?$CJ?5fai@ (0000)
002857B8 004d:
	??_C@_0EN@LDDPDGNO@server?5removed?5player?5from?5machi@ (0000)
00285808 004d:
	??_C@_0EN@KMKKCDOM@client?5machine?5tried?5to?5update?5i@ (0000)
00285858 0056:
	??_C@_0FG@IPMBAPCA@network_game_update_machine?$CI?$CJ?5fa@ (0000)
002858B0 0024:
	??_C@_0CE@NCDPADPN@server?5updated?5machine?5?$CD?$CFd?5setti@ (0000)
002858D4 0029:
	??_C@_0CJ@ECFINOOJ@server?5?$CG?$CG?5machine?5?$CG?$CG?5machine_des@ (0000)
00285900 001b:
	??_C@_0BL@PFJPBEPF@local?5game?5data?5not?5loaded?$AA@ (0000)
0028591C 0026:
	??_C@_0CG@JHLMGKLE@all?5machines?5have?5successfully?5l@ (0000)
00285944 002f:
	??_C@_0CP@JBNFJJAE@still?5waiting?5on?5machine?5?$CD?$CFd?5to?5@ (0000)
00285978 0047:
	??_C@_0EH@NKIEAGCA@client?5update?5packet?5from?5machin@ (0000)
002859C0 0040:
	??_C@_0EA@BIPOHPNI@received?5an?5outdated?5client?5upda@ (0000)
00285A00 004b:
	??_C@_0EL@GBABNBCE@client?5machine?5?$CD?$CFd?5is?5out?5of?5syn@ (0000)
00285A4C 000f:
	??_C@_0P@OKCBNOPP@message_packet?$AA@ (0000)
00285A5C 0031:
	??_C@_0DB@OKJCLNBN@machine?5?$CD?$CFd?5has?5successfully?5swi@ (0000)
00285A90 0012:
	??_C@_0BC@HJLNGPOP@server?5?$CG?$CG?5machine?$AA@ (0000)
00285AA8 0085:
	??_C@_0IF@CCOGGKBF@network_game_server_send_player_@ (0000)
00285B30 002e:
	??_C@_0CO@BNMPLHFO@server?5failed?5to?5add?5a?5network?5p@ (0000)
00285B60 005d:
	??_C@_0FN@BDPHLOBF@server?5failed?5to?5send?5game?5updat@ (0000)
00285BC0 0011:
	??_C@_0BB@JLJAPHKN@server?5?$CG?$CG?5player?$AA@ (0000)
00285BD4 0024:
	??_C@_0CE@KMCEHK@server?5game?5start?5countdown?5star@ (0000)
00285BF8 000b:
	??_C@_0L@DEEHFDNB@join_token?$AA@ (0000)
00285C04 003c:
	??_C@_0DM@GFDOGMMN@client_machine?9?$DOmachine_index?$DMMA@ (0000)
00285C40 0019:
	??_C@_0BJ@PDBLHLMI@server?5?$CG?$CG?5client_machine?$AA@ (0000)
00285C5C 002c:
	??_C@_0CM@HFLLHELF@server?5?$CG?$CG?5network_machine_is_val@ (0000)
00285C88 0030:
	??_C@_0DA@BAJANDFN@server?5?$CG?$CG?5?$CIindex?$DMMAXIMUM_NETWORK@ (0000)
00285CB8 001b:
	??_C@_0BL@OEHAOKDP@no?5machine?5found?5?$EA?5ip?5?$CD?$CFlX?$AA@ (0000)
00285CD4 0026:
	??_C@_0CG@LADAADEM@server?9?$DOclient_machines?$FLi?$FN?4conne@ (0000)
00285CFC 0015:
	??_C@_0BF@BGCFGMDN@server?5?$CG?$CG?5ip_address?$AA@ (0000)
00285D18 0056:
	??_C@_0FG@ENHBPECG@network_game_server_change_map_n@ (0000)
00285D70 0034:
	??_C@_0DE@OPKCBBPP@server?9?$DOstate?5?$DN?$DN?5_network_game_s@ (0000)
00285DA4 0022:
	??_C@_0CC@DFOMMGAI@server?5?$CG?$CG?5map_name?5?$CG?$CG?5map_name?$FL0@ (0000)
00285DC8 005a:
	??_C@_0FK@HEDIBIPF@network_game_server_change_game_@ (0000)
00285E24 0012:
	??_C@_0BC@DLOLLBOD@server?5?$CG?$CG?5variant?$AA@ (0000)
00285E38 0047:
	??_C@_0EH@GICIMKGF@network_game_server_add_new_clie@ (0000)
00285E80 0051:
	??_C@_0FB@EIEHAMPG@failed?5to?5find?5an?5available?5mach@ (0000)
00285ED8 0067:
	??_C@_0GH@CKJGDANO@network_connection_get_address?$CI?$CJ@ (0000)
00285F40 0027:
	??_C@_0CH@GMPLHAIM@new?5remote?5connection?5accepted?5f@ (0000)
00285F68 0062:
	??_C@_0GC@GMAADHDI@remote?5system?5tried?5to?5join?5our?5@ (0000)
00285FCC 0019:
	??_C@_0BJ@BMKILIN@server?5?$CG?$CG?5new_connection?$AA@ (0000)
00285FE8 005d:
	??_C@_0FN@HKDNEOAD@network_game_server_handle_datag@ (0000)
00286048 0069:
	??_C@_0GJ@NFGOFIEO@failed?5to?5create?5a?5message_serve@ (0000)
002860B8 0041:
	??_C@_0EB@ILPMAAHI@error?5sending?5rejection?5message?5@ (0000)
002860FC 0038:
	??_C@_0DI@LLMEOPEK@endpoint?5?$CG?$CG?5?$CIreason?5?$DM?5NUMBER_OF_@ (0000)
00286134 0028:
	??_C@_0CI@NMINAOIB@client?5connection?5refused?$DL?5game?5@ (0000)
00286160 0061:
	??_C@_0GB@DCECGEGP@?$CBall_machines_have_precached?5?$HM?$HM?5@ (0000)
002861C4 003e:
	??_C@_0DO@MCCLFBCA@network?5game?5setup?5failed?$DL?5proba@ (0000)
00286204 0016:
	??_C@_0BG@OLFNBGNL@setting?5up?5a?5net?5game?$AA@ (0000)
0028621C 001c:
	??_C@_0BM@JFGLKNKN@?$CFsnumber_of_games_played?5?$CFd?$AA@ (0000)
00286238 001e:
	??_C@_0BO@JOAPPGLE@?$CFsnetwork_game_random_seed?5?$CFx?$AA@ (0000)
00286258 0018:
	??_C@_0BI@BAJBJCMP@?$CFs?7player_list_index?5?$CFx?$AA@ (0000)
00286270 0011:
	??_C@_0BB@EOOHLDJC@?$CFs?7team_index?5?$CFx?$AA@ (0000)
00286284 0017:
	??_C@_0BH@LNCHICCP@?$CFs?7controller_index?5?$CFx?$AA@ (0000)
0028629C 0014:
	??_C@_0BE@HLHKAFCI@?$CFs?7machine_index?5?$CFx?$AA@ (0000)
002862B0 000c:
	??_C@_0M@ONJMLKP@?$CFsplayer?5?$CFd?$AA@ (0000)
002862BC 0012:
	??_C@_0BC@MPDEELBJ@?$CFsplayer_count?5?$CFd?$AA@ (0000)
002862D0 0011:
	??_C@_0BB@NMBMNGAB@?7?$CFsmachine?5?$CFd?5?$CFx?$AA@ (0000)
002862E4 0013:
	??_C@_0BD@FGMHAHMN@?$CFsmachine_count?5?$CFd?$AA@ (0000)
002862F8 0014:
	??_C@_0BE@JAKJKGBL@?$CFsnetwork_game_data?$AA@ (0000)
0028630C 001e:
	??_C@_0BO@KBMOCMCI@?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CKEND?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$AA@ (0000)
0028632C 002c:
	??_C@_0CM@KBELBPJF@?7time_of_first_client_loading_co@ (0000)
00286358 001c:
	??_C@_0BM@EADPCOLB@?7time_of_last_keep_alive?5?$CFd?$AA@ (0000)
00286374 0017:
	??_C@_0BH@BNHPAHB@?7next_update_number?5?$CFd?$AA@ (0000)
0028638C 000b:
	??_C@_0L@EEANCMOB@?7?7flags?5?$CFx?$AA@ (0000)
00286398 0013:
	??_C@_0BD@JKAKGNJM@?7?7machine_index?5?$CFx?$AA@ (0000)
002863AC 0016:
	??_C@_0BG@PPFEBDPA@?7?7stall_start_time?5?$CFd?$AA@ (0000)
002863C4 002a:
	??_C@_0CK@BMPEFDDJ@?7?7last_received_update_sequence_@ (0000)
002863F0 0013:
	??_C@_0BD@NOBCPADN@?7?7connection?5?$CFx?5?$CFs?$AA@ (0000)
00286404 000b:
	??_C@_0L@NIBBMIDE@?7client?5?$CFd?$AA@ (0000)
00286410 0007:
	??_C@_06NEHEIOHK@?$CIdead?$CJ?$AA@ (0000)
00286418 0009:
	??_C@_08MGFPAODM@?$CIactive?$CJ?$AA@ (0000)
00286424 000e:
	??_C@_0O@EFMNOFHN@no?5connection?$AA@ (0000)
00286434 0011:
	??_C@_0BB@HDACINMK@client_machines?3?$AA@ (0000)
00286448 0002:
	??_C@_01GPOEFGEJ@?7?$AA@ (0000)
0028644C 000a:
	??_C@_09LHENMLPO@?7flags?5?$CFx?$AA@ (0000)
00286458 000a:
	??_C@_09JEKGPEEJ@?7state?5?$CFx?$AA@ (0000)
00286464 000f:
	??_C@_0P@IGEGAEKH@?7connection?5?$CFx?$AA@ (0000)
00286474 0020:
	??_C@_0CA@JIMNGMN@?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CKBEGIN?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$CK?$AA@ (0000)
00286494 002d:
	??_C@_0CN@BOOLPEKD@server?5failed?5to?5close?5a?5client?8@ (0000)
002864C8 005b:
	??_C@_0FL@JJMLLPJG@network_game_server_remove_clien@ (0000)
00286528 0080:
	??_C@_0IA@HCKJGBDF@network_game_server_remove_clien@ (0000)
002865A8 0013:
	??_C@_0BD@OGPILLHP@machine?5index?5?$DN?5?$CFx?$AA@ (0000)
002865BC 0021:
	??_C@_0CB@BNBDJEGI@machine?5name?5?$DN?5?$DMnot?5implemented?$DO@ (0000)
002865E0 0067:
	??_C@_0GH@MMONEEJC@attempted?5to?5remove?5an?5invalid?5m@ (0000)
00286648 0069:
	??_C@_0GJ@MMCLLNAF@network_game_server_remove_machi@ (0000)
002866B8 0057:
	??_C@_0FH@BABKECJC@network_game_remove_machine?$CI?$CJ?5fa@ (0000)
00286710 0054:
	??_C@_0FE@BFPCFLEJ@network_game_server_remove_machi@ (0000)
00286768 006f:
	??_C@_0GP@EIJIPAGB@network_game_server_remove_clien@ (0000)
002867D8 0051:
	??_C@_0FB@JOJEBAMD@network_game_server_remove_machi@ (0000)
0028682C 0008:
	??_C@_07DPHHBAJ@removed?$AA@ (0000)
00286834 003c:
	??_C@_0DM@CCJHIKEB@forcibly?5removing?5client?5system?5@ (0000)
00286870 000f:
	??_C@_0P@IOAOPNJI@?$DMunknown?5name?$DO?$AA@ (0000)
00286880 0010:
	??_C@_0BA@JMALINEK@culprit?5?$CB?$DN?5NONE?$AA@ (0000)
00286890 003e:
	??_C@_0DO@BHFBBCGG@server?5?$CG?$CG?5server?9?$DOstate?5?$DN?$DN?5_netw@ (0000)
002868D0 002a:
	??_C@_0CK@CDKFKGHB@failed?5to?5remove?5client?5machine?5@ (0000)
002868FC 0021:
	??_C@_0CB@MJKIPMIF@client?5machine?5removed?5from?5game@ (0000)
00286920 0063:
	??_C@_0GD@ILCKCFLA@network_game_server_handle_clien@ (0000)
00286984 002d:
	??_C@_0CN@GAKEIIC@failed?5to?5remove?5client?5machine?5@ (0000)
002869B4 0024:
	??_C@_0CE@CPAGILJL@client?5machine?5?$CFx?5removed?5from?5g@ (0000)
002869D8 004b:
	??_C@_0EL@DJHOLFOA@forcibly?5removing?5client?5system?5@ (0000)
00286A28 0041:
	??_C@_0EB@FNLOGEMK@failed?5to?5send?5a?5message_server_@ (0000)
00286A6C 0030:
	??_C@_0DA@IPEDOIIA@network_game_server_start_networ@ (0000)
00286A9C 001f:
	??_C@_0BP@CKGEOJEP@booting?5dead?5client?5machine?5?$CFd?$AA@ (0000)
00286ABC 0018:
	??_C@_0BI@BNPOAEPH@network?5server?5disposed?$AA@ (0000)
00286AD4 0036:
	??_C@_0DG@DABJFLIB@network_game_server_memory_do_no@ (0000)
00286B10 0059:
	??_C@_0FJ@KJNIDGHK@network_game_server_handle_clien@ (0000)
00286B70 004b:
	??_C@_0EL@BMJJENN@failed?5to?5create?5a?5_message_type@ (0000)
00286BBC 0034:
	??_C@_0DE@CJDOLHKA@failed?5to?5notify?5all?5clients?5tha@ (0000)
00286BF0 002c:
	??_C@_0CM@DJIIBLPE@notified?5all?5clients?5that?5we?5are@ (0000)
00286C20 004c:
	??_C@_0EM@NAIJHAGO@failed?5to?5create?5a?5_message_type@ (0000)
00286C6C 001d:
	??_C@_0BN@CJOEAMG@the?5server?8s?5game?5is?5invalid?$AA@ (0000)
00286C8C 0021:
	??_C@_0CB@OBPDACNL@network_connection_idle?$CI?$CJ?5failed@ (0000)
00286CB0 0034:
	??_C@_0DE@MJEBDOOH@network_game_server_handle_publi@ (0000)
00286CE4 0034:
	??_C@_0DE@ECHDPAEM@network_game_server_handle_clien@ (0000)
00286D18 0015:
	??_C@_0BF@HDEDCFJD@unknown?5server?5state?$AA@ (0000)
00286D30 0030:
	??_C@_0DA@GGCMIGDD@failed?5to?5add?5new?5client?5connect@ (0000)
00286D60 0035:
	??_C@_0DF@CAHJJMAP@new?5client?5connected?5from?5ip?5?$CFs?5@ (0000)
00286D98 003a:
	??_C@_0DK@OICDNHOL@failed?5to?5signal?5all?5client?5mach@ (0000)
00286DD8 0050:
	??_C@_0FA@CGIOIDCF@the?5playlist?5has?5ended?5?9?5server?5@ (0000)
00286E28 002b:
	??_C@_0CL@GEFANPJI@the?5playlist?5has?5ended?5?9?5server?5@ (0000)
00286E54 001c:
	??_C@_0BM@DEOHMBDO@server?5resetting?5to?5pregame?$AA@ (0000)
00286E70 0027:
	??_C@_0CH@LGNFBGFC@failed?5to?5create?5the?5server?5conn@ (0000)
00286E98 002d:
	??_C@_0CN@LEAKMMND@failed?5to?5initialize?5server?5preg@ (0000)
00286EC8 0037:
	??_C@_0DH@KNNCDAKL@?$CBnetwork_game_server_memory_do_n@ (0000)
00456CF4 0008:
	_network_game_server_memory_do_not_use_directly_in_use (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "bungie_net/common/message_header.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_address_constants.h"
#include "bungie_net/network/transport_endpoint_winsock.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/player_queues_new.h"
#include "game/players.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "math/real_math.h"
#include "networking/network_client_manager.h"
#include "networking/network_connection.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_game_protocol.h"
#include "networking/network_game_ui.h"
#include "networking/network_messages.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_manager_internal.h"
#include "networking/network_server_message_handler.h"
#include "saved games/player_profile.h"
#include "text/unicode.h"

#include "cache/cache_files.h"
#ifdef HALO_WEB
#include "network_migration.h"
#endif

/* port: internet play's Discord presence (port/linux/src/p2p.c) */
void p2p_set_game_player_counts(int count, int maximum);

/* ---------- constants */

#define NETWORK_SERVER_MANAGER_FILE "c:\\halo\\SOURCE\\networking\\network_server_manager.c"

enum
{
	/* the native builds' session limits (port/linux/include/halo_port_limits.h) */
	MAXIMUM_NETWORK_MACHINE_COUNT = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	MAXIMUM_NETWORK_PLAYER_COUNT = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	NETWORK_GAME_NAME_LENGTH = 16,
	NETWORK_GAME_MAP_NAME_LENGTH = 0x80,
	NETWORK_PLAYER_NAME_LENGTH = 12,
	MAXIMUM_MACHINE_NAME_LENGTH = 32,
	NUMBER_OF_MULTIPLAYER_TEAMS = 2,
	NETWORK_GAME_PLAYER_QUIT_DELAY = 33,
	/* the time the other machines have to load the map once the first has
	finished, allowing for many machines of mixed speed */
	NETWORK_GAME_SERVER_MAXIMUM_WAIT_TIME_FOR_LEVEL_LOADING =
		60 * MILLISECONDS_PER_SECOND,
	MAXIMUM_PLAYERS_PER_MACHINE = MAXIMUM_LOCAL_PLAYERS,
	PLAYER_UPDATE_SIZE = 0x20,
	MAXIMUM_GOOD_COLOR_ATTEMPTS = 10,
	/* with more players than random names or colours, the pickers settle for a
	numbered name or a shared colour after this many tries */
	MAXIMUM_UNIQUE_NAME_ATTEMPTS = 64,
	MAXIMUM_UNIQUE_COLOR_ATTEMPTS = 64,
	/* (the top bit was the lockstep netcode's out of sync, which clients no
	longer set) */
	CLIENT_UPDATE_SEQUENCE_NUMBER_MASK = 0x7FFFFFFF,
	NETWORK_GAME_COUNTDOWN_TIME = 30999,
	NETWORK_GAME_SPLITSCREEN_COUNTDOWN_TIME = 10999,
	NETWORK_GAME_COUNTDOWN_ADJUSTMENT = 5000,
	NETWORK_GAME_MINIMUM_COUNTDOWN_TIME = 999,
	_network_client_machine_connected_bit = 0,
	_network_client_machine_validated_bit,
	_network_client_machine_level_loaded_bit,
	_network_client_machine_precached_bit,
	/* started in the game in progress (network_game_server_start_late_joiner) */
	_network_client_machine_started_late_bit,
	NUMBER_OF_NETWORK_CLIENT_MACHINE_FLAGS,
	/* the protocol's largest message (a client's hits come near it) */
	MAXIMUM_NETWORK_MESSAGE_SIZE = HALO_PORT_MAXIMUM_NETWORK_MESSAGE_SIZE,
	/* a machine that connects must join within this, and one in the game
	not go silent for longer (it sends ten times a second) */
	NETWORK_GAME_SERVER_JOIN_TIMEOUT = 10 * MILLISECONDS_PER_SECOND,
	NETWORK_GAME_SERVER_CLIENT_TIMEOUT = 15 * MILLISECONDS_PER_SECOND,
	/* a machine joining the game in progress, silent while it loads */
	NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT = 120 * MILLISECONDS_PER_SECOND,
};

enum
{
	_network_game_server_game_open_bit = 0,
	_network_game_server_game_valid_bit,
};

enum
{
	_network_game_server_countdown_event_player_left,
	_network_game_server_countdown_event_player_joined,
	_network_game_server_countdown_event_stop,
	_network_game_server_countdown_event_start_immediately,

	NUMBER_OF_NETWORK_GAME_SERVER_COUNTDOWN_EVENTS
};

enum
{
	_network_game_server_state_pregame,
	_network_game_server_state_ingame,
	_network_game_server_state_postgame,

	NUMBER_OF_NETWORK_GAME_SERVER_STATES
};

/* ---------- macros */

#define network_machine_is_valid(machine) \
	((machine) && (machine)->machine_index >= 0 && \
	(machine)->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)

/* ---------- structures */

struct countdown_timer
{
	long time_remaining;
	unsigned long last_update_time;
};

struct message_server_game_over
{
	long unused;
};

struct message_server_begin_game
{
	long unused;
};

struct message_server_remove_player_ingame
{
	struct network_player player;
	long reason;
};

struct message_server_graceful_game_exit_pregame
{
	long unused;
};

struct message_server_switch_to_pregame
{
	long unused;
};

struct message_server_graceful_game_exit_postgame
{
	long unused;
};

struct message_server_machine_rejected
{
	word reason;
};

struct message_server_pregame_countdown
{
	short seconds_to_start;
};

struct message_server_pregame_keep_alive
{
	short unused;
};

struct message_server_postgame_keep_alive
{
	short unused;
};

struct message_client_game_update
{
	unsigned long update_number;
	short unknown04;
	short player_count;
	struct player_action actions[MAXIMUM_PLAYERS_PER_MACHINE];
};

struct message_server_game_update
{
	long update_number;
	long random_seed;
	long game_time;
	word unknown0C;
	word player_count;
	byte player_updates[MAXIMUM_NETWORK_PLAYER_COUNT * PLAYER_UPDATE_SIZE];
};

typedef char network_machine_size_assert[
	sizeof(struct network_machine) == 0x44 ? 1 : -1];

struct network_game_server_client_machine
{
	struct network_connection *connection;
	unsigned long last_received_update_sequence_number;
	/* when it was last heard from (the lockstep netcode's stall_start_time) */
	unsigned long last_heard_time;
	short machine_index;
	word flags;
};

struct network_game_server_countdown_state
{
	struct countdown_timer timer;
	long last_countdown_message_time;
	boolean active;
	boolean paused;
	boolean adjusted_time_this_tick;
	byte padding0F;
};

struct network_game_server
{
	struct network_connection *connection;
	word state;
	word flags;
	struct network_game game;
	struct network_game_server_client_machine client_machines[MAXIMUM_NETWORK_MACHINE_COUNT];
	long next_update_number;
	long time_of_last_keep_alive;
	unsigned long time_of_first_client_loading_completion;
	struct network_game_server_countdown_state countdown_state;
	struct network_player queued_player;
	boolean queued_player_valid;
	boolean sent_start_game_message;
	byte padding4BA[2];
	/* in-game joins waiting behind queued_player (the Xbox game keeps one
	and drops any other that arrives meanwhile) */
	struct network_player waiting_players[MAXIMUM_NETWORK_PLAYER_COUNT];
	long waiting_player_count;
};

/* the layout follows the session limits (port/linux/include/halo_port_limits.h) */
typedef char network_game_players_offset_assert[
	offsetof(struct network_game, players) == HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET ? 1 : -1];
typedef char network_game_variant_has_teams_offset_assert[
	offsetof(struct network_game, variant) +
		offsetof(struct game_variant, universal_variant.teams) == 0xC0 ? 1 : -1];
typedef char network_game_size_assert[
	sizeof(struct network_game) == HALO_PORT_NETWORK_GAME_SIZE ? 1 : -1];
typedef char network_game_server_client_machines_offset_assert[
	offsetof(struct network_game_server, client_machines) == 8 + HALO_PORT_NETWORK_GAME_SIZE ? 1 : -1];
typedef char network_game_server_countdown_state_offset_assert[
	offsetof(struct network_game_server, countdown_state) ==
		8 + HALO_PORT_NETWORK_GAME_SIZE + MAXIMUM_NETWORK_MACHINE_COUNT * 0x10 + 0xC ? 1 : -1];

/* ---------- prototypes */

void countdown_timer_increment(
	struct countdown_timer *timer,
	long adjustment,
	long maximum);

static boolean network_game_server_setup_game_from_playlist(
	struct network_game_server *server);
static boolean network_game_server_machine_has_waiting_players(
	struct network_game_server *server,
	long machine_index);
static boolean network_game_server_machine_has_players(
	struct network_game_server *server,
	long machine_index);
static void network_game_server_refuse_late_joiner(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	word reason);
static boolean network_game_server_drop_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *client);
static void network_game_server_start_late_joiner(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
static void network_game_server_keep_late_joiners_alive(
	struct network_game_server *server);
static boolean network_game_server_add_new_client(
	struct network_game_server *server,
	struct network_connection *new_connection);
static boolean network_game_server_handle_public_endpoint(
	struct network_game_server *server);
static boolean network_game_server_handle_client_machines(
	struct network_game_server *server);
static boolean network_game_server_idle_postgame_tasks(
	struct network_game_server *server);
static boolean network_game_server_have_all_machines_have_precached(
	struct network_game_server *server);
static boolean network_game_server_idle_pregame_tasks(
	struct network_game_server *server);
static void network_game_server_send_rejection_message(
	struct transport_endpoint *endpoint,
	word reason);
static void network_game_server_reject_connection_game_is_full(
	struct transport_endpoint *endpoint);
static short network_game_server_get_client_machine_count(
	struct network_game_server *server);
void get_unique_random_name(
	struct network_game_server *server,
	struct network_player *player);
void get_unique_random_color(
	struct network_game_server *server,
	struct network_player *player);
static boolean player_name_is_unique(
	struct network_game_server *server,
	wchar_t const *name);
static void network_game_server_dump(
	struct network_game_server *server);
static void network_game_server_countdown_started(
	struct network_game_server *server);
static void network_game_server_remove_players_gone_while_loading(
	struct network_game_server *server);

/* ---------- globals */

struct network_game_server network_game_server_memory_do_not_use_directly;
boolean network_game_server_memory_do_not_use_directly_in_use = FALSE;

#ifdef HALO_WEB
static struct
{
	boolean adopting;
	unsigned long epoch;
	short host_machine, departed_machine;
	unsigned long owner_addresses[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	unsigned long disconnected_at[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
} server_migration;

void network_game_server_send_player_quit_messages_ingame(struct network_game_server *server,
	struct network_game_server_client_machine *machine);
static void network_game_server_migration_detach(struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	short original = machine->machine_index;
	if (original >= 0 && original < MAXIMUM_NETWORK_MACHINE_COUNT)
	{
		struct transport_address owner = { 0 };
		if (machine->connection)
			network_connection_get_address(machine->connection, &owner, NULL);
		if (owner.address.long_words[0])
			server_migration.owner_addresses[original] = owner.address.long_words[0] == IPV4_LOOPBACK_ADDRESS ?
				web_quick_play_address() : owner.address.long_words[0];
		server_migration.disconnected_at[original] = MAX(system_milliseconds(), 1);
	}
	if (machine->connection)
		network_server_close_client_connection(server->connection, machine->connection);
	csmemset(machine, 0, sizeof(*machine));
	machine->machine_index = NONE;
}

static void network_game_server_migration_expire_disconnected(struct network_game_server *server)
{
	short index;
	unsigned long now = system_milliseconds();
	if ((!server_migration.epoch && !web_match_migration_enabled()) || server_migration.adopting)
		return;
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		if (server_migration.disconnected_at[index] && now - server_migration.disconnected_at[index] >= 120000UL &&
			network_machine_is_valid(&server->game.machines[index]))
		{
			struct network_game_server_client_machine departing = { 0 };
			departing.machine_index = index;
			network_game_server_send_player_quit_messages_ingame(server, &departing);
			network_game_remove_machine(&server->game, &server->game.machines[index]);
			server_migration.disconnected_at[index] = 0;
		}
	}
}
#endif
/* port: the players in the settings the game started with: every machine
spawned them, and those whose machines left while the game loaded (whom no
quit message reached, the server being in the pregame) are removed from the
game once it has loaded (network_game_server_all_machines_have_loaded) */
static struct network_player network_game_server_start_players[MAXIMUM_NETWORK_PLAYER_COUNT];

/* port: each joined client machine's IPv4 address (0: none), by slot, which
a datagram is matched to without asking every connection for its address
(network_game_server_get_client_machine_at_address) */
static unsigned long network_game_server_client_machine_addresses[MAXIMUM_NETWORK_MACHINE_COUNT];

/* port: the update number up to which a machine that has loaded (at the
start, or joining the game in progress) gets the host's game update every
tick; after that once a second (network_game_server_update_ticks) */
static long network_game_server_frequent_updates_until[MAXIMUM_NETWORK_MACHINE_COUNT];

/* port: the client machines that have slowed this countdown (once each:
network_game_server_client_machine_may_slow_countdown) */
static boolean network_game_server_countdown_slowed[MAXIMUM_NETWORK_MACHINE_COUNT];

/* port/linux/game/network_distributed.c's (the host's bans: bans.txt) */
boolean network_distributed_banned(unsigned long address, char const *hardware_id);
void network_distributed_ban(long machine_index, unsigned long address, char const *names);
/* port/linux/src/p2p.c's */
enum
{
	P2P_HARDWARE_ID_SIZE = 33,
};
void p2p_hardware_id_sanitize(char *destination, int size, const char *source);
/* console.c's */
void console_warning(const char *format, ...);

/* port: the client machines the distributed netcode asked to drop (their
games sped up: network_game_server_kick_machine), dropped as this server
next looks at its machines; and the addresses of those dropped, kept out
of this server's games while it lasts */
enum
{
	MAXIMUM_KICKED_ADDRESSES = 64,
};
static boolean network_game_server_kick_pending[MAXIMUM_NETWORK_MACHINE_COUNT];
/* port: each client machine's hardware id as it told it joining, hex only
(p2p_hardware_id_sanitize), by slot */
static char network_game_server_hardware_ids[MAXIMUM_NETWORK_MACHINE_COUNT][P2P_HARDWARE_ID_SIZE];
static unsigned long network_game_server_kicked_addresses[MAXIMUM_KICKED_ADDRESSES];
static long network_game_server_kicked_address_next;

/* port: when each client machine joined (system_milliseconds): one that has
added no player this long after holds the lobby's countdown (a machine's
player is asked for as it joins) */
static unsigned long network_game_server_client_machine_join_times[MAXIMUM_NETWORK_MACHINE_COUNT];
enum
{
	NETWORK_GAME_SERVER_PLAYERLESS_MACHINE_TIMEOUT = 15 * MILLISECONDS_PER_SECOND,
	/* the connections of one address that have not joined yet a server
	takes (network_game_server_add_new_client) */
	MAXIMUM_WAITING_CONNECTIONS_PER_ADDRESS = 2,
};

/* port: the players added to the game in progress from each client
machine's address (a machine joins from one address, and joining again
drops the old: network_game_server_accept_client_machine_into_game), no
more than MAXIMUM_INGAME_ADDITIONS_PER_ADDRESS a game, so that players
added and removed, or a machine joining again and again, do not take
every player of the game (a player gone stays for its scores) */
enum
{
	MAXIMUM_INGAME_ADDITIONS_PER_ADDRESS = 2 * MAXIMUM_PLAYERS_PER_MACHINE,
};
static struct
{
	unsigned long address;
	short count;
} network_game_server_ingame_additions[MAXIMUM_NETWORK_PLAYER_COUNT];

static short *network_game_server_ingame_addition_count(
	unsigned long address,
	boolean create)
{
	long index;

	if (!address)
		return NULL;
	for (index = 0; index < (long)NUMBEROF(network_game_server_ingame_additions); index++)
	{
		if (network_game_server_ingame_additions[index].address == address)
			return &network_game_server_ingame_additions[index].count;
	}
	/* (more addresses than players: the game is full anyway) */
	for (index = 0; create && index < (long)NUMBEROF(network_game_server_ingame_additions); index++)
	{
		if (!network_game_server_ingame_additions[index].address)
		{
			network_game_server_ingame_additions[index].address = address;
			network_game_server_ingame_additions[index].count = 0;
			return &network_game_server_ingame_additions[index].count;
		}
	}
	return NULL;
}

/* port: a player's name in ASCII (player_name_character_ascii: a letter
with a mark its plain one, what is not ASCII else "?"), for the host's ban
command */
static void network_game_server_player_name_text(
	struct network_player const *player,
	char *text,
	long size)
{
	long index;

	for (index = 0; index < (long)NUMBEROF(player->name) && player->name[index] && index < size - 1; index++)
		text[index] = player_name_character_ascii(player->name[index]);
	text[index] = 0;
}

/* port: whether a player's name (in ASCII) begins with the text, in either
case */
static boolean network_game_server_name_begins_with(
	char const *name,
	char const *text)
{
	for (; *text; name++, text++)
	{
		char a = *name >= 'A' && *name <= 'Z' ? *name - 'A' + 'a' : *name;
		char b = *text >= 'A' && *text <= 'Z' ? *text - 'A' + 'a' : *text;

		if (!*name || a != b)
			return FALSE;
	}
	return TRUE;
}

/* port: the names of the players of the game's other machines beginning with
the text (the host's ban command's completion: console.c); how many */
short network_game_server_matching_player_names(
	char const *text,
	char (*names)[NETWORK_GAME_SERVER_NAME_TEXT_SIZE],
	short maximum_count)
{
	struct network_game_server *server = global_network_game_server_get();
	short count = 0;
	long index;

	if (!server)
		return 0;
	for (index = 0; index < MAXIMUM_NETWORK_PLAYER_COUNT && count < maximum_count; index++)
	{
		struct network_player const *player = &server->game.players[index];

		if (!network_player_is_valid(player) || !VALID_INDEX(player->machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ||
			network_game_server_client_machine_is_local(server, &server->client_machines[player->machine_index]))
		{
			continue;
		}
		network_game_server_player_name_text(player, names[count], NETWORK_GAME_SERVER_NAME_TEXT_SIZE);
		if (network_game_server_name_begins_with(names[count], text))
			count++;
	}
	return count;
}

/* port: the host's ban command: the other machine of the player of the name
(in either case; else the one player whose name begins with it) dropped, its
address in bans.txt (network_distributed_ban), and kept out */
boolean network_game_server_ban_player(
	char const *text)
{
	struct network_game_server *server = global_network_game_server_get();
	long found_index = NONE;
	long match_count = 0;
	long exact_index = NONE;
	long exact_count = 0;
	long index;
	long machine_index;
	char names[96] = "";

	if (!server)
	{
		console_warning("ban: only the host of a game bans");
		return FALSE;
	}
	for (index = 0; index < MAXIMUM_NETWORK_PLAYER_COUNT; index++)
	{
		struct network_player const *player = &server->game.players[index];
		char name[NETWORK_GAME_SERVER_NAME_TEXT_SIZE];

		if (!network_player_is_valid(player))
			continue;
		network_game_server_player_name_text(player, name, sizeof(name));
		/* (the whole name first) */
		if (network_game_server_name_begins_with(name, text) && csstrlen(name) == csstrlen(text))
		{
			exact_index = index;
			exact_count++;
		}
		if (network_game_server_name_begins_with(name, text))
		{
			found_index = index;
			match_count++;
		}
	}
	if (exact_count > 1)
	{
		/* (the host numbers players of the same name: network_server_message_handler.c) */
		console_warning("ban: %ld players are named \"%s\"", exact_count, text);
		return FALSE;
	}
	if (exact_count == 1)
	{
		found_index = exact_index;
		match_count = 1;
	}
	if (!text[0])
	{
		console_warning("ban: give a player's name (Tab completes it)");
		return FALSE;
	}
	if (match_count > 1)
	{
		console_warning("ban: %ld players' names begin with \"%s\": give more of it", match_count, text);
		return FALSE;
	}
	if (match_count == 0)
	{
		console_warning("ban: no player's name begins with \"%s\"", text);
		return FALSE;
	}
	machine_index = server->game.players[found_index].machine_index;
	if (!VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ||
		!network_game_server_client_machine_is_joined_to_game(server, &server->client_machines[machine_index]) ||
		network_game_server_client_machine_is_local(server, &server->client_machines[machine_index]))
	{
		console_warning("ban: not a player of the host's own machine, nor one not joined");
		return FALSE;
	}
	/* (every player of that machine, named) */
	for (index = 0; index < MAXIMUM_NETWORK_PLAYER_COUNT; index++)
	{
		struct network_player const *player = &server->game.players[index];
		char name[NETWORK_GAME_SERVER_NAME_TEXT_SIZE];

		if (!network_player_is_valid(player) || player->machine_index != machine_index)
			continue;
		network_game_server_player_name_text(player, name, sizeof(name));
		if (names[0] && csstrlen(names) + 2 < sizeof(names))
			csstrcat(names, ", ");
		if (csstrlen(names) + csstrlen(name) < sizeof(names))
			csstrcat(names, name);
	}
	network_distributed_ban(machine_index, network_game_server_client_machine_addresses[machine_index], names);
	network_game_server_kick_pending[machine_index] = TRUE;
	return TRUE;
}

/* port: a client machine's hardware id as it told it joining (its join
request: network_server_message_handler.c), kept as hex only */
void network_game_server_set_machine_hardware_id(
	struct network_game_server_client_machine *machine,
	char const *hardware_id)
{
	if (machine && VALID_INDEX(machine->machine_index, MAXIMUM_NETWORK_MACHINE_COUNT))
	{
		p2p_hardware_id_sanitize(network_game_server_hardware_ids[machine->machine_index],
			P2P_HARDWARE_ID_SIZE, hardware_id);
	}
}

/* port: ... and as the distributed netcode logs it (empty if none told) */
char const *network_game_server_machine_hardware_id(
	long machine_index)
{
	return VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ?
		network_game_server_hardware_ids[machine_index] : "";
}

/* port: a joined client machine's IPv4 address (host byte order; 0 if
none), for the distributed netcode's log of cheaters */
unsigned long network_game_server_machine_address(
	long machine_index)
{
	return VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ?
		network_game_server_client_machine_addresses[machine_index] : 0;
}

/* port: the distributed netcode asks that a client machine be dropped (its
game ran faster than this one's: network_distributed.c); it is, once this
server next looks at its machines, not while its messages are read */
void network_game_server_kick_machine(
	long machine_index)
{
	struct network_game_server *server = global_network_game_server_get();

	if (!server || !VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ||
		!network_game_server_client_machine_is_joined_to_game(server, &server->client_machines[machine_index]) ||
		network_game_server_client_machine_is_local(server, &server->client_machines[machine_index]))
	{
		return;
	}
	network_game_server_kick_pending[machine_index] = TRUE;
}

/* (a client machine's player queued to add in game: one refused is as one
the game has no room for) */
static boolean network_game_server_machine_may_add_player_ingame(
	struct network_game_server *server,
	long machine_index)
{
	short *count;

	if (!VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ||
		network_game_server_client_machine_is_local(server, &server->client_machines[machine_index]))
	{
		return TRUE;
	}
	count = network_game_server_ingame_addition_count(network_game_server_client_machine_addresses[machine_index], FALSE);
	if (count && *count >= MAXIMUM_INGAME_ADDITIONS_PER_ADDRESS)
	{
		network_event("client machine #%ld added too many players in game", machine_index);
		return FALSE;
	}

	return TRUE;
}

/* ---------- public code */

struct network_game_server *network_game_server_create(
	void)
{
	struct network_game_server *server =
		&network_game_server_memory_do_not_use_directly;

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0xE0,
		!network_game_server_memory_do_not_use_directly_in_use);
	network_game_server_memory_do_not_use_directly_in_use = TRUE;
#ifdef HALO_WEB
	csmemset(&server_migration, 0, sizeof(server_migration));
#endif

	csmemset(server, 0, sizeof(*server));

	if (server != NULL)
	{
		server->connection = network_connection_new(
			FLAG(_connection_create_server_bit),
			NETWORK_GAME_SERVER_PORT);

		if (server->connection != NULL)
		{
			int i;

#ifdef xbox
			transport_server_initialize();
#endif

			server->state = _network_game_server_state_pregame;
			server->flags = FLAG(_network_game_server_game_valid_bit);
			csmemset(&server->game, 0, sizeof(server->game));

			network_connection_set_connection_rejection_procedure(
				server->connection,
				network_game_server_reject_connection_game_is_full);

			network_game_invalidate(&server->game);

			server->game.difficulty = main_get_difficulty();
			server->game.number_of_games_played = -1;

			for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
			{
				server->client_machines[i].connection = NULL;
				server->client_machines[i].last_received_update_sequence_number = 0;
				server->client_machines[i].last_heard_time = 0;
				server->client_machines[i].machine_index = NONE;
				server->client_machines[i].flags = 0;
				network_game_server_client_machine_addresses[i] = 0;
				network_game_invalidate_machine(&server->game, i);
			}

			server->sent_start_game_message = FALSE;
			server->time_of_first_client_loading_completion = 0;
			csmemset(network_game_server_start_players, NONE, sizeof(network_game_server_start_players));

			if (!network_game_server_reset_to_pregame(server))
			{
				error(
					_error_silent,
					"failed to initialize server pregame settings");
				network_game_server_dispose(server);
				server = NULL;
			}
		}
		else
		{
			error(
				_error_silent,
				"failed to create the server connection");
			network_game_server_dispose(server);
			server = NULL;
		}
	}

	return server;
}

void network_game_server_dispose(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x120, server);

	switch (server->state)
	{
	case _network_game_server_state_pregame:
	{
		/* (the Xbox game sent it uninitialised: stack bytes to every machine) */
		struct message_server_graceful_game_exit_pregame message_packet = { 0 };
		struct network_message *message;

		message = create_network_game_message(
			_message_server_graceful_game_exit_pregame,
			&message_packet,
			sizeof(message_packet));
		if (message != NULL)
		{
			if (network_game_server_send_message_to_all_machines(server, message))
				network_event("notified all clients that we are going down");
			else
				network_event("failed to notify all clients that we are going down");
		}
		else
		{
			network_event(
				"failed to create a _message_type_server_graceful_game_exit_pregame message");
		}

		break;
	}

	case _network_game_server_state_ingame:
		break;

	case _network_game_server_state_postgame:
	{
		struct message_server_graceful_game_exit_postgame message_packet = { 0 };
		struct network_message *message;

		message = create_network_game_message(
			_message_server_graceful_game_exit_postgame,
			&message_packet,
			sizeof(message_packet));
		if (message != NULL)
		{
			if (network_game_server_send_message_to_all_machines(server, message))
				network_event("notified all clients that we are going down");
			else
				network_event("failed to notify all clients that we are going down");
		}
		else
		{
			network_event(
				"failed to create a _message_type_server_graceful_game_exit_postgame message");
		}

		break;
	}
	}

	/* port: no longer waiting for the machines to load: a machine dropped
	below, the last the others waited for, or the last to say it has loaded,
	would start the game, whose local client (and its loaded game) is gone;
	and no longer open */
	server->time_of_first_client_loading_completion = 0;
	server->sent_start_game_message = FALSE;
	SET_FLAG(server->flags, _network_game_server_game_open_bit, FALSE);
	if (!network_game_server_handle_client_machines(server))
	{
		error(
			_error_silent,
			"network_game_server_handle_client_machines() failed inside network_game_server_dispose()");
	}

	if (server->connection)
		network_connection_delete(server->connection);

#ifdef xbox
	SleepEx(MILLISECONDS_PER_SECOND, FALSE);
	transport_server_terminate();
#endif

	csmemset(server, 0, sizeof(*server));

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0x171,
		network_game_server_memory_do_not_use_directly_in_use);
	network_game_server_memory_do_not_use_directly_in_use = FALSE;

	p2p_set_game_player_counts(0, 0);
	network_event("network server disposed");

	return;
}

/* port: whether the host's network is gone: at once but in a game, where a
link that comes back within NETWORK_GAME_SERVER_CLIENT_TIMEOUT (an address
renewed, a wifi drop) goes on (a check more than two seconds after the last
starts the count over) */
static boolean network_game_server_network_lost(
	struct network_game_server *server)
{
	static unsigned long down_time;
	static unsigned long checked_time;
	static boolean asked;
	static boolean available_asked;
	static unsigned long asked_time;
	unsigned long now = system_milliseconds();
	boolean in_game = server->state == _network_game_server_state_ingame ||
		server->state == _network_game_server_state_postgame;
	boolean recent = checked_time && now - checked_time <= 2000;
	boolean available;

	/* (asked once a second: each asking lists the machine's interfaces) */
	if (!asked || now - asked_time >= 1000)
	{
		available_asked = transport_network_available();
		asked_time = now;
		asked = TRUE;
	}
	available = available_asked;

	checked_time = now;
	if (available)
	{
		down_time = 0;
		return FALSE;
	}
	if (!in_game)
		return TRUE;
	if (!down_time || !recent)
		down_time = now;
	return now - down_time > NETWORK_GAME_SERVER_CLIENT_TIMEOUT;
}

boolean network_game_server_idle(
	struct network_game_server *server)
{
	boolean success = TRUE;
#ifdef HALO_WEB
	network_game_server_migration_expire_disconnected(server);
#endif

	if (network_game_server_network_lost(server))
	{
		if (!network_game_is_splitscreen_local())
		{
			display_error_when_main_menu_loaded(_error_network_connection_lost);
			error(_error_silent, "network connection went down!");
			success = FALSE;
			goto exit;
		}
	}

	/* (what Discord shows of a game hosted for internet play) */
	p2p_set_game_player_counts(server->game.player_count, server->game.maximum_players);

	if (network_game_server_game_is_valid(server))
	{
		struct network_connection *new_client_connection = NULL;

		success = network_connection_idle(
			server->connection,
			_connection_dont_timeout,
			&new_client_connection);
		if (success == TRUE)
		{
			if (new_client_connection)
			{
				success = network_game_server_add_new_client(
					server,
					new_client_connection);
				if (success == TRUE)
				{
					struct transport_address client_address;

					network_connection_get_address(
						new_client_connection,
						&client_address,
						NULL);
					network_event(
						"new client connected from ip %s (validation pending)",
						transport_address_to_string(&client_address));
				}
				else
				{
					network_event(
						"failed to add new client connection to the game");
					success = network_server_close_client_connection(
						server->connection,
						new_client_connection);
				}
			}

			success = network_game_server_handle_public_endpoint(server);
			if (success)
			{
				success = network_game_server_handle_client_machines(server);
				if (success)
				{
					switch (server->state)
					{
					case _network_game_server_state_pregame:
						success = network_game_server_idle_pregame_tasks(server);
						break;

					case _network_game_server_state_ingame:
						network_game_server_keep_late_joiners_alive(server);
						break;

					case _network_game_server_state_postgame:
						success = network_game_server_idle_postgame_tasks(server);
						break;

					default:
						network_event("unknown server state");
						success = FALSE;
						break;
					}
				}
				else
				{
					network_event(
						"network_game_server_handle_client_machines() failed");
				}
			}
			else
			{
				network_event(
					"network_game_server_handle_public_endpoint() failed");
			}
		}
		else
		{
			network_event("network_connection_idle() failed");
		}
	}
	else
	{
		network_event("the server's game is invalid");
	}

exit:
	return success;
}

void countdown_timer_update(
	struct countdown_timer *timer)
{
	unsigned long update_time = system_milliseconds();
	/* (the time since, which the milliseconds' wrap leaves right: not
	which time is greater) */
	unsigned long elapsed_time = update_time - timer->last_update_time;

	if (elapsed_time < (unsigned long)timer->time_remaining)
		timer->time_remaining -= (long)elapsed_time;
	else
		timer->time_remaining = 0;

	timer->last_update_time = update_time;

	return;
}

long countdown_timer_get_time_remaining(
	struct countdown_timer *timer)
{
	long time_remaining;
	unsigned long update_time = system_milliseconds();
	unsigned long elapsed_time = update_time - timer->last_update_time;

	if (elapsed_time < (unsigned long)timer->time_remaining)
		timer->time_remaining -= (long)elapsed_time;
	else
		timer->time_remaining = 0;

	time_remaining = timer->time_remaining;
	timer->last_update_time = update_time;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x5F,
		timer->time_remaining >= 0);

	return time_remaining;
}

void countdown_timer_increment(
	struct countdown_timer *timer,
	long adjustment,
	long maximum)
{
	countdown_timer_update(timer);

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x68, adjustment >= 0);

	if (timer->time_remaining + adjustment < adjustment)
	{
		timer->time_remaining = maximum;
	}
	else
	{
		timer->time_remaining += adjustment;
		timer->time_remaining = MIN(timer->time_remaining, maximum);
	}

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0x75,
		timer->time_remaining >= 0);

	return;
}

void countdown_timer_decrement(
	struct countdown_timer *timer,
	long adjustment)
{
	unsigned long update_time = system_milliseconds();
	unsigned long elapsed_time = update_time - timer->last_update_time;

	if (elapsed_time < (unsigned long)timer->time_remaining)
		timer->time_remaining -= (long)elapsed_time;
	else
		timer->time_remaining = 0;

	timer->last_update_time = update_time;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x7E, adjustment >= 0);

	if (timer->time_remaining > adjustment)
		timer->time_remaining -= adjustment;
	else
		timer->time_remaining = 0;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x89,
		timer->time_remaining >= 0);

	return;
}

struct network_connection *network_game_server_get_client_connection(
	struct network_game_server_client_machine *client_machine)
{
	if (client_machine)
		return client_machine->connection;

	return NULL;
}

struct network_connection *network_game_server_get_connection(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x712, server);

	return server->connection;
}

struct network_game *network_game_server_get_game(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x769, server);

	return &server->game;
}

wchar_t *network_game_server_get_game_name(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x1E9, server);

	return server->game.name;
}

boolean network_game_server_set_game_name(
	struct network_game_server *server,
	wchar_t const *name)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x1DD, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x1DE, name);

	ustrncpy(server->game.name, name, NETWORK_GAME_NAME_LENGTH - 1);
	server->game.name[NETWORK_GAME_NAME_LENGTH - 1] = 0;

	return FALSE;
}

word network_game_server_get_state(
	struct network_game_server *server,
	short *substate)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x1F2, server);

	if (substate)
		*substate = 0;

	return server->state;
}

void network_game_server_open_game(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x1FC, server);

	SET_FLAG(server->flags, _network_game_server_game_open_bit, TRUE);
	network_server_allow_client_connections(server->connection, TRUE);
	network_event("opening game");

	return;
}

boolean network_game_server_start_network_game(
	struct network_game_server *server)
{
	boolean success = TRUE;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x2DE, server);

	if (server->sent_start_game_message == FALSE)
	{
		struct message_server_begin_game begin_game = { 0 };
		void *message;

		/* the settings record goes out in pieces */
		/* port: once the settings are out, the start is sent once: a machine
		that missed it (its connection failed, and is closed) is dropped, and
		sent again, the start reached those that loaded it already (which
		refuse a second start) and the wait for the others never began */
		if (network_game_server_send_game_settings_to_all_machines(server, &server->game, sizeof(server->game)) &&
			((message = create_network_game_message(
				_message_server_begin_game,
				&begin_game,
				sizeof(begin_game))) != NULL))
		{
			if (network_game_server_send_message_to_all_machines(server, message))
				network_event("signalling client machines to begin loading for network game");
			else
				network_event("signalling client machines to begin loading for network game (some machines missed it)");
			server->sent_start_game_message = TRUE;
			csmemcpy(network_game_server_start_players, server->game.players,
				sizeof(network_game_server_start_players));
			success = TRUE;
		}
		else
		{
			network_event("failed to signal client machines to begin loading for network game");
		}
	}

	server->next_update_number = 0;

	return success;
}

static void network_game_server_send_player_quit_messages_ingame(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	long player_index;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x267,
		_network_game_server_state_ingame == server->state);

	for (player_index = 0;
		player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
		player_index++)
	{
		struct network_player *player = &server->game.players[player_index];

		if (network_player_is_valid(player) &&
			player->machine_index == machine->machine_index)
		{
			struct message_server_remove_player_ingame remove_player;
			void *message;

			remove_player.player = *player;
			remove_player.reason = game_time_get() + NETWORK_GAME_PLAYER_QUIT_DELAY;

			error(_error_silent, "sending quit out of game, time = %x", remove_player.reason);

			message = create_network_game_message(
				_message_server_remove_player_ingame,
				&remove_player,
				sizeof(remove_player));
			if (message &&
				!network_game_server_send_message_to_all_machines(server, message))
			{
				network_event(
					"network_game_server_send_message_to_all_machines() failed in network_game_server_handle_message_client_remove_player_request_ingame()");
			}
		}
	}

	return;
}

void network_game_server_switch_to_postgame(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x2FF, server);

	if (server->state == _network_game_server_state_ingame)
	{
		struct message_server_game_over game_over = { 0 };
		void *message;

		server->state = _network_game_server_state_postgame;

		message = create_network_game_message(
			_message_server_game_over,
			&game_over,
			sizeof(game_over));
		if (message)
		{
			if (network_game_server_send_message_to_all_machines(server, message))
				network_event("server sent message_game_over to all clients");
			else
				network_event("failed to signal all client machines to switch to postgame");
		}
		else
		{
			network_event("failed to create a _message_type_server_game_over message");
		}
	}

	return;
}

boolean network_game_server_graceful_shutdown(
	struct network_game_server *server)
{
	boolean success = FALSE;
	void *message = NULL;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x39F, server);

	switch (network_game_server_get_state(server, NULL))
	{
		case _network_game_server_state_pregame:
		{
			struct message_server_graceful_game_exit_pregame exit_pregame = { 0 };

			message = create_network_game_message(
				_message_server_graceful_game_exit_pregame,
				&exit_pregame,
				sizeof(exit_pregame));
			if (!message)
				network_event("failed to create a message_server_graceful_game_exit_pregame");
		}
		break;

		case _network_game_server_state_postgame:
		{
			struct message_server_graceful_game_exit_postgame exit_postgame = { 0 };

			message = create_network_game_message(
				_message_server_graceful_game_exit_postgame,
				&exit_postgame,
				sizeof(exit_postgame));
			if (!message)
				network_event("failed to create a message_server_graceful_game_exit_postgame");
		}
		break;
	}

	if (message)
	{
		success = network_game_server_send_message_to_all_machines(server, message);
		if (success == TRUE)
		{
			network_event(
				"server closing down; all client machines were properly informed");
		}
		else
		{
			network_event(
				"server going down, but failed to properly inform all client machines");
		}
	}

	return success;
}

void countdown_timer_set_time_remaining(
	struct countdown_timer *timer,
	long time_remaining)
{
	unsigned long update_time = system_milliseconds();

	timer->time_remaining = time_remaining;
	timer->last_update_time = update_time;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x95, timer->time_remaining >= 0);

	return;
}

void network_game_server_client_machine_is_precached(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	char const *map_name)
{
	char const *multiplayer_map_name = main_get_multiplayer_map_name();

	if (!csstrcmp(multiplayer_map_name, map_name))
		SET_FLAG(client_machine->flags, _network_client_machine_precached_bit, TRUE);

	return;
}

boolean network_game_server_game_is_open(
	struct network_game_server *server)
{
	boolean game_is_open;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x214, server);

	game_is_open = TEST_FLAG(server->flags, _network_game_server_game_open_bit);
	/* port: the game stays open in progress (a machine may join it), but
	not while the machines load it, nor once it is over */
	if ((server->state == _network_game_server_state_pregame && server->sent_start_game_message) ||
		server->state == _network_game_server_state_postgame)
	{
		game_is_open = FALSE;
	}

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x217,
		(TRUE == game_is_open) || (FALSE == game_is_open));

	return game_is_open;
}

boolean network_game_server_game_is_valid(
	struct network_game_server *server)
{
	boolean game_is_valid;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x220, server);

	game_is_valid = TEST_FLAG(server->flags, _network_game_server_game_valid_bit);

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x223,
		(TRUE == game_is_valid) || (FALSE == game_is_valid));

	return game_is_valid;
}

boolean network_game_server_accept_client_machine_into_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	boolean success = FALSE;
	long machine_index;
#ifdef HALO_WEB
	if (server_migration.adopting)
		return FALSE;
#endif

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x3DA, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x3DB, machine);

	/* port: into its own slot (network_game_server_add_new_client numbered
	it by it), not the first free one: another connection's slot gave two
	machines one index */
	machine_index = machine->machine_index;
	/* port: not a machine of an address dropped for cheating */
	{
		struct transport_address address = { { { 0 } } };
		long index;

		network_connection_get_address(machine->connection, &address, FALSE);
		for (index = 0; index < MAXIMUM_KICKED_ADDRESSES && address.address.long_words[0]; index++)
		{
			if (network_game_server_kicked_addresses[index] == address.address.long_words[0])
			{
				network_event("refusing a machine @ %s: dropped from this game for cheating",
					transport_address_to_string(&address));
				return FALSE;
			}
		}
		/* (and not one the host banned: its address or hardware id in
		bans.txt) */
		/* (the host's own client, which connects from 127.0.0.1, never) */
		if (!network_game_server_client_machine_is_local(server, machine) &&
			address.address.long_words[0] != IPV4_LOOPBACK_ADDRESS &&
			network_distributed_banned(address.address.long_words[0],
				VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ?
					network_game_server_hardware_ids[machine_index] : ""))
		{
			network_event("refusing a machine @ %s: banned (bans.txt)", transport_address_to_string(&address));
			return FALSE;
		}
	}
	/* (a kick asked for the slot's machine before is not this one's) */
	if (VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT))
		network_game_server_kick_pending[machine_index] = FALSE;
	if (VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) &&
		machine == &server->client_machines[machine_index])
	{
		if (!network_machine_is_valid(&server->game.machines[machine_index]))
		{
			struct network_machine new_machine;

			csmemcpy(
				&new_machine,
				&server->game.machines[machine_index],
				sizeof(new_machine));
			new_machine.machine_index = (char)machine_index;

			success = network_game_add_machine(&server->game, &new_machine);
			if (success == TRUE)
			{
				struct transport_address address = { { { 0 } } };

				network_connection_get_address(
					machine->connection,
					&address,
					FALSE);

#ifdef HALO_WEB
				if (server_migration.epoch)
				{
					server_migration.disconnected_at[machine_index] = 0;
					server_migration.owner_addresses[machine_index] = address.address.long_words[0] == IPV4_LOOPBACK_ADDRESS ?
						web_quick_play_address() : address.address.long_words[0];
				}
#endif

				network_event(
					"server added machine @ %s to the game at machine index #%d",
					transport_address_to_string(&address),
					machine_index);

				SET_FLAG(
					machine->flags,
					_network_client_machine_validated_bit,
					TRUE);
				machine->machine_index = (short)machine_index;

				/* port: a machine that joins again from its address before
				its old connection is found dead: the old one goes (the
				datagrams from the address are the new one's, and were taken
				for the old slot's). Two machines of one address are not in a
				game: the client's port is fixed, and internet play gives each
				machine its own address. */
				{
					long other_index;

					for (other_index = 0; other_index < MAXIMUM_NETWORK_MACHINE_COUNT; other_index++)
					{
						struct network_game_server_client_machine *other = &server->client_machines[other_index];

						if (other != machine &&
							network_game_server_client_machine_is_joined_to_game(server, other) &&
							network_game_server_client_machine_addresses[other_index] == address.address.long_words[0] &&
							!network_game_server_client_machine_is_local(server, other))
						{
							network_event("machine #%ld joined from the address of machine #%ld, which is dropped",
								machine_index, other_index);
							network_game_server_drop_client_machine(server, other);
						}
					}
				}
				network_game_server_client_machine_addresses[machine_index] = address.address.long_words[0];
				network_game_server_client_machine_join_times[machine_index] = system_milliseconds();
			}
			else
			{
				network_event(
					"network_game_add_machine() failed in network_game_server_accept_client_machine_into_game()");
			}
		}
	}

	if (!success)
	{
		network_event(
			"network_game_server_accept_client_machine_into_game() failed to find an available opening for the new machine");
	}

	return success;
}

boolean network_game_server_client_machine_is_joined_to_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x3CD, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x3CE, machine);

	return TEST_FLAG(machine->flags, _network_client_machine_validated_bit);
}

boolean network_game_server_switch_machine_from_postgame_to_pregame(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x547, server && machine);

	network_event(
		"machine #%d has successfully switched to pregame",
		machine->machine_index);
	SET_FLAG(machine->flags, _network_client_machine_level_loaded_bit, FALSE);

	return TRUE;
}

void network_game_server_all_machines_have_loaded(
	struct network_game_server *server)
{
	long client_machine_index;

	network_event("all machines have successfully loaded");

	server->state = _network_game_server_state_ingame;
	server->time_of_first_client_loading_completion = 0;
	/* (the machines that loaded first waited in silence: their timeouts
	start now) */
	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		if (network_game_server_client_machine_is_joined_to_game(server, client_machine))
			network_game_server_client_machine_heard(server, client_machine);
		/* (the host's game update every tick for the game's first second) */
		network_game_server_frequent_updates_until[client_machine_index] =
			server->next_update_number + TICKS_PER_SECOND;
	}
	network_game_server_remove_players_gone_while_loading(server);
	server->game.local_data.game_objects_loaded = global_network_game_client_get()
		? network_game_client_get_game(global_network_game_client_get())->local_data.game_objects_loaded
		: FALSE;

	match_vassert(NETWORK_SERVER_MANAGER_FILE, 0x4E0, server->game.local_data.game_objects_loaded,
		"local game data not loaded");

	return;
}

void network_game_server_client_machine_game_loading_complete(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	boolean all_machines_loaded = TRUE;
	long client_machine_index;
	long loading_machine_count = 0;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4ED, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4EE, machine);

	/* port: not in the lobby, before the game starts: the wait for the
	others would have run out by the start */
	if (!server->sent_start_game_message)
	{
		network_event("ignoring machine #%d's load before the game started", machine->machine_index);
		return;
	}

	SET_FLAG(machine->flags, _network_client_machine_level_loaded_bit, TRUE);
	if (!server->time_of_first_client_loading_completion)
		server->time_of_first_client_loading_completion = system_milliseconds();

	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		/* (the machines in the game: not a connection that has not joined) */
		if (network_game_server_client_machine_is_joined_to_game(server, client_machine) &&
			!TEST_FLAG(client_machine->flags, _network_client_machine_level_loaded_bit))
		{
			loading_machine_count++;
			all_machines_loaded = FALSE;
		}
	}

	/* a line per machine still loading, each time one finishes, is 8,000
	lines as 128 machines load, and the host writes its log a line at a time */
	if (loading_machine_count)
	{
		network_event("still waiting for machines to finish loading (%ld left)", loading_machine_count);
	}

	if (all_machines_loaded == TRUE)
		network_game_server_all_machines_have_loaded(server);

	return;
}

/* port: the machines still loading the game have gone (a machine that left,
the last it waited for, left it waiting out the wait for the others) */
static void network_game_server_check_loading_complete(
	struct network_game_server *server)
{
	long client_machine_index;

	if (server->state != _network_game_server_state_pregame ||
		!server->sent_start_game_message ||
		!server->time_of_first_client_loading_completion)
	{
		return;
	}
	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		if (network_game_server_client_machine_is_joined_to_game(server, client_machine) &&
			!TEST_FLAG(client_machine->flags, _network_client_machine_level_loaded_bit))
		{
			return;
		}
	}
	network_game_server_all_machines_have_loaded(server);
}

static boolean network_game_server_same_player(
	struct network_player const *player0,
	struct network_player const *player1);
static boolean network_game_server_player_among(
	struct network_player const *player,
	struct network_player *players,
	long count);

/* port: the players of the machines that left while the game loaded: every
machine spawned them from the settings the game started with, and none was
told they quit (the server was in the pregame, which sends no quit messages):
they quit now */
static void network_game_server_remove_players_gone_while_loading(
	struct network_game_server *server)
{
	long count = (long)NUMBEROF(server->game.players);
	long index;

	for (index = 0; index < count; index++)
	{
		struct network_player *player = &network_game_server_start_players[index];
		struct message_server_remove_player_ingame remove_player;
		void *message;

		if (!network_player_is_valid(player) || network_game_server_player_among(player, server->game.players, count))
			continue;
		remove_player.player = *player;
		remove_player.reason = game_time_get() + NETWORK_GAME_PLAYER_QUIT_DELAY;
		message = create_network_game_message(_message_server_remove_player_ingame, &remove_player,
			sizeof(remove_player));
		if (message && !network_game_server_send_message_to_all_machines(server, message))
			network_event("some machines missed the quit of a player gone while the game loaded");
		network_event("a player gone while the game loaded quits (machine #%d / controller #%d)",
			player->machine_index, player->controller_index);
	}
	csmemset(network_game_server_start_players, NONE, sizeof(network_game_server_start_players));
}

void network_game_server_handle_client_update_packet(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	struct message_client_game_update *message_packet)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x51E, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x51F, machine);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x520, message_packet);

	/* (a client's out of sync, the update number's top bit, was the
	lockstep netcode's, and ended the game: the distributed netcode's machines
	are never in sync, and the host's game is the game) */
	if ((message_packet->update_number & CLIENT_UPDATE_SEQUENCE_NUMBER_MASK) <
		machine->last_received_update_sequence_number)
	{
		/* (a datagram overtaken by a newer: dropped without a line in the
		log for each, ten times a second from every machine) */
	}
	else if (message_packet->player_count < 0 ||
		message_packet->player_count > MAXIMUM_PLAYERS_PER_MACHINE)
	{
		network_event(
			"client update packet from machine #%d had a bad player count; ignoring",
			machine->machine_index);
	}
	else
	{
		struct player_action actions[MAXIMUM_PLAYERS_PER_MACHINE] = { 0 };
		long player_index;

		for (player_index = 0;
			player_index < message_packet->player_count;
			player_index++)
		{
			actions[player_index] = message_packet->actions[player_index];
		}

		update_server_handle_client_update(machine->machine_index, actions);

		machine->last_received_update_sequence_number =
			message_packet->update_number & CLIENT_UPDATE_SEQUENCE_NUMBER_MASK;
	}

	return;
}

boolean network_game_server_add_player_to_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	struct network_player *player)
{
	boolean success;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x46C, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x46D, machine);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x46E, player);

	if (machine->machine_index == player->machine_index)
	{
		/* port: the smaller team (the Xbox game took turns with a counter of
		its own, which a failed add or an earlier game moved on) */
		{
			long player_count_by_team[NUMBER_OF_MULTIPLAYER_TEAMS] = { 0, 0 };
			long player_index;

			for (player_index = 0; player_index < MAXIMUM_NETWORK_PLAYER_COUNT; player_index++)
			{
				struct network_player *other_player = &server->game.players[player_index];

				if (network_player_is_valid(other_player) &&
					VALID_INDEX(other_player->team_index, NUMBER_OF_MULTIPLAYER_TEAMS))
				{
					player_count_by_team[other_player->team_index]++;
				}
			}
			player->team_index = player_count_by_team[1] < player_count_by_team[0] ? 1 : 0;
		}

		/* (the name comes from the wire) */
		player->name[NETWORK_PLAYER_NAME_LENGTH - 1] = 0;
#ifdef HALO_WEB
		if (network_game_player_name_is_blank(player->name))
#else
		if (!player->name[0])
#endif
			get_unique_random_name(server, player);

		if (!player_name_is_unique(server, player->name))
			get_unique_random_name(server, player);

		if (player->primary_color_index == NONE)
			get_unique_random_color(server, player);

		/* (the host chooses the player's slot, which is its datum on every
		machine: network_game_add_player) */
		player->player_list_index = NONE;
		success = network_game_add_player(&server->game, player);
		if (success == TRUE)
		{
			network_event(
				"server added player from machine #%d at controller index #%d to the game",
				player->machine_index,
				player->controller_index);
		}
		else
		{
			network_event(
				"network_game_add_player() failed in network_game_server_add_player_to_game()");
		}
	}
	else
	{
		network_event(
			"client machine tried to add a player with a non-matching machine identifier");
		success = FALSE;
	}

	return success;
}

void network_game_server_update_ticks(
	struct network_game_server *server,
	short tick_count)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x553, server);

	switch (network_game_server_get_state(server, NULL))
	{
		case _network_game_server_state_ingame:
		{
			struct network_game_server_client_machine *client_machine = NULL;
			long client_machine_index;
			short tick_index;

			for (tick_index = 0; tick_index < tick_count; tick_index++)
			{
				struct message_server_game_update game_update;
				long update_number = server->next_update_number++;
				void *message;

				update_server_next_update();

				/* (the distributed netcode relays the actions unreliably, each
				tick's buttons with the next ticks', network_distributed.c: this
				update only keeps the clients' count of the host's ticks and
				gives them the host's time, which is all they read of it; with
				no players, none of its actions is sent or read) */
				game_update.update_number = update_number;
				game_update.random_seed = 0;
				game_update.game_time = game_time_get();
				game_update.unknown0C = 0;
				game_update.player_count = 0;

				message = create_network_game_message(
					_message_server_game_update,
					&game_update,
					sizeof(game_update));
				/* port: a machine takes only the update's number and time (its
				clock starts with the first, and follows the host's time), not a
				reliable message and its acknowledgement every tick: every tick
				for a machine's first second in the game, then once a second (a
				machine still loading the game in progress gets none, as it gets
				none of the game's messages) */
				if (message)
				{
					boolean sent = TRUE;

					for (client_machine_index = 0;
						client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
						client_machine_index++)
					{
						struct network_game_server_client_machine *machine =
							&server->client_machines[client_machine_index];

						if (network_game_server_client_machine_is_joined_to_game(server, machine) &&
							TEST_FLAG(machine->flags, _network_client_machine_level_loaded_bit) &&
							(update_number < network_game_server_frequent_updates_until[client_machine_index] ||
								update_number % TICKS_PER_SECOND == 0) &&
							network_connection_active(machine->connection) &&
							!network_game_server_send_message_to_client_machine(server, machine, message))
						{
							sent = FALSE;
						}
					}
					if (!sent)
					{
						network_event(
							"server failed to send game update message to all machines; client machine may be out of sync");
					}
				}
			}

			if (!server->queued_player_valid && server->waiting_player_count > 0)
			{
				csmemcpy(&server->queued_player, &server->waiting_players[0], sizeof(server->queued_player));
				server->waiting_player_count--;
				csmemmove(
					&server->waiting_players[0],
					&server->waiting_players[1],
					server->waiting_player_count * sizeof(struct network_player));
				server->queued_player_valid = TRUE;
			}
			if (server->queued_player_valid)
			{
				/* (a machine's slot is its index; the handlers queued only a
				machine's own players) */
				client_machine_index = server->queued_player.machine_index;
				if (VALID_INDEX(client_machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) &&
					network_game_server_client_machine_is_joined_to_game(server,
						&server->client_machines[client_machine_index]))
				{
					client_machine = &server->client_machines[client_machine_index];
				}

				if (client_machine &&
					network_game_server_machine_may_add_player_ingame(server, client_machine_index) &&
					network_game_server_add_player_to_game(
						server,
						client_machine,
						&server->queued_player))
				{
					if (!network_game_server_client_machine_is_local(server, client_machine))
					{
						short *count = network_game_server_ingame_addition_count(
							network_game_server_client_machine_addresses[client_machine_index], TRUE);

						if (count)
							(*count)++;
					}
					if (!network_game_server_send_player_joined_info_ingame(
						server,
						&server->queued_player))
					{
						network_event(
							"network_game_server_send_player_joined_info_ingame() failed in network_game_server_handle_message_client_add_player_request_ingame()");
					}
					/* a machine joining the game in progress, its players all in:
					it loads the game now */
					if (!TEST_FLAG(client_machine->flags, _network_client_machine_level_loaded_bit) &&
						!TEST_FLAG(client_machine->flags, _network_client_machine_started_late_bit) &&
						!network_game_server_machine_has_waiting_players(server, client_machine->machine_index))
					{
						network_game_server_start_late_joiner(server, client_machine);
					}
				}
				else
				{
					network_event("server failed to add a network player in-game");
					/* port: a machine joining the game in progress whose last
					player could not join (the game is full): started with those
					that did, or refused, not left waiting for ever */
					if (client_machine &&
						!TEST_FLAG(client_machine->flags, _network_client_machine_level_loaded_bit) &&
						!TEST_FLAG(client_machine->flags, _network_client_machine_started_late_bit) &&
						!network_game_server_machine_has_waiting_players(server, client_machine->machine_index))
					{
						if (network_game_server_machine_has_players(server, client_machine->machine_index))
							network_game_server_start_late_joiner(server, client_machine);
						else
							network_game_server_refuse_late_joiner(server, client_machine, _rejection_code_game_is_full);
					}
				}

				server->queued_player_valid = FALSE;
			}
		}
		break;

		case _network_game_server_state_postgame:
			game_engine_update();
			break;
	}

	return;
}

/* ---------- joining a game in progress (the distributed netcode's)

A machine may join a distributed game in progress: the host keeps the game
open, accepts the machine's join as in the pregame, and adds its players as
it adds a player in game (every machine in the game spawns them, told by
_message_server_add_player_ingame). Then the host sends the machine alone
the settings and the start, with its game time, and the machine loads the
game and runs its clock from there; the distributed netcode gives it the
host's objects when it has loaded (network_objects.c). Until then the
machine hears none of the game's messages (network_game_server_send_message_to_all_machines),
which a machine in the pregame would refuse. */

boolean network_game_server_accepts_late_joins(
	struct network_game_server *server)
{
	/* (and a player can join it: a machine none of whose players can is
	refused, network_game_server_refuse_late_joiner) */
	return server->state == _network_game_server_state_ingame &&
		network_game_server_game_is_open(server) &&
		network_game_has_free_player_slot(&server->game);
}

boolean network_game_server_client_machine_is_loaded(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	(void)server;
	return TEST_FLAG(machine->flags, _network_client_machine_level_loaded_bit);
}

/* whether players of the machine are still waiting to be added (after the
one being added) */
static boolean network_game_server_machine_has_waiting_players(
	struct network_game_server *server,
	long machine_index)
{
	long index;

	for (index = 0; index < server->waiting_player_count; index++)
	{
		if (server->waiting_players[index].machine_index == machine_index)
			return TRUE;
	}
	return FALSE;
}

/* whether players of the machine are in the game */
static boolean network_game_server_machine_has_players(
	struct network_game_server *server,
	long machine_index)
{
	long index;

	for (index = 0; index < MAXIMUM_NETWORK_PLAYER_COUNT; index++)
	{
		if (network_player_is_valid(&server->game.players[index]) &&
			server->game.players[index].machine_index == machine_index)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* a machine joining the game in progress none of whose players could join
(the game filled up): told the game is full, and let go */
static void network_game_server_refuse_late_joiner(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	word reason)
{
	struct message_server_machine_rejected rejection = { reason };
	struct network_message *message;

	network_event("refusing machine #%d: no player of it can join the game", machine->machine_index);
	message = create_network_game_message(_message_server_machine_rejected, &rejection, sizeof(rejection));
	if (message)
		network_game_server_send_message_to_client_machine(server, machine, message);
	network_game_server_drop_client_machine(server, machine);
}

/* the players in the settings each machine joining the game in progress
was started with (by machine index): those added and gone while it loaded
it, whose messages it did not hear, it is told of once it has */
static struct network_player late_joiner_players[MAXIMUM_NETWORK_MACHINE_COUNT][NUMBEROF(((struct network_game *)0)->players)];

static boolean network_game_server_same_player(
	struct network_player const *player0,
	struct network_player const *player1)
{
	return player0->machine_index == player1->machine_index &&
		player0->controller_index == player1->controller_index &&
		player0->player_list_index == player1->player_list_index;
}

/* whether the player is valid and one of the players */
static boolean network_game_server_player_among(
	struct network_player const *player,
	struct network_player *players,
	long count)
{
	long index;

	for (index = 0; index < count; index++)
	{
		if (network_player_is_valid(&players[index]) && network_game_server_same_player(&players[index], player))
			return TRUE;
	}
	return FALSE;
}

/* the settings (with the machine's players) and the start, to the machine
alone, the start with the host's game time */
static void network_game_server_start_late_joiner(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	struct message_server_begin_game begin_game = { 0 };
	struct network_message *message;

	/* (the message carries 16 bits: the client has the rest from its first
	game update; never 0, which is a game starting) */
	begin_game.unused = MAX(game_time_get() & 0xFFFF, 1);
	if (!network_game_server_send_game_settings_to_client_machine(server, machine, &server->game, sizeof(server->game)) ||
		!(message = create_network_game_message(_message_server_begin_game, &begin_game, sizeof(begin_game))) ||
		!network_game_server_send_message_to_client_machine(server, machine, message))
	{
		network_event("failed to start machine #%d in the game in progress", machine->machine_index);
		return;
	}
	/* (its players at the start are told apart from those of an earlier
	machine at its index by this: network_game_server_late_joiner_loaded) */
	SET_FLAG(machine->flags, _network_client_machine_started_late_bit, TRUE);
	machine->last_heard_time = system_milliseconds();
	if (machine->machine_index >= 0 && machine->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)
	{
		csmemcpy(late_joiner_players[machine->machine_index], server->game.players,
			sizeof(late_joiner_players[machine->machine_index]));
	}
	network_event("machine #%d joins the game in progress at game tick #%ld", machine->machine_index,
		begin_game.unused);
}

/* the machines joining the game in progress (in the pregame, or loading)
hear none of the game's messages, and drop a connection they hear nothing on
for 15 seconds: a pregame keep-alive every 5 */
static void network_game_server_keep_late_joiners_alive(
	struct network_game_server *server)
{
	unsigned long now = system_milliseconds();
	struct message_server_pregame_keep_alive message_packet = { 0 };
	long client_machine_index;

	if (now - server->time_of_last_keep_alive <= 5UL * MILLISECONDS_PER_SECOND)
		return;
	server->time_of_last_keep_alive = now;
	for (client_machine_index = 0; client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT; client_machine_index++)
	{
		struct network_game_server_client_machine *machine = &server->client_machines[client_machine_index];
		struct network_message *message;

		if (!network_game_server_client_machine_is_joined_to_game(server, machine) ||
			TEST_FLAG(machine->flags, _network_client_machine_level_loaded_bit))
		{
			continue;
		}
		message = create_network_game_message(_message_server_pregame_keep_alive, &message_packet,
			sizeof(message_packet));
		if (message)
			network_game_server_send_message_to_client_machine(server, machine, message);
	}
}

/* a machine that joined the game in progress has loaded it (or it loaded
it as the game ended: in the postgame, it is only loaded) */
void network_game_server_late_joiner_loaded(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	if (server->state == _network_game_server_state_ingame &&
		!TEST_FLAG(machine->flags, _network_client_machine_started_late_bit))
	{
		network_event("ignoring machine #%d's load of a game it was not started in", machine->machine_index);
		return;
	}
	SET_FLAG(machine->flags, _network_client_machine_level_loaded_bit, TRUE);
	machine->last_heard_time = system_milliseconds();
	network_event("machine #%d has loaded the game in progress", machine->machine_index);
	/* (the host's game update every tick for its first second, which sets
	its clock) */
	if (machine->machine_index >= 0 && machine->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)
	{
		network_game_server_frequent_updates_until[machine->machine_index] =
			server->next_update_number + TICKS_PER_SECOND;
	}
	if (server->state == _network_game_server_state_ingame &&
		machine->machine_index >= 0 && machine->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)
	{
		struct network_player *started = late_joiner_players[machine->machine_index];
		long count = (long)NUMBEROF(server->game.players);
		long index;

		/* the players gone while it loaded (first: a player's machine index
		may be a new machine's, which a player added is told apart from by
		that only) */
		for (index = 0; index < count; index++)
		{
			struct network_player *player = &started[index];
			struct message_server_remove_player_ingame remove_player;
			void *message;

			if (!network_player_is_valid(player) || network_game_server_player_among(player, server->game.players, count))
				continue;
			remove_player.player = *player;
			remove_player.reason = game_time_get();
			message = create_network_game_message(_message_server_remove_player_ingame, &remove_player,
				sizeof(remove_player));
			if (message)
				network_game_server_send_message_to_client_machine(server, machine, message);
			network_event("told machine #%d of a player gone while it loaded (machine #%d / controller #%d)",
				machine->machine_index, player->machine_index, player->controller_index);
		}
		/* and those added */
		for (index = 0; index < count; index++)
		{
			struct network_player *player = &server->game.players[index];
			struct network_player message_packet;
			void *message;

			if (!network_player_is_valid(player) || network_game_server_player_among(player, started, count))
				continue;
			message_packet = *player;
			message = create_network_game_message(_message_server_add_player_ingame, &message_packet,
				sizeof(message_packet));
			if (message)
				network_game_server_send_message_to_client_machine(server, machine, message);
			network_event("told machine #%d of a player added while it loaded (machine #%d / controller #%d)",
				machine->machine_index, player->machine_index, player->controller_index);
		}
	}
}

void network_game_server_queue_player_for_addition(
	struct network_game_server *server,
	struct network_player *player)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x5DE, server && player);

	/* port: a player already in the game or queued to join it, asked for
	again: the pregame screen asks every frame until its player is in the
	settings, which a machine joining the game in progress has only once
	started, and that waits for the machine's queued players (the repeats
	among them, which fail to join) */
	{
		long index;

		if (server->queued_player_valid &&
			server->queued_player.machine_index == player->machine_index &&
			server->queued_player.controller_index == player->controller_index)
		{
			return;
		}
		for (index = 0; index < server->waiting_player_count; index++)
		{
			if (server->waiting_players[index].machine_index == player->machine_index &&
				server->waiting_players[index].controller_index == player->controller_index)
			{
				return;
			}
		}
		for (index = 0; index < (long)NUMBEROF(server->game.players); index++)
		{
			if (network_player_is_valid(&server->game.players[index]) &&
				server->game.players[index].machine_index == player->machine_index &&
				server->game.players[index].controller_index == player->controller_index)
			{
				return;
			}
		}
	}
	if (!server->queued_player_valid && network_player_is_valid(player))
	{
		csmemcpy(&server->queued_player, player, sizeof(server->queued_player));
		server->queued_player_valid = TRUE;
	}
	else if (network_player_is_valid(player) &&
		server->waiting_player_count < MAXIMUM_NETWORK_PLAYER_COUNT)
	{
		csmemcpy(&server->waiting_players[server->waiting_player_count++], player, sizeof(struct network_player));
	}

	return;
}

boolean network_game_server_remove_player_from_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	struct network_player *player)
{
	boolean success;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4A0, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4A1, machine);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4A2, player);

	if (machine->machine_index == player->machine_index)
	{
		success = network_game_remove_player(&server->game, player);
		if (success == TRUE)
		{
			network_event(
				"server removed player from machine #%d at controller index #%d from the game",
				player->machine_index,
				player->controller_index);
		}
		else
		{
			network_event(
				"network_game_remove_player() failed in network_game_server_remove_player_from_game()");
		}
	}
	else
	{
		network_event(
			"client machine tried to remove a player with a non-matching machine identifier");
		success = FALSE;
	}

	return success;
}

boolean network_game_server_adjust_machine_settings(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	struct network_machine *machine_description)
{
	boolean success = FALSE;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x4BF,
		server && machine && machine_description);

	if (machine->machine_index == machine_description->machine_index)
	{
		success = network_game_update_machine(&server->game, machine_description);
		if (success == TRUE)
		{
			network_event(
				"server updated machine #%d settings",
				machine_description->machine_index);
		}
		else
		{
			network_event(
				"network_game_update_machine() failed in network_game_server_adjust_machine_settings()");
		}
	}
	else
	{
		network_event(
			"client machine tried to update itself with a non-matching machine identifier");
	}

	return success;
}

void network_game_server_begin_game_start_countdown(
	struct network_game_server *server,
	long time_remaining)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x5ED, server);

	if (!server->countdown_state.active && !server->countdown_state.paused)
	{
		countdown_timer_set_time_remaining(
			&server->countdown_state.timer,
			time_remaining);
		server->countdown_state.adjusted_time_this_tick = FALSE;
		server->countdown_state.active = TRUE;
		network_game_server_countdown_started(server);
		network_event("server game start countdown started");
	}

	return;
}

boolean server_needs_more_teams(
	struct network_game_server *server)
{
	boolean needs_more_teams = FALSE;

	if (server->game.variant.universal_variant.teams)
	{
		short player_count_by_team[NUMBER_OF_MULTIPLAYER_TEAMS] = { 0, 0 };
		long player_index;
		long team_index;

		for (player_index = 0;
			player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
			player_index++)
		{
			struct network_player *player = &server->game.players[player_index];

			if (network_player_is_valid(player) &&
				player->team_index >= 0 &&
				player->team_index < NUMBER_OF_MULTIPLAYER_TEAMS)
			{
				player_count_by_team[player->team_index]++;
			}
		}

		for (team_index = 0;
			team_index < NUMBER_OF_MULTIPLAYER_TEAMS;
			team_index++)
		{
			if (player_count_by_team[team_index] == 0)
			{
				needs_more_teams = TRUE;
				break;
			}
		}
	}

	return needs_more_teams;
}

boolean server_has_a_player_on_each_machine(
	struct network_game_server *server)
{
	long client_machine_index;

	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		/* (the machines in the game: not a connection that has not joined) */
		if (network_game_server_client_machine_is_joined_to_game(server, client_machine))
		{
			boolean has_a_player = FALSE;
			long player_index;

			for (player_index = 0;
				player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
				player_index++)
			{
				if (network_player_is_valid(&server->game.players[player_index]) &&
					server->game.players[player_index].machine_index ==
						client_machine->machine_index)
				{
					has_a_player = TRUE;
				}
			}

			if (!has_a_player)
				return FALSE;
		}
	}

	return TRUE;
}

boolean server_has_enough_machines(
	struct network_game_server *server)
{
	boolean has_enough_machines;
	long minimum_machine_count =
		network_game_is_splitscreen_local() ? 1 : 2;
	long machine_count = 0;
	long client_machine_index;

#ifdef HALO_WEB
	if (network_game_distributed())
		minimum_machine_count = 1;
#endif

	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		if (network_game_server_client_machine_is_joined_to_game(server, client_machine))
		{
			machine_count++;
		}
	}

	has_enough_machines = machine_count >= minimum_machine_count;

	return has_enough_machines;
}

boolean server_ok_to_countdown(
	struct network_game_server *server)
{
	if (server_has_enough_machines(server) &&
		server_has_a_player_on_each_machine(server) &&
		!server_needs_more_teams(server) &&
		server->game.player_count >= server->game.minimum_players)
	{
		return TRUE;
	}

	return FALSE;
}

void network_game_server_invalidate_network_machine(
	struct network_machine *machine)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x6C9, machine);

	csmemset(machine, 0, sizeof(*machine));
	machine->machine_index = NONE;

	return;
}

void network_game_generate_join_game_token(
	byte join_token[NETWORK_JOIN_GAME_TOKEN_SIZE])
{
	byte join_token_initializer[] =
	{
		0x6D, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x20,
		0x69, 0x6E, 0x20, 0x61, 0x20, 0x62, 0x6F, 0x74,
		0x74, 0x6C, 0x65
	};

	match_assert(NETWORK_SERVER_MANAGER_FILE, 1754, join_token);
	/* January and the supplied source both clear the decayed pointer's size. */
	memset(join_token, 0, sizeof(join_token));
	memcpy(join_token, join_token_initializer,
		MIN(NETWORK_JOIN_GAME_TOKEN_SIZE, sizeof(join_token_initializer)));

#ifndef DEBUG
	{
		char *build_timestamp = __DATE__ __TIME__;
		int i, j, length = strlen(build_timestamp);
		unsigned long tags_checksum = tag_groups_checksum();

		/* Release builds also incorporate compilation time and tag checksum. */
		for (i = 0; i < sizeof(join_token); i++)
		{
			for (j = 0; j < length; j++)
			{
				join_token[i] ^= build_timestamp[j];
			}
		}
		for (i = j = 0; i < sizeof(join_token); i++)
		{
			join_token[i] ^= ((byte *)&tags_checksum)[j++];
			if (j == sizeof(tags_checksum))
			{
				j = 0;
			}
		}
	}
#endif

	return;
}

struct network_machine *network_game_server_get_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	long *machine_index)
{
	struct network_machine *machine;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x701, server && client_machine);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x702,
		client_machine->machine_index<MAXIMUM_NETWORK_MACHINE_COUNT);

	if (machine_index)
		*machine_index = NONE;

	machine = &server->game.machines[client_machine->machine_index];
	if (machine_index)
		*machine_index = machine->machine_index;

	return machine;
}

struct network_connection *network_game_server_get_machine_connection(
	struct network_game_server *server,
	struct network_machine *machine)
{
	struct network_connection *connection = NULL;
	long index;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x72F,
		server && network_machine_is_valid(machine));

	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		if (server->client_machines[index].machine_index == machine->machine_index)
		{
			connection = server->client_machines[index].connection;
			break;
		}
	}

	return connection;
}

struct network_game_server_client_machine *network_game_server_get_client_machine_at_index(
	struct network_game_server *server,
	long index)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x741,
		server && (index<MAXIMUM_NETWORK_MACHINE_COUNT));

	return &server->client_machines[index];
}

struct network_game_server_client_machine *network_game_server_get_client_machine_at_address(
	struct network_game_server *server,
	unsigned long ip_address)
{
	struct network_game_server_client_machine *client_machine = NULL;
	long i;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x74D, server && ip_address);

	/* port: from the addresses the machines joined from (every datagram
	asked each machine's connection for its address) */
	for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
	{
		/* (a machine in the game: not a connection that has not joined) */
		if (network_game_server_client_machine_addresses[i] == ip_address &&
			network_game_server_client_machine_is_joined_to_game(server, &server->client_machines[i]))
		{
			match_assert(NETWORK_SERVER_MANAGER_FILE, 0x755,
				server->client_machines[i].connection);

			client_machine = &server->client_machines[i];
			break;
		}
	}

	/* (none: a datagram from a machine not in the game, which is dropped
	without a line in the log for each) */
	return client_machine;
}

boolean network_game_server_game_can_start(
	struct network_game_server *server)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x782, server);

	return server->state == 0 &&
		server->game.player_count >= server->game.minimum_players;
}

void network_game_server_pause_countdown(
	struct network_game_server *server,
	boolean pause_countdown)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x78C, server);

	if (pause_countdown == TRUE)
		csmemset(&server->countdown_state, 0, sizeof(server->countdown_state));

	server->countdown_state.paused = pause_countdown;

	return;
}

void network_game_server_change_map_name(
	struct network_game_server *server,
	char const *map_name)
{
	long client_machine_index;

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0x79B,
		server && map_name && map_name[0]);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x79C,
		server->state == _network_game_server_state_pregame);

	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		struct network_game_server_client_machine *client_machine =
			&server->client_machines[client_machine_index];

		if (client_machine->machine_index >= 0 &&
			client_machine->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)
		{
			SET_FLAG(
				client_machine->flags,
				_network_client_machine_precached_bit,
				FALSE);
		}
	}

	csstrncpy(
		server->game.map.name,
		map_name,
		NETWORK_GAME_MAP_NAME_LENGTH - 1);
	server->game.map.name[NETWORK_GAME_MAP_NAME_LENGTH - 1] = 0;

	if (!network_game_server_send_game_data_pregame(server))
	{
		network_event(
			"network_game_server_change_map_name() failed to send updated game settings to clients");
	}

	return;
}

void network_game_server_change_game_variant(
	struct network_game_server *server,
	struct game_variant *variant)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x7BE, server && variant);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x7BF,
		server->state == _network_game_server_state_pregame);

	csmemcpy(&server->game.variant, variant, sizeof(server->game.variant));

	if (!network_game_server_send_game_data_pregame(server))
	{
		network_event(
			"network_game_server_change_game_variant() failed to send updated game settings to clients");
	}

	return;
}

#ifdef HALO_WEB
boolean network_game_server_enable_quick_play(struct network_game_server *server)
{
	if (!server || server->state != _network_game_server_state_pregame || !network_game_distributed() ||
		server->game.variant.game_engine_index != game_engine_slayer || server->game.variant.universal_variant.teams)
		return FALSE;
	server->game.minimum_players = 1;
	return network_game_server_send_game_data_pregame(server);
}
#endif

boolean network_game_server_remove_client_machine_from_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *client)
{
	boolean success = FALSE;
	int i;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x22F, server);
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x230, client);

#ifdef HALO_WEB
	if ((server_migration.epoch || (server->state == _network_game_server_state_ingame && web_match_migration_enabled())) &&
		!TEST_FLAG(client->flags, _network_client_machine_validated_bit))
	{
		if (client->connection)
			network_server_close_client_connection(server->connection, client->connection);
		csmemset(client, 0, sizeof(*client));
		client->machine_index = NONE;
		return TRUE;
	}
#endif

	if (_network_game_server_state_ingame == server->state)
	{
		network_game_server_send_player_quit_messages_ingame(server, client);
	}

	for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
	{
		/* (not a slot no machine holds, for a client already removed) */
		if (client->machine_index != NONE &&
			server->game.machines[i].machine_index == client->machine_index)
		{
			if (!network_game_remove_machine(
				&server->game,
				&server->game.machines[i]))
			{
				error(
					_error_silent,
					"network_game_server_remove_client_machine_from_game() failed to remove the offending machine from the server's copy of the game");
			}
			break;
		}
	}

	for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
	{
		if (&server->client_machines[i] == client)
		{
			if (server->client_machines[i].connection != NULL)
			{
				if (!network_server_close_client_connection(
					server->connection,
					server->client_machines[i].connection))
				{
					network_event("server failed to close a client's connection");
				}
			}

			{
				/* players this machine queued to join in game go with it: a
				machine that joins later may get its index */
				long waiting_index = 0;

				if (server->queued_player_valid &&
					server->queued_player.machine_index == client->machine_index)
				{
					server->queued_player_valid = FALSE;
				}
				while (waiting_index < server->waiting_player_count)
				{
					if (server->waiting_players[waiting_index].machine_index == client->machine_index)
					{
						server->waiting_player_count--;
						csmemmove(
							&server->waiting_players[waiting_index],
							&server->waiting_players[waiting_index + 1],
							(server->waiting_player_count - waiting_index) * sizeof(struct network_player));
					}
					else
					{
						waiting_index++;
					}
				}
			}
			server->client_machines[i].connection = NULL;
			server->client_machines[i].last_received_update_sequence_number = 0;
			server->client_machines[i].last_heard_time = 0;
			server->client_machines[i].machine_index = NONE;
			server->client_machines[i].flags = 0;
			network_game_server_client_machine_addresses[i] = 0;
			success = TRUE;
			break;
		}
	}

	if (!success)
	{
		network_event(
			"network_game_server_remove_client_machine_from_game() failed to find the specified machine");
	}
	else
	{
		/* the machine may have been the last the others waited for */
		network_game_server_check_loading_complete(server);
	}

	return success;
}

/* a client machine gone: out of the game if it joined it, else (a
connection that never joined, whose machine the game does not have) only
its connection */
static boolean network_game_server_drop_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *client)
{
	if (network_game_server_client_machine_is_joined_to_game(server, client) &&
		network_game_server_remove_machine_from_game(server, &server->game.machines[client->machine_index]))
	{
		return TRUE;
	}
	if (client->machine_index == NONE)
	{
		return TRUE;
	}
	return network_game_server_remove_client_machine_from_game(server, client);
}

void network_game_server_client_machine_heard(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	(void)server;
	machine->last_heard_time = system_milliseconds();
}

boolean network_game_server_client_machine_is_local(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	struct network_game_client *client = global_network_game_client_get();

	return client &&
		network_game_server_client_machine_is_joined_to_game(server, machine) &&
		network_game_client_get_machine_index(client) == machine->machine_index;
}

/* port: a connection that has not joined in time, or a machine in the game
gone silent (the lockstep netcode's stall timed it out; the distributed
netcode waits for no machine) */
static boolean network_game_server_client_machine_timed_out(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	unsigned long silence = system_milliseconds() - machine->last_heard_time;

	if (!network_game_server_client_machine_is_joined_to_game(server, machine))
		return silence > NETWORK_GAME_SERVER_JOIN_TIMEOUT;
	if (server->state != _network_game_server_state_ingame ||
		network_game_server_client_machine_is_local(server, machine))
	{
		return FALSE;
	}
	if (TEST_FLAG(machine->flags, _network_client_machine_level_loaded_bit))
		return silence > NETWORK_GAME_SERVER_CLIENT_TIMEOUT;
	/* (joining the game in progress: waiting for its players to be added,
	or loading) */
	return silence > NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT;
}

boolean network_game_server_remove_machine_from_game(
	struct network_game_server *server,
	struct network_machine *machine)
{
	boolean success = FALSE;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x299, server);

#ifdef HALO_WEB
	if (server_migration.epoch || (server->state == _network_game_server_state_ingame && web_match_migration_enabled()))
	{
		int pending;
		for (pending = 0; pending < MAXIMUM_NETWORK_MACHINE_COUNT; pending++)
		{
			struct network_game_server_client_machine *candidate = &server->client_machines[pending];
			if (candidate->machine_index == machine->machine_index && candidate->connection &&
				!TEST_FLAG(candidate->flags, _network_client_machine_validated_bit))
				return network_game_server_remove_client_machine_from_game(server, candidate);
		}
	}
#endif

	if (NONE == machine->machine_index)
	{
		network_event(
			"network_game_server_remove_machine_from_game called with a machine_index of NONE");
	}

	if (network_machine_is_valid(machine))
	{
		int i;

		for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
		{
			if (server->client_machines[i].machine_index == machine->machine_index)
			{
				success = network_game_server_remove_client_machine_from_game(
					server,
					&server->client_machines[i]);
				if (!success)
				{
					network_event(
						"network_game_server_remove_client_machine_from_game() failed in network_game_server_remove_machine_from_game()");
				}
				break;
			}
		}

		if (i == MAXIMUM_NETWORK_MACHINE_COUNT)
		{
			network_event(
				"network_game_server_remove_machine_from_game() failed to find the specified machine");
		}

		if (machine->machine_index != NONE)
		{
			success = network_game_remove_machine(&server->game, machine);
			if (!success)
			{
				network_event(
					"network_game_remove_machine() failed in network_game_server_remove_machine_from_game()");
			}
		}

		/* (the lobby's: not to machines loading the game, which have the
		settings it started with, and are told of a player gone once it has
		loaded, network_game_server_remove_players_gone_while_loading) */
		if (network_game_server_lobby_is_open(server))
		{
			boolean sent_game_settings =
				network_game_server_send_game_data_pregame(server);
			if (!sent_game_settings)
			{
				network_event(
					"network_game_server_remove_machine_from_game() failed to send updated game settings to remaining clients");
			}
		}
	}
	else
	{
		network_event(
			"attempted to remove an invalid machine from the game in network_game_server_remove_machine_from_game()");
		network_event("machine name = <not implemented>");
		network_event("machine index = %x", machine->machine_index);
		network_game_server_dump(server);
	}

	return success;
}

static void dump_network_game_data(
	char *prefix,
	struct network_game *network_game_data)
{
#ifdef DEBUG
	network_event("%snetwork_game_data", prefix);
	network_event("%smachine_count %d", prefix, network_game_data->machine_count);
	{
		long itr;
		for (itr = 0; itr < MAXIMUM_NETWORK_MACHINE_COUNT; itr++)
		{
			/* the native builds log only the slots in use, a line each: each
			line reopens the log, and 128 empty slots took seconds */
			if (network_game_data->machines[itr].machine_index == NONE)
			{
				continue;
			}
			network_event(
				"\t%smachine %d %x",
				prefix,
				itr,
				network_game_data->machines[itr].machine_index);
		}
	}

	network_event("%splayer_count %d", prefix, network_game_data->player_count);
	{
		long itr;
		for (itr = 0; itr < MAXIMUM_NETWORK_PLAYER_COUNT; itr++)
		{
			if (network_game_data->players[itr].machine_index == NONE)
			{
				continue;
			}
			network_event("%splayer %d: machine_index %x controller_index %x team_index %x player_list_index %x",
				prefix,
				itr,
				network_game_data->players[itr].machine_index,
				network_game_data->players[itr].controller_index,
				network_game_data->players[itr].team_index,
				network_game_data->players[itr].player_list_index);
		}
	}

	network_event("%snetwork_game_random_seed %x", prefix,
		network_game_data->random_seed);
	network_event("%snumber_of_games_played %d", prefix,
		network_game_data->number_of_games_played);
#endif

	return;
}

static void network_game_server_dump(
	struct network_game_server *server)
{
#ifdef DEBUG
	long itr;

	network_event("*************BEGIN*************");
	network_event("\tconnection %x", server->connection);
	network_event("\tstate %x", server->state);
	network_event("\tflags %x", server->flags);
	dump_network_game_data("\t", &server->game);

	network_event("client_machines:");
	for (itr = 0; itr < MAXIMUM_NETWORK_MACHINE_COUNT; itr++)
	{
		struct network_game_server_client_machine *client_machine =
			server->client_machines + itr;
		char *connection_status = "no connection";

		if (client_machine->connection != NULL)
		{
			connection_status = network_connection_active(client_machine->connection)
				? "(active)" : "(dead)";
		}

		if (client_machine->connection == NULL && client_machine->machine_index == NONE)
		{
			continue;
		}
		network_event("\tclient %d: connection %x %s last_received_update_sequence_number %d last_heard_time %d machine_index %x flags %x",
			itr,
			client_machine->connection,
			connection_status,
			client_machine->last_received_update_sequence_number,
			client_machine->last_heard_time,
			client_machine->machine_index,
			client_machine->flags);
	}

	network_event("\tnext_update_number %d", server->next_update_number);
	network_event("\ttime_of_last_keep_alive %d", server->time_of_last_keep_alive);
	network_event("\ttime_of_first_client_loading_completion %d",
		server->time_of_first_client_loading_completion);
	network_event("*************END*************");
#endif

	return;
}

void network_game_server_update_countdown(
	struct network_game_server *server,
	short countdown_event)
{
	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x66E,
		server && server->state == _network_game_server_state_pregame);

	if (server->countdown_state.paused == FALSE)
	{
		boolean ok_to_countdown = server_ok_to_countdown(server);

		if (ok_to_countdown ||
			countdown_event == _network_game_server_countdown_event_stop)
		{
			if (server->countdown_state.active == TRUE)
			{
				if (!server->countdown_state.adjusted_time_this_tick)
				{
					switch (countdown_event)
					{
						case _network_game_server_countdown_event_player_left:
							server->countdown_state.adjusted_time_this_tick = TRUE;
							countdown_timer_increment(
								&server->countdown_state.timer,
								NETWORK_GAME_COUNTDOWN_ADJUSTMENT,
								NETWORK_GAME_COUNTDOWN_TIME);
							break;

						case _network_game_server_countdown_event_player_joined:
							server->countdown_state.adjusted_time_this_tick = TRUE;
							if (countdown_timer_get_time_remaining(&server->countdown_state.timer) >
								NETWORK_GAME_MINIMUM_COUNTDOWN_TIME)
							{
								countdown_timer_decrement(
									&server->countdown_state.timer,
									NETWORK_GAME_COUNTDOWN_ADJUSTMENT);
								if (countdown_timer_get_time_remaining(&server->countdown_state.timer) <
									NETWORK_GAME_MINIMUM_COUNTDOWN_TIME)
								{
									countdown_timer_set_time_remaining(
										&server->countdown_state.timer,
										NETWORK_GAME_MINIMUM_COUNTDOWN_TIME);
								}
							}
							break;

						case _network_game_server_countdown_event_stop:
							server->countdown_state.active = FALSE;
							server->countdown_state.adjusted_time_this_tick = TRUE;
							break;

						case _network_game_server_countdown_event_start_immediately:
							server->countdown_state.adjusted_time_this_tick = TRUE;
							countdown_timer_set_time_remaining(&server->countdown_state.timer, 0);
							break;
					}
				}
			}
			else
			{
				unsigned long now = system_milliseconds();

				if (countdown_event == _network_game_server_countdown_event_start_immediately)
				{
					countdown_timer_set_time_remaining(&server->countdown_state.timer, 0);
					server->countdown_state.active = TRUE;
					server->countdown_state.adjusted_time_this_tick = FALSE;
					network_game_server_countdown_started(server);
				}
				else
				{
					if (network_game_should_accept_remote_connections() == FALSE ||
#ifdef HALO_WEB
						(network_game_distributed() && server_has_enough_machines(server)) ||
#endif
						network_game_server_get_client_machine_count(server) > 1)
					{
						unsigned long countdown;

						if (network_game_is_splitscreen_local())
							countdown = NETWORK_GAME_SPLITSCREEN_COUNTDOWN_TIME;
						else
							countdown = NETWORK_GAME_COUNTDOWN_TIME;

						server->countdown_state.active = TRUE;
						countdown_timer_set_time_remaining(
							&server->countdown_state.timer,
							countdown);
						server->countdown_state.adjusted_time_this_tick = FALSE;
						server->countdown_state.last_countdown_message_time = 0;
						network_game_server_countdown_started(server);
					}
				}
			}
		}
	}

	return;
}

/* port: the lobby takes changes (players, machines' and players' settings):
not once the game has started, which every machine loads from the settings
sent with its start */
boolean network_game_server_lobby_is_open(
	struct network_game_server *server)
{
	return server->state == _network_game_server_state_pregame && !server->sent_start_game_message;
}

/* port: a client machine's slower (the pregame screen's, adding time to the
countdown): once each countdown, and not in its last ten seconds (the Xbox
game took any number from any machine, which could hold the lobby for ever);
the host's own as often as it likes */
boolean network_game_server_client_machine_may_slow_countdown(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine)
{
	if (!server->countdown_state.active ||
		network_game_server_client_machine_is_local(server, machine))
	{
		return TRUE;
	}
	if (!VALID_INDEX(machine->machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) ||
		network_game_server_countdown_slowed[machine->machine_index] ||
		countdown_timer_get_time_remaining(&server->countdown_state.timer) < 10 * MILLISECONDS_PER_SECOND)
	{
		return FALSE;
	}
	network_game_server_countdown_slowed[machine->machine_index] = TRUE;
	return TRUE;
}

/* ---------- private code */

static void network_game_server_countdown_started(
	struct network_game_server *server)
{
	(void)server;
	csmemset(network_game_server_countdown_slowed, 0, sizeof(network_game_server_countdown_slowed));
}

void get_unique_random_name(
	struct network_game_server *server,
	struct network_player *player)
{
	wchar_t const *name;
	long duplicate_count;
	long attempt_count = 0;

	do
	{
		long player_index;

		name = network_game_get_random_player_name();
		duplicate_count = 0;

		for (player_index = 0;
			player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
			player_index++)
		{
			struct network_player *existing_player =
				&server->game.players[player_index];

			if (network_player_is_valid(existing_player) &&
				!ustrcmp(existing_player->name, name))
			{
				duplicate_count++;
			}
		}
	}
	while (duplicate_count != 0 && ++attempt_count < MAXIMUM_UNIQUE_NAME_ATTEMPTS);

	ustrncpy(player->name, name, NETWORK_PLAYER_NAME_LENGTH - 1);
	player->name[NETWORK_PLAYER_NAME_LENGTH - 1] = 0;

	/* every random name is taken: number this one ("Name2", "Name3", ...)
	until it is unique */
	if (duplicate_count != 0)
	{
		wchar_t base_name[NETWORK_PLAYER_NAME_LENGTH];
		long number;

		csmemcpy(base_name, player->name, sizeof(base_name));
		for (number = 2; number < 1000 && !player_name_is_unique(server, player->name); number++)
		{
			wchar_t digits[4];
			long digit_count = 0;
			long base_length = (long)ustrlen(base_name);
			long value;

			for (value = number; value; value /= 10)
			{
				digits[digit_count++] = (wchar_t)(L'0' + value % 10);
			}
			base_length = MIN(base_length, NETWORK_PLAYER_NAME_LENGTH - 1 - digit_count);
			csmemcpy(player->name, base_name, base_length * sizeof(wchar_t));
			while (digit_count > 0)
			{
				player->name[base_length++] = digits[--digit_count];
			}
			player->name[base_length] = 0;
		}
	}

	return;
}

void get_unique_random_color(
	struct network_game_server *server,
	struct network_player *player)
{
	long attempt_count = 0;
	long color_index;
	boolean unique;

	do
	{
		long player_index;

		color_index = attempt_count < MAXIMUM_GOOD_COLOR_ATTEMPTS
			? player_profile_get_random_good_color()
			: player_profile_get_random_color();

		unique = TRUE;

		for (player_index = 0;
			player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
			player_index++)
		{
			if (network_player_is_valid(&server->game.players[player_index]) &&
				server->game.players[player_index].primary_color_index == color_index)
			{
				unique = FALSE;
				break;
			}
		}

		attempt_count++;
	}
	while (!unique && attempt_count < MAXIMUM_UNIQUE_COLOR_ATTEMPTS);

	player->primary_color_index = (short)color_index;

	return;
}

static boolean player_name_is_unique(
	struct network_game_server *server,
	wchar_t const *name)
{
	long player_index;

	for (player_index = 0;
		player_index < MAXIMUM_NETWORK_PLAYER_COUNT;
		player_index++)
	{
		struct network_player *player = &server->game.players[player_index];

		if (network_player_is_valid(player) && !ustrcmp(player->name, name))
			return FALSE;
	}

	return TRUE;
}

static short network_game_server_get_client_machine_count(
	struct network_game_server *server)
{
	short client_machine_count = 0;
	short client_machine_index;

	for (client_machine_index = 0;
		client_machine_index < MAXIMUM_NETWORK_MACHINE_COUNT;
		client_machine_index++)
	{
		if (server->client_machines[client_machine_index].connection &&
			network_game_server_client_machine_is_joined_to_game(server,
				&server->client_machines[client_machine_index]))
		{
			client_machine_count++;
		}
	}

	return client_machine_count;
}

static boolean network_game_server_setup_game_from_playlist(
	struct network_game_server *server)
{
	boolean success = FALSE;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x961, server);

	network_event("setting up a net game");
	if (game_engine_get_current_stage(&server->game.variant, server->game.map.name))
	{
		wchar_t machine_name[MAXIMUM_MACHINE_NAME_LENGTH] = L"<unknown>";

		network_game_generate_local_machine_name(machine_name);
		ustrncpy(server->game.name, machine_name, NETWORK_GAME_NAME_LENGTH - 1);
		server->game.name[NETWORK_GAME_NAME_LENGTH - 1] = L'\0';
		server->game.map.version = 0;
		server->game.minimum_players = 2;
#ifdef HALO_WEB
		/* A browser host can play while waiting for peers. Distributed
		netcode admits players in progress; lockstep still needs two. The
		countdown retains its team and per-machine readiness checks. */
		if (network_game_distributed())
			server->game.minimum_players = 1;
#endif
		server->game.maximum_players = MAXIMUM_NETWORK_PLAYER_COUNT;

		if (server->game.variant.universal_variant.teams)
		{
			server->game.maximum_teams = 2;
		}
		else
		{
			server->game.maximum_teams = 1;
		}

		network_game_server_open_game(server);
		success = TRUE;
	}
	else
	{
		error(
			_error_silent,
			"network game setup failed; probably due to a missing playlist");
	}

	return success;
}

static boolean network_game_server_add_new_client(
	struct network_game_server *server,
	struct network_connection *new_connection)
{
	boolean success = FALSE;
	int i;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x7D4, server && new_connection);

	/* (open, if not to joins now: a machine that joins while the others
	load the game is told so, network_game_server_handle_message_client_join_game_request) */
	if (TEST_FLAG(server->flags, _network_game_server_game_open_bit))
	{
		for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
		{
			if (server->client_machines[i].machine_index == NONE)
			{
				struct transport_address client_address = { 0 };

				network_connection_get_address(
					new_connection,
					&client_address,
					NULL);
				if (client_address.address.ipv4_address)
				{
					/* port: the connections of an address that have not joined
					yet (each holds a machine's slot until the join timeout):
					a few, so that one machine does not take them all */
					long waiting_count = 0;
					long other_index;

					for (other_index = 0;
						other_index < MAXIMUM_NETWORK_MACHINE_COUNT &&
							client_address.address.ipv4_address != IPV4_LOOPBACK_ADDRESS;
						other_index++)
					{
						struct network_game_server_client_machine *other = &server->client_machines[other_index];
						struct transport_address other_address = { 0 };

						if (!other->connection || network_game_server_client_machine_is_joined_to_game(server, other))
							continue;
						network_connection_get_address(other->connection, &other_address, NULL);
						if (other_address.address.ipv4_address == client_address.address.ipv4_address)
							waiting_count++;
					}
					if (!network_game_should_accept_remote_connections() &&
						client_address.address.ipv4_address != IPV4_LOOPBACK_ADDRESS)
					{
						network_event(
							"remote system tried to join our server but we are not accepting remote connections: address= '%s'",
							transport_address_to_string(&client_address));
					}
					else if (waiting_count >= MAXIMUM_WAITING_CONNECTIONS_PER_ADDRESS)
					{
						network_event(
							"refusing another connection from %s, which has not joined with those it has",
							transport_address_to_string(&client_address));
					}
					else
					{
						server->client_machines[i].connection = new_connection;
#ifdef HALO_WEB
						/* Reattach transports reserve no new roster/player slot. */
						if (!server_migration.epoch && !(server->state == _network_game_server_state_ingame && web_match_migration_enabled()))
#endif
						network_game_invalidate_machine(&server->game, i);
						server->client_machines[i].machine_index = (short)i;
						server->client_machines[i].flags =
							FLAG(_network_client_machine_connected_bit);
						/* (it has until the join timeout to join) */
						server->client_machines[i].last_heard_time = system_milliseconds();
						success = network_connection_server_accept_client_connection(
							server->connection,
							new_connection);
						if (success == TRUE)
						{
							network_event(
								"new remote connection accepted from %s",
								transport_address_to_string(&client_address));
						}
						else
						{
							/* port: the caller closes the connection: the slot
							must not keep it */
							server->client_machines[i].connection = NULL;
							server->client_machines[i].last_heard_time = 0;
							server->client_machines[i].machine_index = NONE;
							server->client_machines[i].flags = 0;
						}
					}
				}
				else
				{
					network_event(
						"network_connection_get_address() failed to get a valid address in network_game_server_add_new_client()");
				}

				break;
			}
		}

		if (i == MAXIMUM_NETWORK_MACHINE_COUNT)
		{
			network_event(
				"failed to find an available machine slot in network_game_server_add_new_client()");
		}
	}
	else
	{
		network_event(
			"network_game_server_add_new_client() failed because the game is closed");
	}

	return success;
}

static boolean network_game_server_handle_public_endpoint(
	struct network_game_server *server)
{
	boolean success = TRUE;
	word datagram_buffer[DATAGRAM_MAXIMUM_SIZE / sizeof(word)];
	word *message = datagram_buffer;
	word datagram_size = sizeof(datagram_buffer);
	struct transport_address source_address;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x810, server);

	while (success && network_connection_read(
		server->connection,
		message,
		&datagram_size,
		&source_address))
	{
		if ((success = network_game_server_handle_datagram(
			server,
			message,
			datagram_size,
			&source_address)) == FALSE)
		{
			network_event(
				"network_game_server_handle_datagram() failed in network_game_server_handle_public_endpoint()");
		}

		datagram_size = sizeof(datagram_buffer);
	}

	return success;
}

static boolean network_game_server_remove_disconnected_client(
	struct network_game_server *server,
	struct network_game_server_client_machine *client)
{
#ifdef HALO_WEB
	/* A quick-play transport can disappear before the first migration epoch
	   reaches the native host. Retain its roster and owner for reattachment. */
	if (server_migration.epoch || (server->state == _network_game_server_state_ingame && web_match_migration_enabled()))
	{
		if (TEST_FLAG(client->flags, _network_client_machine_validated_bit))
		{
			short machine_index = client->machine_index;
			network_game_server_migration_detach(server, client);
			network_game_server_client_machine_addresses[machine_index] = 0;
			return TRUE;
		}
	}
#endif
	return network_game_server_drop_client_machine(server, client);
}

static boolean network_game_server_handle_client_machines(
	struct network_game_server *server)
{
	boolean success = TRUE;
	int i;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x827, server);

	for (i = 0; success && i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
	{
		struct network_game_server_client_machine *client_machine = &server->client_machines[i];

		if (client_machine->machine_index == NONE)
			continue;

		if (!network_connection_active(client_machine->connection))
		{
			short machine_index = client_machine->machine_index;

			/* (a connection that never joined goes too: it held its slot, and
			the lobby's start, for ever) */
			if (network_game_server_remove_disconnected_client(server, client_machine))
			{
				network_event("client machine %x removed from game", machine_index);
				/* the native builds skip the dump here: it lists every machine
				and player, and when many machines leave at once the dumps
				keep the host writing its log for minutes */
			}
			else
			{
				network_event("failed to remove client machine %x from game", machine_index);
				network_game_server_dump(server);
			}
		}
		else if (network_connection_idle(
			client_machine->connection,
			_connection_dont_timeout,
			NULL) &&
			network_connection_connected(client_machine->connection))
		{
			word message_buffer[MAXIMUM_NETWORK_MESSAGE_SIZE / sizeof(word)];
			word *message = message_buffer;
			word message_buffer_size = sizeof(message_buffer);

			while (success && network_connection_read(
				client_machine->connection,
				message,
				&message_buffer_size,
				NULL))
			{
				if (network_game_server_handle_client_message(
					server,
					client_machine,
					message,
					message_buffer_size))
				{
					/* the message may have taken the machine out of the game
					(its graceful exit): its connection is gone */
					if (client_machine->machine_index == NONE || client_machine->connection == NULL)
						break;
					/* (a connection that has not joined is timed from when it
					connected: what it sends before its join does not count) */
					if (network_game_server_client_machine_is_joined_to_game(server, client_machine))
						network_game_server_client_machine_heard(server, client_machine);
					message_buffer_size = sizeof(message_buffer);
				}
				else
				{
					short machine_index = client_machine->machine_index;

					network_event(
						"network_game_server_handle_client_message() failed in network_game_server_handle_client_machines()");
					if (client_machine->machine_index != NONE &&
						!network_game_server_drop_client_machine(server, client_machine))
					{
						network_event("failed to remove client machine %x from game", machine_index);
					}
					else
					{
						network_event("client machine %x removed from game", machine_index);
					}
					break;
				}
			}

			if (client_machine->machine_index != NONE &&
				network_game_server_client_machine_timed_out(server, client_machine))
			{
				short machine_index = client_machine->machine_index;

				network_event("client machine %x timed out", machine_index);
				if (!network_game_server_remove_disconnected_client(server, client_machine))
					network_event("failed to remove client machine %x from game", machine_index);
			}
			/* port: one the distributed netcode found cheating: told, and
			dropped, its address kept out */
			else if (VALID_INDEX(client_machine->machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) &&
				network_game_server_kick_pending[client_machine->machine_index])
			{
				short machine_index = client_machine->machine_index;
				struct message_server_machine_rejected rejection = { _rejection_code_blacklisted_machine };
				struct network_message *message;
				unsigned long address = network_game_server_client_machine_addresses[machine_index];

				network_game_server_kick_pending[machine_index] = FALSE;
				if (address && !network_game_server_client_machine_is_local(server, client_machine))
				{
					network_game_server_kicked_addresses[network_game_server_kicked_address_next++ %
						MAXIMUM_KICKED_ADDRESSES] = address;
				}
				message = create_network_game_message(_message_server_machine_rejected, &rejection, sizeof(rejection));
				if (message)
					network_game_server_send_message_to_client_machine(server, client_machine, message);
				if (!network_game_server_drop_client_machine(server, client_machine))
					network_event("failed to remove client machine %x from game", machine_index);
			}
		}
		else
		{
			short machine_index = client_machine->machine_index;

			if (network_game_server_drop_client_machine(server, client_machine))
				network_event("client machine %x removed from game", machine_index);
			else
				network_event("failed to remove client machine %x from game", machine_index);
		}
	}

	return success;
}

static void network_game_server_send_rejection_message(
	struct transport_endpoint *endpoint,
	word reason)
{
	struct message_server_machine_rejected farewell_message = { reason };
	message_header *message;

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0x878,
		endpoint && (reason < NUMBER_OF_SERVER_REJECTION_CODES));

	message = create_network_game_message(
		_message_server_machine_rejected,
		&farewell_message,
		sizeof(farewell_message));
	if (message != NULL)
	{
		int length = GET_MESSAGE_SIZE(*message);
		int bytes_written;

		byte_swap_message_header(message, _byte_order_network);
		bytes_written = write_endpoint(endpoint, message, length);
		if (bytes_written != length)
		{
			network_event(
				"error sending rejection message to client; transport error= '%s'",
				transport_error_to_string(bytes_written));
		}
	}
	else
	{
		network_event(
			"failed to create a message_server_machine_rejected message in network_game_server_send_rejection_message");
	}

	return;
}

static void network_game_server_reject_connection_game_is_full(
	struct transport_endpoint *endpoint)
{
	network_event("client connection refused; game is full");
	network_game_server_send_rejection_message(
		endpoint,
		_rejection_code_game_is_full);

	return;
}

static boolean network_game_server_idle_postgame_tasks(
	struct network_game_server *server)
{
	unsigned long now = system_milliseconds();
	boolean success = TRUE;

	/* (the time since, which the milliseconds' wrap leaves right) */
	if (now - (unsigned long)server->time_of_last_keep_alive > 5UL * MILLISECONDS_PER_SECOND)
	{
		struct message_server_postgame_keep_alive message_packet = { 0 };
		struct network_message *message;

		message = create_network_game_message(
			_message_server_postgame_keep_alive,
			&message_packet,
			sizeof(message_packet));
		network_game_server_send_message_to_all_machines(server, message);

		server->time_of_last_keep_alive = (long)now;
	}

	return success;
}

static boolean network_game_server_have_all_machines_have_precached(
	struct network_game_server *server)
{
	long i;
	boolean all_machines_have_precached = TRUE;

	for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
	{
		struct network_game_server_client_machine *machine =
			server->client_machines + i;

		if (network_game_server_client_machine_is_joined_to_game(server, machine))
		{
			boolean client_has_precached = TEST_FLAG(
				server->client_machines[i].flags,
				_network_client_machine_precached_bit);

			if (!client_has_precached)
			{
				all_machines_have_precached = FALSE;
				break;
			}
		}
	}

	match_assert(
		NETWORK_SERVER_MANAGER_FILE,
		0x8C8,
		!all_machines_have_precached ||
			cache_files_precache_map_loaded(main_get_multiplayer_map_name()));

	return all_machines_have_precached;
}

static boolean network_game_server_idle_pregame_tasks(
	struct network_game_server *server)
{
	long now = (long)system_milliseconds();
	boolean success = TRUE;

	if (server->sent_start_game_message == FALSE)
	{
		long itr;

		/* send the lobby changes collected since the last settings update */
		network_game_server_flush_game_data_pregame(server);

		for (itr = 0; itr < MAXIMUM_NETWORK_MACHINE_COUNT; itr++)
		{
			struct network_game_server_client_machine *client_machine =
				server->client_machines + itr;

			if (client_machine->connection != NULL &&
				!network_connection_active(client_machine->connection))
			{
				network_event("booting dead client machine %d", itr);
				network_game_server_remove_client_machine_from_game(
					server,
					client_machine);
			}
			/* port: a machine that joined and can add no player (the lobby
			filled after it joined), or adds none, holds the countdown for
			ever: refused */
			else if (network_game_server_client_machine_is_joined_to_game(server, client_machine) &&
				!network_game_server_client_machine_is_local(server, client_machine) &&
				!network_game_server_machine_has_players(server, client_machine->machine_index) &&
				(!network_game_has_free_player_slot(&server->game) ||
					system_milliseconds() - network_game_server_client_machine_join_times[client_machine->machine_index] >
						NETWORK_GAME_SERVER_PLAYERLESS_MACHINE_TIMEOUT))
			{
				network_game_server_refuse_late_joiner(server, client_machine,
					network_game_has_free_player_slot(&server->game) ? _rejection_code_game_is_closed :
						_rejection_code_game_is_full);
			}
		}

		if (server->countdown_state.active == TRUE)
		{
			boolean send_countdown_update = FALSE;
			boolean ok_to_countdown = server_ok_to_countdown(server);

			if (!ok_to_countdown)
			{
				csmemset(
					&server->countdown_state,
					0,
					sizeof(server->countdown_state));
				send_countdown_update = TRUE;
			}
			else if (countdown_timer_get_time_remaining(
				&server->countdown_state.timer) == 0 &&
				network_game_server_have_all_machines_have_precached(server) &&
				server->countdown_state.paused == FALSE)
			{
				/* (the game stays open: a machine may join it in progress,
				network_game_server_start_late_joiner) */
				if ((success = network_game_server_start_network_game(server)) != TRUE)
					network_event("network_game_server_start_network_game() failed");
			}
			else if ((long)((unsigned long)now -
				(unsigned long)server->countdown_state.last_countdown_message_time) >
				MILLISECONDS_PER_SECOND)
			{
				send_countdown_update = TRUE;
			}

			if (send_countdown_update == TRUE)
			{
				struct message_server_pregame_countdown message_packet;
				struct network_message *message;

				server->countdown_state.adjusted_time_this_tick = FALSE;

				if (ok_to_countdown)
				{
					long time_remaining = countdown_timer_get_time_remaining(
						&server->countdown_state.timer);

					message_packet.seconds_to_start =
						(short)(time_remaining / MILLISECONDS_PER_SECOND);
				}
				else
				{
					message_packet.seconds_to_start = NONE;
				}

				message = create_network_game_message(
					_message_server_pregame_countdown,
					&message_packet,
					sizeof(message_packet));
				if (message != NULL)
				{
					if (network_game_server_send_message_to_all_machines(server, message))
					{
						server->countdown_state.last_countdown_message_time = now;
					}
					else
					{
						network_event(
							"failed to send a message_server_pregame_countdown to all clients");
					}
				}
			}
		}
		else
		{
			if ((unsigned long)now - (unsigned long)server->time_of_last_keep_alive >
				5UL * MILLISECONDS_PER_SECOND)
			{
				struct message_server_pregame_keep_alive message_packet = { 0 };
				struct network_message *message;

				message = create_network_game_message(
					_message_server_pregame_keep_alive,
					&message_packet,
					sizeof(message_packet));
				network_game_server_send_message_to_all_machines(server, message);

				server->time_of_last_keep_alive = now;
			}
		}
	}
	else if (server->time_of_first_client_loading_completion)
	{
		/* machines that have loaded wait for the others in silence, and a
		client drops a connection it hears nothing on for 15 seconds, less than
		the wait for the others: keep their connections alive (a client in game
		ignores a pregame keep-alive) */
		if ((unsigned long)now - (unsigned long)server->time_of_last_keep_alive >
			5UL * MILLISECONDS_PER_SECOND)
		{
			struct message_server_pregame_keep_alive message_packet = { 0 };
			struct network_message *message;

			message = create_network_game_message(
				_message_server_pregame_keep_alive,
				&message_packet,
				sizeof(message_packet));
			if (message)
			{
				network_game_server_send_message_to_all_machines(server, message);
			}

			server->time_of_last_keep_alive = now;
		}
		if (system_milliseconds() - server->time_of_first_client_loading_completion >=
			NETWORK_GAME_SERVER_MAXIMUM_WAIT_TIME_FOR_LEVEL_LOADING)
		{
			int i;

			for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
			{
				if (TEST_FLAG(
					server->client_machines[i].flags,
					_network_client_machine_connected_bit) &&
					!TEST_FLAG(
						server->client_machines[i].flags,
						_network_client_machine_level_loaded_bit))
				{
					char ascii_name[MAXIMUM_MACHINE_NAME_LENGTH];
					boolean removed;

					network_event(
						"forcibly removing client system '%s' due to timeout while loading for game",
						wide_to_ascii(
							server->game.machines[
								server->client_machines[i].machine_index].name,
							ascii_name,
							sizeof(ascii_name))
							? ascii_name
							: "<unknown name>");
					removed = network_game_server_remove_client_machine_from_game(
						server,
						&server->client_machines[i]);
					match_assert(NETWORK_SERVER_MANAGER_FILE, 0x94E, removed);
				}
			}

			/* (the last removal may have started it) */
			if (server->state == _network_game_server_state_pregame)
				network_game_server_all_machines_have_loaded(server);
		}
	}

	return success;
}

boolean network_game_server_reset_to_pregame(
	struct network_game_server *server)
{
	boolean success = FALSE;
	struct message_server_switch_to_pregame message_packet = { 0 };
	struct network_message *message;
	int i;

	match_assert(NETWORK_SERVER_MANAGER_FILE, 0x324, server);

	csmemset(&server->countdown_state, 0, sizeof(server->countdown_state));
	server->next_update_number = 0;
	server->time_of_first_client_loading_completion = 0;
	server->sent_start_game_message = FALSE;
	server->queued_player_valid = FALSE;
	server->waiting_player_count = 0;
	csmemset(network_game_server_ingame_additions, 0, sizeof(network_game_server_ingame_additions));
	/* Preserve January's 32-bit wrap without overflowing signed arithmetic.
	 * VC7 converts the unsigned result back to the same signed bit pattern.
	 */
	server->game.number_of_games_played =
		(long)((unsigned long)server->game.number_of_games_played + 1);

	if (server->state == _network_game_server_state_postgame)
	{
		message = create_network_game_message(
			_message_server_switch_to_pregame,
			&message_packet,
			sizeof(message_packet));
		/* port: the machines are switched whether or not the message reached
		every one: those it did are in the pregame already (the host's own
		too), and one it did not (its connection failed, and is closed) is
		dropped; the server kept to the postgame for good, and a second try
		switched the others again, which refuse it. Only the machines that
		were in the game: one joining it in progress and not yet started is
		in the pregame already (it refuses the switch), and has the settings
		below. */
		if (message)
		{
			boolean sent = TRUE;

			for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
			{
				struct network_game_server_client_machine *machine = &server->client_machines[i];

				if (network_game_server_client_machine_is_joined_to_game(server, machine) &&
					(TEST_FLAG(machine->flags, _network_client_machine_level_loaded_bit) ||
						TEST_FLAG(machine->flags, _network_client_machine_started_late_bit)) &&
					network_connection_active(machine->connection) &&
					!network_game_server_send_message_to_client_machine(server, machine, message))
				{
					sent = FALSE;
				}
			}
			if (sent)
				network_event("server resetting to pregame");
			else
				network_event("server resetting to pregame (some machines missed it)");

			if (server->game.variant.universal_variant.teams)
			{
				for (i = 0; i < MAXIMUM_NETWORK_PLAYER_COUNT; i++)
				{
					if (network_player_is_valid(&server->game.players[i]))
					{
						switch (server->game.players[i].team_index)
						{
						case _team_red:
							server->game.players[i].team_index = _team_blue;
							break;
						case _team_blue:
							server->game.players[i].team_index = _team_red;
							break;
						}
					}
				}
			}

			for (i = 0; i < MAXIMUM_NETWORK_MACHINE_COUNT; i++)
			{
				SET_FLAG(
					server->client_machines[i].flags,
					_network_client_machine_level_loaded_bit,
					FALSE);
				SET_FLAG(
					server->client_machines[i].flags,
					_network_client_machine_started_late_bit,
					FALSE);
				server->client_machines[i].last_received_update_sequence_number = 0;
			}

			network_game_reset_for_next_round(&server->game, FALSE);
			if (network_game_server_setup_game_from_playlist(server))
			{
				/* the settings record goes out in pieces */
				/* (the pregame whatever a machine missed: the machines are in it,
				and the pregame's flush sends the settings again) */
				if (!network_game_server_send_game_settings_to_all_machines(server, &server->game, sizeof(server->game)))
					network_event("some machines missed the game settings; they are sent again in the pregame");
				server->state = _network_game_server_state_pregame;
				success = TRUE;
			}
			else
			{
				struct message_server_graceful_game_exit_pregame shutdown_message = { 0 };

				message = create_network_game_message(
					_message_server_graceful_game_exit_pregame,
					&shutdown_message,
					sizeof(shutdown_message));
				if (message &&
					network_game_server_send_message_to_all_machines(server, message) &&
					network_game_server_handle_client_machines(server))
				{
					network_event("the playlist has ended - server going down");
				}
				else
				{
					network_event(
						"the playlist has ended - server going down, but failed to alert client machines");
				}
			}
		}
		else
		{
			network_event("failed to create a _message_type_server_switch_to_pregame message");
		}
	}
	else
	{
		success = network_game_server_setup_game_from_playlist(server);

		if (server->game.variant.universal_variant.teams)
		{
			for (i = 0; i < MAXIMUM_NETWORK_PLAYER_COUNT; i++)
			{
				if (network_player_is_valid(&server->game.players[i]))
				{
					switch (server->game.players[i].team_index)
					{
					case _team_red:
						server->game.players[i].team_index = _team_blue;
						break;
					case _team_blue:
						server->game.players[i].team_index = _team_red;
						break;
					}
				}
			}
		}
	}

	return success;
}

#ifdef HALO_WEB
struct network_game_server *network_game_server_adopt_match(struct network_game_client *client, unsigned long epoch)
{
	struct network_game_server *server = &network_game_server_memory_do_not_use_directly;
	long index;
	if (!client || !epoch || network_game_server_memory_do_not_use_directly_in_use ||
		network_game_client_get_state(client, NULL) != _network_game_client_state_ingame)
		return NULL;
	csmemset(server, 0, sizeof(*server));
	server->connection = network_connection_new(FLAG(_connection_create_server_bit), NETWORK_GAME_SERVER_PORT);
	if (!server->connection)
		return NULL;
#ifdef xbox
	transport_server_initialize();
#endif
	network_game_server_memory_do_not_use_directly_in_use = TRUE;
	csmemset(&server_migration, 0, sizeof(server_migration));
	server_migration.adopting = TRUE;
	server_migration.epoch = epoch;
	server_migration.host_machine = network_game_client_get_machine_index(client);
	server_migration.departed_machine = network_game_client_migration_host_machine(client);
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
		server_migration.owner_addresses[index] = network_game_client_migration_owner_address((short)index);
	/* A missing host can be partitioned rather than gone. Fence its authority,
	   retaining its player and recorded owner through the normal rejoin grace. */
	csmemcpy(&server->game, network_game_client_get_game(client), sizeof(server->game));
	server->state = _network_game_server_state_ingame;
	server->flags = FLAG(_network_game_server_game_valid_bit);
	server->sent_start_game_message = TRUE;
	server->next_update_number = MAX(game_time_get(), 1);
	server->time_of_last_keep_alive = system_milliseconds();
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
		server->client_machines[index].machine_index = NONE;
	network_connection_set_connection_rejection_procedure(server->connection, network_game_server_reject_connection_game_is_full);
	/* Promotion bypasses the menu's host setup. A former client must open
	the same remote admission gate before its surviving peers reconnect. */
	network_game_accept_remote_connections(TRUE);
	network_game_server_open_game(server);
	network_event("machine #%d adopted the live match at tick #%ld, epoch #%lu",
		server_migration.host_machine, game_time_get(), epoch);
	return server;
}

void network_game_server_migration_routes(unsigned long *addresses, short count)
{
	struct network_game_server *server = global_network_game_server_get();
	short index;
	if (!server || count != HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return;
	csmemset(addresses, 0, count * sizeof(*addresses));
	if (server_migration.epoch || web_match_migration_enabled())
		for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
			if (network_machine_is_valid(&server->game.machines[index]))
				addresses[index] = server_migration.owner_addresses[index];
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		struct network_game_server_client_machine *machine = &server->client_machines[index];
		if (machine->connection && TEST_FLAG(machine->flags, _network_client_machine_validated_bit) &&
			machine->machine_index >= 0 && machine->machine_index < count)
		{
			struct transport_address address = { 0 };
			network_connection_get_address(machine->connection, &address, NULL);
			addresses[machine->machine_index] = address.address.long_words[0] == IPV4_LOOPBACK_ADDRESS ?
				web_quick_play_address() : address.address.long_words[0];
		}
	}
}

boolean network_game_server_recover_match(struct network_game_server *server, unsigned long epoch)
{
	struct network_game_client *client = global_network_game_client_get();
	unsigned long owners[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short index;
	if (!server || !client || !epoch || epoch <= server_migration.epoch ||
		server->state != _network_game_server_state_ingame)
		return FALSE;
	network_game_server_migration_routes(owners, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
	if (server->connection)
		network_connection_delete(server->connection);
	server->connection = network_connection_new(FLAG(_connection_create_server_bit), NETWORK_GAME_SERVER_PORT);
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		csmemset(&server->client_machines[index], 0, sizeof(server->client_machines[index]));
		server->client_machines[index].machine_index = NONE;
	}
	if (!server->connection)
	{
		/* Retire only the failed listener, preserving the loaded client and
		   world while another survivor can take over. */
		network_game_demote_migration_host();
		return FALSE;
	}
	csmemset(&server_migration, 0, sizeof(server_migration));
	server_migration.adopting = TRUE;
	server_migration.epoch = epoch;
	server_migration.host_machine = network_game_client_get_machine_index(client);
	server_migration.departed_machine = NONE;
	csmemcpy(server_migration.owner_addresses, owners, sizeof(owners));
	server->next_update_number = MAX(game_time_get(), 1);
	server->time_of_last_keep_alive = system_milliseconds();
	network_connection_set_connection_rejection_procedure(server->connection, network_game_server_reject_connection_game_is_full);
	network_game_accept_remote_connections(TRUE);
	network_game_server_open_game(server);
	network_event("existing host #%d renewed live-match transport at tick #%ld, epoch #%lu",
		server_migration.host_machine, game_time_get(), epoch);
	return TRUE;
}

boolean network_game_server_migration_machines(void *buffer, long size)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_migration_membership *membership = buffer;
	if (!server || !buffer || size != sizeof(*membership) ||
		sizeof(server->game.machines) != sizeof(membership->machines))
		return FALSE;
	membership->machine_count = server->game.machine_count;
	csmemcpy(membership->machines, server->game.machines, sizeof(membership->machines));
	return network_game_client_migration_validate_machines(buffer, size);
}

static void network_game_server_migration_acknowledge(struct network_game_server *server)
{
	long index;
	struct network_migration_message ack;
	csmemset(&ack, 0, sizeof(ack));
	ack.type = NETWORK_MIGRATION_MESSAGE;
	ack.version = NETWORK_MIGRATION_VERSION;
	ack.epoch = server_migration.epoch;
	ack.session_seed = server->game.random_seed;
	ack.kind = NETWORK_MIGRATION_ACK;

	ack.host_machine_index = server_migration.host_machine;
	ack.departed_machine_index = server_migration.departed_machine;
	ack.game_tick = game_time_get();
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
		if (network_machine_is_valid(&server->game.machines[index]))
			ack.machine_present[index / 8] |= (byte)(1 << (index % 8));
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		struct network_game_server_client_machine *machine = &server->client_machines[index];
		if (machine->connection && TEST_FLAG(machine->flags, _network_client_machine_validated_bit))
		{
			ack.machine_index = machine->machine_index;
			/* network_connection_write swaps this header in place. Rebuild
			it for every recipient so all survivors receive valid framing. */
			build_message_header(&ack.header, sizeof(ack), 2, 0);
			network_connection_write(machine->connection, &ack, sizeof(ack), NULL, TRUE);
		}
	}

}

boolean network_game_server_handle_migration(struct network_game_server *server,
	struct network_game_server_client_machine *machine, void const *message, word size)
{
	struct network_migration_message attach;
	struct transport_address address = { 0 };
	long index;
	unsigned long expected_address;
	if (size < 4 || ((byte const *)message)[2] != NETWORK_MIGRATION_MESSAGE)
		return FALSE;
	if (size != sizeof(attach))
		return TRUE;
	csmemcpy(&attach, message, sizeof(attach));
	if ((!server_migration.epoch && !web_match_migration_enabled()) ||
		server->state != _network_game_server_state_ingame || GET_MESSAGE_FLAGS(attach.header) ||
		GET_MESSAGE_TYPE(attach.header) != 2 || attach.version != NETWORK_MIGRATION_VERSION ||
		attach.kind != NETWORK_MIGRATION_ATTACH || attach.epoch != server_migration.epoch ||
		attach.session_seed != (unsigned long)server->game.random_seed || attach.machine_index < 0 ||
		attach.machine_index >= MAXIMUM_NETWORK_MACHINE_COUNT ||
		!network_machine_is_valid(&server->game.machines[attach.machine_index]))
		return TRUE;
	if (!server_migration.epoch)
	{
		/* Initial authority also admits an existing player's repaired stream.
		   Routes come only from validated connections and retained owner records. */
		unsigned long owners[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
		struct network_game_client *local = global_network_game_client_get();
		if (!local) return TRUE;
		network_game_server_migration_routes(owners, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		csmemcpy(server_migration.owner_addresses, owners, sizeof(owners));
		server_migration.host_machine = network_game_client_get_machine_index(local);
		server_migration.departed_machine = NONE;
	}
	network_connection_get_address(machine->connection, &address, NULL);
	expected_address = attach.machine_index == server_migration.host_machine ? IPV4_LOOPBACK_ADDRESS :
		server_migration.owner_addresses[attach.machine_index];
	if (!expected_address || address.address.long_words[0] != expected_address)
	{
		network_event("rejected migration reattach: original machine #%d owner address differs", attach.machine_index);
		return TRUE;
	}
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		struct network_game_server_client_machine *other = &server->client_machines[index];
		if (other != machine && other->connection && other->machine_index == attach.machine_index &&
			TEST_FLAG(other->flags, _network_client_machine_validated_bit))
		{
			/* The same recorded owner may replace a half-open old stream.
			   Retire its transport, without deleting its machine or player. */
			network_server_close_client_connection(server->connection, other->connection);
			csmemset(other, 0, sizeof(*other));
			other->machine_index = NONE;
		}
	}
	machine->machine_index = attach.machine_index;
	server_migration.disconnected_at[attach.machine_index] = 0;
	machine->flags = FLAG(_network_client_machine_connected_bit) | FLAG(_network_client_machine_validated_bit) |
		FLAG(_network_client_machine_level_loaded_bit) | FLAG(_network_client_machine_precached_bit);
	machine->last_received_update_sequence_number = MAX(game_time_get(), 1);
	/* The final ACK remains replayable when its transport dropped after
	the cohort committed; there is still no new join/player allocation. */
	if (!server_migration.adopting)
		network_game_server_migration_acknowledge(server);
	return TRUE;
}

boolean network_game_server_migration_ready(struct network_game_server *server)
{
	long index, connection_index;
	if (!server || !server_migration.adopting)
		return FALSE;
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		boolean attached = FALSE;
		if (!network_machine_is_valid(&server->game.machines[index]) || index == server_migration.departed_machine)
			continue;
		for (connection_index = 0; connection_index < MAXIMUM_NETWORK_MACHINE_COUNT; connection_index++)
		{
			struct network_game_server_client_machine *machine = &server->client_machines[connection_index];
			if (machine->machine_index == index && machine->connection &&
				TEST_FLAG(machine->flags, _network_client_machine_validated_bit))
			{
				attached = TRUE;
				break;
			}
		}
		if (!attached)
			return FALSE;
	}
	return TRUE;
}

void network_game_server_migration_finish(struct network_game_server *server)
{
	long index, connection_index;
	if (!server || !server_migration.adopting)
		return;
	for (connection_index = 0; connection_index < MAXIMUM_NETWORK_MACHINE_COUNT; connection_index++)
	{
		struct network_game_server_client_machine *machine = &server->client_machines[connection_index];
		if (machine->connection && !TEST_FLAG(machine->flags, _network_client_machine_validated_bit))
			network_game_server_remove_client_machine_from_game(server, machine);
	}
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
	{
		boolean attached = FALSE;
		if (!network_machine_is_valid(&server->game.machines[index]))
			continue;
		for (connection_index = 0; connection_index < MAXIMUM_NETWORK_MACHINE_COUNT; connection_index++)
		{
			struct network_game_server_client_machine *machine = &server->client_machines[connection_index];
			if (machine->machine_index == index && machine->connection &&
				TEST_FLAG(machine->flags, _network_client_machine_validated_bit))
				attached = TRUE;
		}
		if (!attached)
		{
			/* Resume reachable players, retaining missing owners for bounded
			   retries instead of invalidating their identity after 12 seconds. */
			server_migration.disconnected_at[index] = MAX(system_milliseconds(), 1);
		}
	}
	network_game_server_migration_acknowledge(server);
	server_migration.adopting = FALSE;
}
#endif

#ifdef HALO_WEB
void network_game_server_release_migration_transport(struct network_game_server *server)
{
	if (!server)
		return;
	if (server->connection)
		network_connection_delete(server->connection);
#ifdef xbox
	transport_server_terminate();
#endif
	csmemset(server, 0, sizeof(*server));
	csmemset(&server_migration, 0, sizeof(server_migration));
	network_game_server_memory_do_not_use_directly_in_use = FALSE;
}
#endif
