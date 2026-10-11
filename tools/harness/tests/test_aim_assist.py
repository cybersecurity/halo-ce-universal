"""Host aim policy and real mouse/controller/touch selection, without assets."""
import re,sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,function,mutated,run,CHECK_FAILED

def generated(fault=None):
    policy=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/network_aim_assist.c'),flags=re.M)
    input_source=read('port/linux/src/xinput_sdl.c')
    inp='\n'.join(function(input_source,n) for n in ('mouse_is_aim_device','halo_linux_mouse_aiming'))
    if fault=='authority': policy=mutated(policy,'machine_index != NONE ||','FALSE ||')
    if fault=='override': inp=mutated(inp,'network_aim_assist_block_mouse() &&','FALSE &&')
    if fault=='controller': inp=mutated(inp,'if (gamepad_index != 0) return FALSE;','if (FALSE) return FALSE;')
    return (('policy.inc',policy),('input.inc',inp))

@pytest.mark.parametrize('case',['allowed','devices','scope','replication','admission','reset'])
def test_case(case):
    status,output=run(build('aim_assist',generated()),case)
    assert status==0,output

@pytest.mark.parametrize('fault,case',[('authority','admission'),('override','devices')])
def test_negative(fault,case):
    status,output=run(build('aim_assist',generated(fault)),case)
    assert status==CHECK_FAILED,output

def test_menu():
    import port_settings as p
    import xml.etree.ElementTree as ET
    root=ET.fromstring('\n'.join(p.multiplayer_files()[p.PLAYER_EDIT.replace('/','.')+'.port.xml']))
    choices=root.find("strings[@name='"+p.PLAYER_EDIT+"/var_aim_assist']")
    assert [s.attrib['text'] for s in choices]==['ALLOWED','BLOCK M+KB']
    assert 313+28<345 and 345+60<414
    assert '"network.block_mouse_aim_assist", { "false", "true" }' in read('port/linux/game/menu_functions.c')
