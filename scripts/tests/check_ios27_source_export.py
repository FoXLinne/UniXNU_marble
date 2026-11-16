#!/usr/bin/env python3
"""Exercise atomic source exports with failed archives and incomplete cached trees."""
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'stage_ios27_components.py'
spec = importlib.util.spec_from_file_location('marble_stage', path)
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
run = subprocess.run
revision = 'a' * 40
payload = b'pub fn retained() {}\n'


def archive(files):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as tar:
        for name, data in files.items():
            item = tarfile.TarInfo(name)
            item.size = len(data)
            tar.addfile(item, io.BytesIO(data))
    return stream.getvalue()


def repo(root):
    item = stage.Repo(root / 'repo', root / 'scratch')
    item.tree = lambda _: {'source.rs': ('100644', 'blob', stage.blob_sha(payload))}
    return item


def execute(blob=None, failure=0):
    def mock(command, **kwargs):
        if command[0] == 'git':
            kwargs['stdout'].write(blob or b'')
            return subprocess.CompletedProcess(command, failure, stderr=b'injected archive failure' if failure else b'')
        return run(command, **kwargs)
    return mock


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    item = repo(root / 'success')
    destination = item.scratch / 'export' / ('repo-' + revision[:10])
    destination.mkdir(parents=True)  # The old failed pipeline left this empty.
    with patch.object(stage.subprocess, 'run', side_effect=execute(archive({'source.rs': payload}))) as mocked:
        result = item.export(revision)
        assert (result / 'source.rs').read_bytes() == payload
        assert (result / '.marble-export-revision').read_text().strip() == revision
        assert mocked.call_count == 2
    with patch.object(stage.subprocess, 'run', side_effect=AssertionError('completed cache was rebuilt')):
        assert repo(root / 'success').export(revision) == result

    legacy = repo(root / 'legacy-complete')
    legacy_dest = legacy.scratch / 'export' / ('repo-' + revision[:10])
    legacy_dest.mkdir(parents=True)
    (legacy_dest / 'source.rs').write_bytes(payload)
    with patch.object(stage.subprocess, 'run', side_effect=AssertionError('verified legacy cache was rebuilt')):
        assert legacy.blob(revision, stage.blob_sha(payload), 'source.rs') == payload
    (legacy_dest / 'source.rs').write_bytes(b'corrupted')
    try:
        legacy.blob(revision, stage.blob_sha(payload), 'source.rs')
    except ValueError:
        pass
    else:
        raise AssertionError('corrupted exported blob was accepted')

    for name, blob, failure in [('archive-fails', b'', 7), ('missing-file', archive({'other.rs': payload}), 0),
                                ('invalid-tar', b'invalid archive', 0)]:
        item = repo(root / name)
        try:
            with patch.object(stage.subprocess, 'run', side_effect=execute(blob, failure)):
                item.export(revision)
        except (RuntimeError, subprocess.CalledProcessError):
            assert revision not in item.exports
            assert not (item.scratch / 'export' / ('repo-' + revision[:10])).exists()
        else:
            raise AssertionError(name + ' exposed an incomplete export')
print('PASS: empty cache repair, complete and verified legacy caches, blob integrity, git failure, incomplete archive and tar failure')
