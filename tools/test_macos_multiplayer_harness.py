#!/usr/bin/env python3
"""Test endpoint and concurrency guards without launching either game engine."""
import socket
import tempfile
from unittest.mock import patch
from run_macos_multiplayer_pair import check_endpoints
from macos_network_test_lock import acquire_network_test_lock


def main():
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        with socket.socket(socket.AF_INET, kind) as occupied:
            occupied.bind(('127.0.0.1', 0))
            port = occupied.getsockname()[1]
            try:
                check_endpoints(('127.0.0.1',), (port,))
            except RuntimeError as error:
                assert f':{port}' in str(error)
                assert ('TCP' if kind == socket.SOCK_STREAM else 'UDP') in str(error)
            else:
                raise AssertionError('occupied endpoint was accepted')
        check_endpoints(('127.0.0.1',), (port,))
    # A real previously connected listener leaves TIME_WAIT but no live server.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
        listener.listen(1)
        try:
            check_endpoints(('127.0.0.1',), (port,))
        except RuntimeError:
            pass
        else:
            raise AssertionError('active reusable listener was accepted')
        with socket.create_connection(('127.0.0.1', port)) as client:
            accepted, _ = listener.accept()
            accepted.close()  # Server actively closes, establishing local TIME_WAIT.
            assert client.recv(1) == b''
    check_endpoints(('127.0.0.1',), (port,))
    with tempfile.TemporaryDirectory(prefix='halo-pair-guard-') as root:
        with patch('macos_network_test_lock.tempfile.gettempdir', return_value=root):
            owner = acquire_network_test_lock()
            try:
                try:
                    acquire_network_test_lock()
                except RuntimeError:
                    pass
                else:
                    raise AssertionError('concurrent alias pair was accepted')
            finally:
                owner.close()
            following = acquire_network_test_lock()
            following.close()
    print('Multiplayer supervisor guards PASS: occupied TCP/UDP endpoints, reusable TCP listener exclusion, closed TCP/TIME_WAIT reuse, concurrent alias pair, release/reacquire')


if __name__ == '__main__':
    main()
