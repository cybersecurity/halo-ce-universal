"""Serialize original local multiplayer test harnesses, without changing ports."""
import fcntl
import os
from pathlib import Path
import tempfile


def acquire_network_test_lock():
    """Hold returned file open for the entire owned engine lifecycle."""
    path = Path(tempfile.gettempdir()) / f'halo-ce-network-test-{os.getuid()}.lock'
    descriptor = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    lease = os.fdopen(descriptor, 'r+')
    try:
        fcntl.flock(lease.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lease.close()
        raise RuntimeError('Another local Halo multiplayer test owns the shared endpoints. Wait for it to finish; serialize multiplayer map starts.') from None
    return lease
