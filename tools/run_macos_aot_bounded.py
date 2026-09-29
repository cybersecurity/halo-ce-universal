#!/usr/bin/env python3
"""Run the native AOT guest with durable logs and process-group cleanup."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def cleanup_group(process: subprocess.Popen, grace_seconds: float = 2.0) -> bool:
	group_id = process.pid
	def group_exists() -> bool:
		# Reap our child before probing its group; Darwin can reject a probe
		# while the terminating child is still an unreaped process.
		process.poll()
		try:
			os.killpg(group_id, 0)
			return True
		except ProcessLookupError:
			return False
		except PermissionError:
			groups = subprocess.check_output(["ps", "-axo", "pgid="], text=True)
			return any(int(value) == group_id for value in groups.split())
	if not group_exists():
		return True
	try:
		os.killpg(group_id, signal.SIGTERM)
	except ProcessLookupError:
		return True
	except PermissionError:
		return not group_exists()
	deadline = time.monotonic() + grace_seconds
	while time.monotonic() < deadline:
		if not group_exists():
			return True
		time.sleep(0.05)
	try:
		os.killpg(group_id, signal.SIGKILL)
	except ProcessLookupError:
		return True
	except PermissionError:
		return not group_exists()
	deadline = time.monotonic() + 1.0
	while time.monotonic() < deadline:
		if not group_exists():
			return True
		time.sleep(0.05)
	return False


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--timeout", type=float, default=45.0)
	parser.add_argument("--binary", type=Path, default=ROOT / "build/native/halo_macos_aot")
	parser.add_argument("--elf", type=Path, default=ROOT / "build/macos-aot/halo_guest.elf")
	parser.add_argument("--data-root", type=Path, required=True)
	parser.add_argument("--run-id")
	parser.add_argument("--native-app", type=Path, help="run the packaged native launcher with an external owned deadline")
	parser.add_argument("--sample-at", type=float,
		help="sample the owned game process once at this elapsed second")
	args = parser.parse_args()
	if args.native_app:
		args.binary = args.native_app / "Contents/MacOS/halo_engine"
		args.elf = args.native_app / "Contents/Resources/halo_guest.elf"
	if args.timeout <= 0:
		parser.error("--timeout must be positive")
	if args.sample_at is not None and not 0 < args.sample_at < args.timeout:
		parser.error("--sample-at must fall within the bounded run")
	run_id = args.run_id or dt.datetime.now().strftime("%Y%m%d-%H%M%S")
	if not re.fullmatch(r"[A-Za-z0-9_-]+", run_id): parser.error("invalid run-id")
	logs = ROOT / "build/macos-aot/run-logs"
	run_root = ROOT / "build/macos-aot/run-data" / run_id
	if run_root.exists() or (logs / f"{run_id}.json").exists(): parser.error("run-id exists; choose a new run-id to preserve its state/logs")
	save_root = run_root / "save"
	data_root = run_root / "data"
	data_source = args.data_root.resolve()
	logs.mkdir(parents=True, exist_ok=True)
	save_root.mkdir(parents=True, exist_ok=True)
	data_root.mkdir(parents=True, exist_ok=True)
	if not (data_source / "maps").is_dir():
		parser.error(f"--data-root must contain a maps/ directory: {data_source}")
	for source in data_source.iterdir():
		if source.name == "config.toml":
			continue
		target = data_root / source.name
		if target.exists() or target.is_symlink():
			continue
		target.symlink_to(source.resolve(), target_is_directory=source.is_dir())
	log_path = logs / f"{run_id}.log"
	status_path = logs / f"{run_id}.json"
	env = os.environ.copy()
	env.update({
		"HALO_GUEST_ELF": str(args.elf.resolve()),
		"HALO_DATA_ROOT": str(data_root.resolve()),
		"HALO_SAVE_ROOT": str(save_root.resolve()),
		"HALO_PROJECT_ROOT": str(ROOT),
		"HALO_CONFIG_ROOT": str(save_root.resolve()),
	})
	for test_variable in ("HALO_TEST_START_AT_FRAME", "HALO_TEST_KEY_SEQUENCE",
		"HALO_TEST_DISABLE_CULL", "HALO_TEST_INVERT_FRONTFACE"):
		env.pop(test_variable, None)
	started = time.monotonic()
	status: dict[str, object] = {
		"run_id": run_id,
		"command": [str(args.binary.resolve())],
		"elf": str(args.elf.resolve()),
		"elf_sha256": hashlib.sha256(args.elf.read_bytes()).hexdigest(),
		"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
		"data_root": str(data_root.resolve()),
		"data_source": str(data_source),
		"save_root": str(save_root.resolve()),
		"log": str(log_path),
		"timeout_seconds": args.timeout,
	}
	if env.get("HALO_CAPTURE_FRAME_DIR"):
		Path(env["HALO_CAPTURE_FRAME_DIR"]).mkdir(parents=True, exist_ok=True)
		status["capture_frame_dir"] = env["HALO_CAPTURE_FRAME_DIR"]
	if args.native_app:
		log_path = run_root / "halo.log"
		status["log"] = str(log_path)
		status["app"] = str(args.native_app.resolve())
		status["command"] = [str((args.native_app / "Contents/MacOS/Halo Combat Evolved").resolve()),
			"--state-root", str(run_root)]
		if any(key.startswith(("HALO_CAPTURE_", "HALO_SDL_TEST_")) or key == "HALO_FRAME_METRICS" for key in env):
			status["command"].append("--diagnostic")
	with log_path.open("wb") as log:
		process = subprocess.Popen(status["command"], cwd=ROOT,
			stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
			env=env, start_new_session=True)
		status["pid"] = process.pid
		status["pgid"] = process.pid
		stop_sample = threading.Event()
		sample_thread = None
		if args.sample_at is not None:
			def take_sample() -> None:
				if stop_sample.wait(args.sample_at) or process.poll() is not None:
					return
				path = logs / f"{run_id}.sample.txt"
				status["sample"] = str(path)
				status["sample_started_seconds"] = round(time.monotonic() - started, 3)
				try:
					result = subprocess.run(["/usr/bin/sample", str(process.pid), "1", "-file", str(path)],
						stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
					status["sample_returncode"] = result.returncode
					if result.returncode:
						status["sample_error"] = result.stdout.strip()
				except subprocess.TimeoutExpired:
					status["sample_error"] = "owned-process sample exceeded 15 seconds"
			sample_thread = threading.Thread(target=take_sample, daemon=True)
			sample_thread.start()
		try:
			try:
				code = process.wait(timeout=args.timeout)
				status["returncode"] = code
				status["timed_out"] = False
				# The guest is allowed to create helper processes; the supervisor
				# owns this new process group and cleans survivors on all exits.
				status["process_group_clean"] = cleanup_group(process)
			except subprocess.TimeoutExpired:
				status["timed_out"] = True
				status["process_group_clean"] = cleanup_group(process)
				process.wait()
				status["returncode"] = process.returncode
				status["supervisor_code"] = 124
		except KeyboardInterrupt:
			status["interrupted"] = True
			status["process_group_clean"] = cleanup_group(process)
			process.wait()
			status["returncode"] = process.returncode
			status["supervisor_code"] = 130
		stop_sample.set()
		if sample_thread is not None:
			sample_thread.join(timeout=16)
	status["elapsed_seconds"] = round(time.monotonic() - started, 3)
	status_path.write_text(json.dumps(status, indent=2) + "\n")
	print(json.dumps(status, indent=2))
	return int(status.get("supervisor_code", status.get("returncode", 1)))


if __name__ == "__main__":
	sys.exit(main())
