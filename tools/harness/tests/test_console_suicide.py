"""Compile the complete production self-kill handler against a small world."""
import re,sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,mutated,run,CHECK_FAILED
def generated(fault=None):
    code=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/network_suicide.c'),flags=re.M)
    if fault=='stream': code=mutated(code,'!from_stream ||','FALSE ||')
    if fault=='owner': code=mutated(code,'player->network_player_data.machine_index != machine_index','FALSE')
    if fault=='unit': code=mutated(code,'player->unit_index == unit_index &&','TRUE &&')
    return (('under_test.inc',code),)
@pytest.mark.parametrize('case',['commands','client','admission','stale','rate','scope'])
def test_case(case):
    status,output=run(build('console_suicide',generated()),case)
    assert status==0,output
@pytest.mark.parametrize('fault,case',[('stream','admission'),('owner','admission'),('unit','stale')])
def test_negative(fault,case):
    status,output=run(build('console_suicide',generated(fault)),case)
    assert status==CHECK_FAILED,output
