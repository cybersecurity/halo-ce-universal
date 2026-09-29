#!/usr/bin/env python3
"""Bound two actual protocol-4 engines on one Mac using an offline transport alias.

No packet payload is synthesized or changed. Requires the original multiplayer
map data, a matching native binary/ELF and a real configured non-loopback local
IPv4 address. Logs distinguish socket transport from actual two-player gameplay.
"""
from pathlib import Path
import argparse, datetime, ipaddress, json, math, os, re, signal, socket, subprocess, sys, time
from macos_network_test_lock import acquire_network_test_lock
ROOT=Path(__file__).resolve().parents[1]
NETWORK_PORTS=(0x141e,0x141f) # Upstream protocol's server/client ports, 5150/5151.
def check_endpoints(addresses,ports=NETWORK_PORTS):
 # TCP bind+listen with REUSEADDR matches the engine and permits only closed
 # TIME_WAIT connections. Active listeners still fail listen; UDP never reuses.
 # The shared lease excludes other owned harnesses during this preflight.
 for address in addresses:
  for port in ports:
   for kind in (socket.SOCK_STREAM,socket.SOCK_DGRAM):
    with socket.socket(socket.AF_INET,kind) as probe:
     try:
      if kind == socket.SOCK_STREAM:probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
      probe.bind((address,port))
      if kind == socket.SOCK_STREAM:probe.listen(1)
     except OSError as error:raise RuntimeError(f'occupied multiplayer endpoint {address}:{port} {"TCP" if kind==socket.SOCK_STREAM else "UDP"}: {error}') from error
def evidence(log):
 text=log.read_text(errors='replace') if log.exists() else ''
 ticks=[line for line in text.splitlines() if 'network test: tick ' in line and ' | playing | ' in line]
 two=[line for line in ticks if len(re.findall(r' player \d+: \(',line))>=2 and len(set(re.findall(r' m(\d+)',line)))>=2]
 received=[int(x) for line in two for x in re.findall(r' received (\d+)',line)]
 return {'hosting':'network test: hosting ' in text,'joining':'network test: joining' in text,
         'starting':'network test: starting the game' in text,'playing_samples':len(ticks),
         'two_live_players_distinct_machines':len(two),'received_packets':max(received,default=0),
         'player_position_sets':len(set(tuple(re.findall(r' player \d+: \(([^)]*)\)',line)) for line in two)),
         'last_two_player_sample':two[-1] if two else None}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--elf',type=Path,required=True);p.add_argument('--data-root',type=Path,required=True);p.add_argument('--host-ip',required=True)
 p.add_argument('--timeout',type=float,default=150);p.add_argument('--start-delay',type=float,default=25);p.add_argument('--run-id');p.add_argument('--map',default='bloodgulch');p.add_argument('--observer-role',choices=('host','client'),help='Disable scripted input on this role to isolate the opposite replication direction');p.add_argument('--input-mode',choices=('move','bot'),default='move',help='Explicit diagnostic scripted input; move disables fire/grenade/jump');p.add_argument('--resolution',choices=('640x480','1280x720','1920x1080'),default='1280x720');p.add_argument('--map-evidence',action='store_true',help='Capture requested-map state and two rendered frames per role for map-load review');p.add_argument('--check-only',action='store_true');a=p.parse_args()
 host=ipaddress.IPv4Address(a.host_ip)
 if host.is_loopback or host.is_multicast or host.is_unspecified:p.error('host-ip must be an existing non-loopback local IPv4 address')
 if not math.isfinite(a.timeout) or not math.isfinite(a.start_delay) or a.timeout<30 or not 15<=a.start_delay<a.timeout-10:p.error('require finite timeout >=30 and start-delay >=15 within the bound')
 if not re.fullmatch(r'[a-z0-9_]+',a.map):p.error('invalid map name')
 for path in (a.binary,a.elf):
  if not path.is_file():p.error(f'missing {path}')
 if not (a.data_root/'maps'/f'{a.map}.map').is_file():p.error('original multiplayer map is missing')
 # A local bind probe neither sends traffic nor creates an interface alias.
 with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
  try:probe.bind((str(host),0))
  except OSError as e:p.error(f'host-ip is not locally bindable: {e}')
 rid=a.run_id or 'mp-pair-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
 if not re.fullmatch(r'[A-Za-z0-9_-]+',rid):p.error('invalid run-id')
 common={k:v for k,v in os.environ.items() if not k.startswith('HALO_')}
 common.update(HALO_LOCAL_TEST_IPV4_ALIAS='127.0.0.2',HALO_NET_ONLINE='false',HALO_NET_ALLOW_UPNP='false',HALO_NETCODE='distributed',HALO_AUDIO_ENABLE=os.environ.get('HALO_AUDIO_ENABLE','1'),HALO_FULLSCREEN='false',HALO_WINDOW_SCALE='1',HALO_RESOLUTION=a.resolution,HALO_NETWORK_TEST_START=str(a.start_delay),HALO_NETWORK_TRACE='1')
 runs={};processes=[]
 for role in ('host','client'):
  env=common.copy();env.update(HALO_SDL_TEST_ROLE=role,HALO_NET_ADDRESS=str(host) if role=='host' else '127.0.0.2',HALO_NET_BROADCAST='127.0.0.2' if role=='host' else str(host),HALO_NETWORK_TEST=f'host:{a.map}' if role=='host' else 'join',HALO_TEST_INPUT=a.input_mode+(':0' if role=='host' else ':1'))
  if role == a.observer_role:
   env['HALO_TEST_INPUT']=''
  if a.map_evidence:
   capture_times=','.join(str(int((a.timeout-offset)*1000)) for offset in (20,10))
   env.update(HALO_SDL_TEST_WINDOW_POSITION='50,100' if role=='host' else '800,100',HALO_SDL_SWAP_TRACE='1',HALO_INPUT_TRACE='1',HALO_MAP_EVIDENCE='1',HALO_GL_DEBUG='1',HALO_CAPTURE_FRAME_AT_MS=capture_times,HALO_CAPTURE_FRAME_DIR=str(ROOT/'build/macos-aot/run-logs'/(rid+'-'+role+'-frames')),HALO_SDL_TEST_QUIT_AT_MS=str(int((a.timeout-3)*1000)))
  cmd=[sys.executable,str(ROOT/'tools/run_macos_aot_bounded.py'),'--binary',str(a.binary.resolve()),'--elf',str(a.elf.resolve()),'--data-root',str(a.data_root.resolve()),'--timeout',str(a.timeout),'--run-id',rid+'-'+role]
  runs[role]={'command':cmd,'settings':{k:v for k,v in env.items() if k.startswith('HALO_')}}
 if a.check_only:print(json.dumps(runs,indent=2));return 0
 try:
  pair_lock=acquire_network_test_lock()
  check_endpoints((str(host),'127.0.0.1'))
 except RuntimeError as error:p.error(str(error))
 logs=ROOT/'build/macos-aot/run-logs';logs.mkdir(parents=True,exist_ok=True)
 for role in runs:
  if (logs/(rid+'-'+role+'.json')).exists():p.error('run-id already exists; choose a new ID')
 try:
  for role,run in runs.items():
   env=common.copy();env.update(run['settings']);out=(logs/(rid+'-'+role+'-supervisor.log')).open('wb')
   child=subprocess.Popen(run['command'],env=env,stdout=out,stderr=subprocess.STDOUT,start_new_session=True);processes.append((child,out))
  deadline=time.monotonic()+a.timeout+12
  while any(child.poll() is None for child,_ in processes):
   if time.monotonic()>deadline:raise TimeoutError('pair supervisors exceeded their bounds')
   time.sleep(.25)
 finally:
  for child,out in processes:
   if child.poll() is None:
    child.send_signal(signal.SIGINT) # bounded child owns and cleans its native PGID
    try:child.wait(timeout=8)
    except subprocess.TimeoutExpired:child.kill();child.wait()
   out.close()
 result={'run_id':rid,'input_mode':a.input_mode,'observer_role':a.observer_role,'resolution':a.resolution,'transport':'explicit offline logical127.0.0.2/native127.0.0.1 alias; original protocol payload unchanged','roles':{}}
 for role in runs:
  status_path=logs/(rid+'-'+role+'.json');status=json.loads(status_path.read_text()) if status_path.exists() else {}
  result['roles'][role]={'status':status,'evidence':evidence(Path(status.get('log',logs/(rid+'-'+role+'.log'))))}
 result['passed']=all(r['status'].get('process_group_clean') and (r['status'].get('returncode')==0 or (r['status'].get('timed_out') and r['status'].get('returncode')==-signal.SIGTERM)) and r['evidence']['two_live_players_distinct_machines']>=3 and r['evidence']['received_packets']>0 and r['evidence']['player_position_sets']>=2 for r in result['roles'].values()) and result['roles']['client']['evidence']['joining'] and result['roles']['host']['evidence']['hosting']
 target=logs/(rid+'-pair.json');target.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0 if result['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
