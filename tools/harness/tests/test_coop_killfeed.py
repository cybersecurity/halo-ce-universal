"""The complete co-op death formatter, with full-handle and mode checks."""
import re,sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,mutated,run,CHECK_FAILED
def generated(fault=None):
    code=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/network_killfeed.c'),flags=re.M)
    if fault=='host':code=mutated(code,'game_connection() != _game_connection_network_server','FALSE')
    if fault=='dedup':code=mutated(code,'killfeed_dead_units[slot] == unit_index','FALSE')
    if fault=='backlink':code=mutated(code,'dead->unit_index != unit_index','FALSE')
    return (('under_test.inc',code),)
@pytest.mark.parametrize('case',['causes','players','dedup','scope','stale','fallback','direct'])
def test_case(case):
    status,output=run(build('coop_killfeed',generated()),case)
    assert status==0,output
@pytest.mark.parametrize('fault,case',[('host','scope'),('dedup','dedup'),('backlink','stale')])
def test_negative(fault,case):
    status,output=run(build('coop_killfeed',generated(fault)),case)
    assert status==CHECK_FAILED,output
