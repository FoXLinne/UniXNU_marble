#!/usr/bin/env python3
"""Send UI actions through the active observer and retain a settled screenshot."""
import argparse
import json
import os
from pathlib import Path
import select
import time
import tempfile


def await_change(path, predicate, deadline):
    watched = path if path.exists() else path.parent
    descriptor = os.open(watched, os.O_RDONLY)
    queue = select.kqueue()
    event = select.kevent(descriptor, filter=select.KQ_FILTER_VNODE,
                         flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                         fflags=select.KQ_NOTE_WRITE | select.KQ_NOTE_RENAME)
    queue.control([event], 0, 0)
    try:
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Observer UI result pending; running device state retained')
            queue.control(None, 1, min(remaining, 5))
    finally:
        queue.close()
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--output', type=Path, help='Screenshot path; defaults to the temporary UI directory')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--tap', type=int, nargs=2)
    action.add_argument('--swipe', type=int, nargs=4)
    action.add_argument('--button', choices=('power', 'volume-up', 'volume-down'))
    parser.add_argument('--duration-ms', type=int, default=500)
    parser.add_argument('--hold-ms', type=int, default=0)
    parser.add_argument('--settle', type=float, default=2)
    args = parser.parse_args()
    if not args.name.replace('-', '').replace('_', '').isalnum():
        parser.error('Capture name requires letters, digits, hyphens or underscores')
    if not 0 <= args.settle <= 10:
        parser.error('Settle interval requires 0..10 seconds')
    root = args.evidence.resolve()
    redirected = set()
    while (root / 'observer-output-redirect.json').exists():
        if root in redirected:
            raise RuntimeError('Observer evidence redirect loop')
        redirected.add(root)
        target = Path(json.loads((root / 'observer-output-redirect.json').read_text())['output'])
        if target == root:
            break
        root = target.resolve()
    temporary = Path(tempfile.gettempdir()) / 'marble-live-ui'
    temporary.mkdir(exist_ok=True)
    output = args.output.resolve() if args.output else temporary / (args.name + '.png')
    if output.exists():
        parser.error('Capture name exists; select a new evidence name')
    operations = []
    if args.tap:
        operations = [dict(command='tap', point=args.tap)]
    elif args.swipe:
        operations = [dict(command='swipe', **{'from': args.swipe[:2], 'to': args.swipe[2:]},
                           duration_ms=args.duration_ms, hold_ms=args.hold_ms)]
    elif args.button:
        operations = [dict(command='button', button=args.button)]
    began = time.time_ns()
    deadline = time.monotonic() + 90
    if operations:
        for operation in operations:
            operation['capture'] = False
        request, response = root / 'ui-request.json', root / 'ui-response.json'
        if request.exists():
            raise RuntimeError('An observer UI request is pending')
        request.write_text(json.dumps(operations) + '\n')
        def acknowledged():
            try:
                if response.stat().st_mtime_ns < began:return False
                value=json.loads(response.read_text())
                if value.get('operations')!=operations:return False
                if value.get('error'):raise RuntimeError(value['error'])
                return True
            except (FileNotFoundError, json.JSONDecodeError):
                return False
        await_change(response, acknowledged, deadline)
    time.sleep(args.settle)
    request = root / 'context-request.json'
    if request.exists():
        raise RuntimeError('An observer context request is pending')
    began = time.time_ns()
    request.write_text(json.dumps({'processes': [], 'fresh_screen': True}) + '\n')
    screen = root / 'screen.png'
    def fresh_capture():
        nonlocal screen
        manifest = root / 'live-screen.json'
        if manifest.exists():
            value = json.loads(manifest.read_text())
            if value['captured_at_ns'] > began:
                screen = Path(value['path'])
        return screen.exists() and screen.stat().st_mtime_ns > began
    await_change(root, fresh_capture, deadline)
    # screenshot() writes its final PNG with one write_bytes call. Give the
    # open writer time to finish before another tool reads the artifact.
    time.sleep(.2)
    output.write_bytes(screen.read_bytes())
    output.with_suffix('.json').write_text(json.dumps(
        dict(operations=operations, screenshot=str(output), observed_at_ns=time.time_ns()), indent=2) + '\n')
    print(output, flush=True)


if __name__ == '__main__':
    main()
