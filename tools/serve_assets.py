#!/usr/bin/env python3
"""Serve only the selected scene's active meshes and robot visual resources."""
import argparse
from pathlib import Path
import threading

from asset_transport import AssetHTTPServer, AssetRegistry, process_token


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--status', type=Path)
    parser.add_argument('--session', type=Path)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8899)
    parser.add_argument('--cache-mib', type=int, default=64)
    parser.add_argument('--texture-size', type=int, default=512, help='Viewer texture size cap; 0 preserves original texture bytes')
    parser.add_argument('--jpeg-quality', type=int, default=90)
    parser.add_argument('--watch-pid', type=int)
    args = parser.parse_args()
    if not (args.status or args.session) or not 0<=args.port<=65535 or not 1<=args.cache_mib<=4096:
        parser.error('Supply a status/session, a valid port and a cache target from 1 to 4096 MiB')
    owner = process_token(args.watch_pid) if args.watch_pid else None
    if args.watch_pid and owner is None: raise SystemExit('The scene process has exited.')
    if args.texture_size not in (0, 128, 256, 512, 1024, 2048) or not 50<=args.jpeg_quality<=100:
        parser.error('Use a texture size of 0/128/256/512/1024/2048 and JPEG quality from 50 to 100')
    registry = AssetRegistry(args.status, args.session, args.cache_mib*1024*1024, args.texture_size, args.jpeg_quality)
    with AssetHTTPServer((args.bind,args.port),registry) as server:
        stopped = threading.Event()
        def watch():
            while not stopped.wait(1):
                if process_token(args.watch_pid) != owner:
                    server.shutdown()
                    return
        if args.watch_pid: threading.Thread(target=watch, daemon=True).start()
        print(f'Cesium assets: http://{args.bind}:{server.server_port}/v1/status', flush=True)
        try:
            server.serve_forever(poll_interval=.2)
        except KeyboardInterrupt:
            pass
        finally:
            stopped.set()


if __name__ == '__main__':
    main()
