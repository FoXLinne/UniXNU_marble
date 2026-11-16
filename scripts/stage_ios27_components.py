#!/usr/bin/env python3
"""Stage marble driver/component sources for iOS 27 from upstream + local deltas.

For every component tracked in ``source_origins.json`` under ``ios18/sources``
this performs a true three-way merge:

    base     = recorded ``upstream_commit`` (the pin the local tree was cut from)
    local    = ``ios18/sources/<name>`` (marble hardware adaptations; files the
               earlier partial sync took from ``pending_upstream_commit`` use
               that blob as their merge ancestor)
    upstream = the freshly fetched revision in ``upstream_revisions.json`` —
               git objects only, never the stale working checkouts

Merged output goes to ``ios27/sources/<name>`` (tracked, self-contained like
``ios18/sources``): no .git metadata, caches, agent files, or binary inputs.
Unmerged paths keep conflict markers and fail integration; a staged conflicted
file without markers is preserved as a manual resolution, which keeps the
script idempotent.  Per-component pins and a digest of the staged tree are
recorded in ``records/ios27-24B119/source-integration.json``; a rerun reports
pin changes and recomputes affected components.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
WORKSPACE = DEVICE.parents[1]
SCRATCH = DEVICE / 'build' / 'ios27-source-integration'
RECORD_DIR = DEVICE / 'records' / 'ios27-24B119'
OUT_DIR = DEVICE / 'ios27' / 'sources'
LOCAL_ROOT = DEVICE / 'ios18' / 'sources'
TARGET = '27.2'
BUILD = '24B119'

# origins = source_origins.json entry; extra_refs = additional upstream
# revisions that verbatim local copies may descend from (refreshed to `new`).
COMPONENTS = [
    {'dir': 'Adreno740', 'origins': 'Adreno740'},
    {'dir': 'AdrenoMetal', 'origins': 'AdrenoMetal'},
    {'dir': 'ArmSmmu4K', 'origins': 'ArmSmmu4K'},
    {'dir': 'GopFramebuffer', 'origins': 'GopFramebuffer'},
    {'dir': 'XnuPrelink', 'origins': 'XnuPrelink'},
    {'dir': 'RamBlock', 'origins': 'RamBlock'},
    {'dir': 'SoftwareAES', 'origins': 'SoftwareAES'},
    {'dir': 'AppleKeyStoreState', 'origins': 'AppleKeyStoreState'},
    {'dir': 'AdrenoPM4', 'origins': 'AdrenoPM4'},
    {'dir': 'AdrenoFirmware', 'origins': 'AdrenoFirmware'},
    {'dir': 'ArmCpu', 'origins': 'ArmCpu'},
    {'dir': 'ArmDTPlatform', 'origins': 'ArmDTPlatform'},
    {'dir': 'DarwinBoot', 'origins': 'DarwinBoot'},
    {'dir': 'ShengUfs', 'origins': 'ShengUfs'},
    {'dir': 'QcomGeniSpi', 'origins': 'QcomGeniSpi'},
    {'dir': 'Ath12kWifi', 'origins': 'Ath12kWifi'},
    {'dir': 'Mesa', 'origins': 'ios18-Mesa', 'scoped': True,
     'extra_refs': ['85fda3b36405bcaea8bffc64aa4f2651dc90770d']},
    {'dir': 'metal2vulkan', 'origins': 'metal2vulkan'},
]

EXCLUDE_DIRS = {'.git', '__pycache__', '.cache', 'node_modules', 'target'}
ROOT_BUILD_DIRS = {'deps', 'out', 'build'}
EXCLUDE_FILES = {'.DS_Store', 'AGENTS.md'}
EXCLUDE_SUFFIXES = ('.pyc', '.pyo', '.orig', '.rej', '.bak', '.swp', '.stage', '~')
MARKERS = (b'<<<<<<<', b'>>>>>>>', b'|||||||')


def normalize_url(url):
    url = (url or '').strip()
    if url.startswith('git@'):
        url = url[4:].replace(':', '/', 1)
    else:
        url = url.split('://', 1)[-1]
    return url.removesuffix('.git').rstrip('/').lower()


def git(repo, *args):
    proc = subprocess.run(['git', '-C', str(repo), *args], capture_output=True,
                          timeout=300)
    if proc.returncode != 0:
        raise SystemExit(f'git -C {repo} {" ".join(args)}: {proc.stderr.decode()[:300]}')
    return proc.stdout


def list_tree(repo, rev):
    out = git(repo, 'ls-tree', '-r', '-z', rev).decode()
    entries = {}
    for record in out.split('\0'):
        if not record:
            continue
        meta, path = record.split('\t', 1)
        mode, kind, sha = meta.split(' ')
        entries[path] = (mode, kind, sha)
    return entries


class Repo:
    """Git objects at fetched revisions; trees are exported once to scratch so
    blob reads are plain file reads (never the stale working checkout)."""

    def __init__(self, path, scratch):
        self.path = Path(path)
        self.scratch = Path(scratch)
        self.trees = {}
        self.exports = {}

    def tree(self, rev):
        if rev not in self.trees:
            self.trees[rev] = list_tree(self.path, rev)
        return self.trees[rev]

    def export(self, rev):
        if rev not in self.exports:
            dest = self.scratch / 'export' / f'{self.path.name}-{rev[:10]}'
            marker = dest / '.marble-export-revision'
            if dest.is_dir() and not marker.exists():
                valid = True
                for name, (_, kind, sha) in self.tree(rev).items():
                    if kind != 'blob' or excluded(name):
                        continue
                    file = dest / name
                    try:
                        data = os.fsencode(os.readlink(file)) if file.is_symlink() else file.read_bytes()
                    except OSError:
                        valid = False
                        break
                    if blob_sha(data) != sha:
                        valid = False
                        break
                if valid:
                    marker.write_text(rev + '\n')
            if not marker.is_file() or marker.read_text().strip() != rev:
                dest.parent.mkdir(parents=True, exist_ok=True)
                # Check git's status independently. A failed archive piped into tar
                # can leave an empty directory with tar reporting success.
                with tempfile.TemporaryDirectory(dir=dest.parent) as temporary:
                    temporary = Path(temporary)
                    archive = temporary / 'source.tar'
                    with archive.open('wb') as stream:
                        proc = subprocess.run(['git', '-C', str(self.path), 'archive', rev],
                                              stdout=stream, stderr=subprocess.PIPE, timeout=600)
                    if proc.returncode:
                        raise RuntimeError(f'export {self.path}@{rev}: {proc.stderr.decode()[:1000]}')
                    staged = temporary / 'source'
                    staged.mkdir()
                    subprocess.run(['tar', '-xf', str(archive), '-C', str(staged)],
                                   check=True, capture_output=True, timeout=600)
                    required = [name for name, (_, kind, _) in self.tree(rev).items()
                                if kind == 'blob' and not excluded(name)]
                    missing = [name for name in required
                               if not (staged / name).is_file() and not (staged / name).is_symlink()]
                    if missing:
                        raise RuntimeError(f'incomplete export {rev}: {missing[:8]}')
                    (staged / marker.name).write_text(rev + '\n')
                    if dest.exists():
                        shutil.rmtree(dest)
                    staged.rename(dest)
            self.exports[rev] = dest
        return self.exports[rev]

    def blob(self, rev, sha, path):
        fp = self.export(rev) / path
        data = os.fsencode(os.readlink(fp)) if fp.is_symlink() else fp.read_bytes()
        if blob_sha(data) != sha:
            raise ValueError(f'exported blob differs from {rev}:{path}')
        return data

    def link_target(self, rev, path):
        return os.readlink(self.export(rev) / path)


def blob_sha(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def is_binary(data):
    return b'\0' in data[:8192]


def has_markers(data):
    return any(m in data[:262144] for m in MARKERS)


def excluded(rel):
    parts = Path(rel).parts
    if len(parts) > 1 and parts[0] in ROOT_BUILD_DIRS:
        return True
    if any(p in EXCLUDE_DIRS for p in parts):
        return True
    name = parts[-1]
    return name in EXCLUDE_FILES or name.endswith(EXCLUDE_SUFFIXES)


def local_files(directory):
    files = {}
    if not directory.is_dir():
        return files
    for root, dirs, names in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS
                         and not (Path(root) == directory and d in ROOT_BUILD_DIRS))
        for fname in sorted(names):
            path = Path(root) / fname
            rel = str(path.relative_to(directory))
            if excluded(rel):
                continue
            if path.is_symlink():
                files[rel] = ('link', os.readlink(path))
            elif path.is_file():
                files[rel] = ('file', path.read_bytes())
    return files


def merge_text(base, local, upstream, label):
    with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
        tmp = Path(tmp)
        for tag, data in (('local', local), ('base', base), ('up', upstream)):
            (tmp / tag).write_bytes(data)
        proc = subprocess.run(
            ['git', 'merge-file', '-p', '--diff3',
             '-L', f'{label} (local)', '-L', 'base', '-L', 'upstream',
             str(tmp / 'local'), str(tmp / 'base'), str(tmp / 'up')],
            capture_output=True)
        return proc.stdout, max(proc.returncode, 0)


def tree_digest(files):
    h = hashlib.sha256()
    for rel in sorted(files):
        kind, content = files[rel]
        digest = (hashlib.sha256(os.fsencode(content)).hexdigest()
                  if kind == 'link' else hashlib.sha256(content).hexdigest())
        h.update(rel.encode() + b'\0' + digest.encode() + b'\0' + kind.encode() + b'\0')
    return h.hexdigest()


def load_json(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except Exception:
        return default


def in_scope(path, scopes):
    return not scopes or any(path == s or path.startswith(s + '/') for s in scopes)


def run_component(spec, origins, revisions_by_url, write, prior=None, advance=False):
    name = spec['dir']
    origin = origins[spec['origins']]
    url = normalize_url(origin['upstream_url'])
    rows = revisions_by_url.get(url)
    if not rows:
        return {'name': name, 'status': 'gap', 'files': 0, 'detail': {},
                'semantic_gaps': [{'path': '-', 'kind': 'no_fetched_repo',
                                   'url': origin['upstream_url']}]}
    candidates = [r for r in rows if (WORKSPACE / r['path']).exists()] or rows
    fetch_only = [r for r in candidates if r['path'].startswith('work/upstream/')]
    row = (fetch_only or candidates)[0]
    repo = Repo(WORKSPACE / row['path'], SCRATCH / name)
    base_rev = origin['upstream_commit']
    new_rev = row['revision']
    pend_rev = origin.get('pending_upstream_commit')
    scopes = origin.get('source_paths') if spec.get('scoped') else None
    origin_base_rev = base_rev
    advancing = bool(advance and prior and (OUT_DIR / name).is_dir())
    if advancing:
        # The staged tree includes resolved marble adaptations. Merge the
        # next upstream delta into that tree with its recorded pin as base.
        base_rev, pend_rev = prior[1], None

    for rev in (base_rev, new_rev, pend_rev, *(spec.get('extra_refs') or [])):
        if rev:
            repo.tree(rev)  # raises if the fetched objects are absent

    base_tree = {p: v for p, v in repo.tree(base_rev).items() if in_scope(p, scopes)}
    new_tree = {p: v for p, v in repo.tree(new_rev).items() if in_scope(p, scopes)}
    pend_tree = ({} if not pend_rev or pend_rev in (base_rev, new_rev)
                 else {p: v for p, v in repo.tree(pend_rev).items()
                       if in_scope(p, scopes)})
    if scopes:
        extra_sets = [repo.tree(base_rev)]
        if pend_rev and pend_rev not in (base_rev, new_rev):
            extra_sets.append(repo.tree(pend_rev))
        for ref in spec.get('extra_refs') or []:
            extra_sets.append(repo.tree(ref))
        any_new = repo.tree(new_rev)
    else:
        extra_sets, any_new = [], {}

    staged_current = local_files(OUT_DIR / name)
    local = staged_current if advancing else local_files(LOCAL_ROOT / name)
    # Manual edits staged under a previous run are only trusted when the
    # base/new pins are unchanged; a pin move means they may no longer apply.
    pins_same = prior is not None and tuple(prior) == (base_rev, new_rev)

    keys = ('merged_clean', 'local_only', 'upstream_added', 'marble_removed',
            'upstream_removed_kept', 'deleted_upstream', 'conflicts',
            'manual_resolutions', 'excluded_binary', 'refreshed_copy',
            'excluded_meta', 'stale_edits_dropped')
    detail = {k: [] for k in keys}
    files_out = {}

    # Files that exist only in the staged tree (hand-added on top of the
    # merge, e.g. patch-applied API files) are manual work: keep them while
    # the pins stand, drop them when a pin moved.
    for rel in sorted(set(staged_current) - set(local) - set(base_tree)
                      - set(new_tree)):
        if excluded(rel):
            continue
        kind, content = staged_current[rel]
        if kind == 'file' and not has_markers(content):
            if pins_same:
                files_out[rel] = (kind, content)
                detail['manual_resolutions'].append(rel)
            else:
                detail['stale_edits_dropped'].append(rel)
        else:
            files_out[rel] = (kind, content)
            detail['manual_resolutions'].append(rel)

    paths = sorted(set(local) | set(base_tree) | set(new_tree))
    for rel in paths:
        if excluded(rel):
            detail['excluded_meta'].append(rel)
            continue
        lkind, lcontent = local.get(rel, (None, None))
        in_base, in_new = rel in base_tree, rel in new_tree
        b_mode = base_tree.get(rel, ('',))[0]
        n_mode = new_tree.get(rel, ('',))[0]
        link_side = lkind == 'link' or b_mode == '120000' or n_mode == '120000'
        _nb = []

        def get_nb():
            if not _nb:
                _nb.append(repo.blob(new_rev, new_tree[rel][2], rel))
            return _nb[0]

        def take(chosen, kind='merged_clean'):
            # A staged file without conflict markers that differs from the
            # computed content is a manual merge resolution (or a hand edit):
            # keep it verbatim rather than regressing it on every rerun, but
            # only while base/new pins still match the recorded staging.
            cur = staged_current.get(rel)
            if (cur and cur[0] == 'file' and not has_markers(cur[1])
                    and cur[1] != chosen):
                if pins_same:
                    files_out[rel] = ('file', cur[1])
                    detail['manual_resolutions'].append(rel)
                    return
                detail['stale_edits_dropped'].append(rel)
            files_out[rel] = ('file', chosen)
            detail[kind].append(rel)

        if lkind == 'file' and is_binary(lcontent):
            detail['excluded_binary'].append(rel)
            continue

        if not in_base and not in_new:
            # marble-authored, or taken from pending and removed upstream
            # since. Hand edits staged over it are still manual work worth
            # keeping while the pins stand.
            cur = staged_current.get(rel)
            if (cur and cur[0] == lkind == 'file'
                    and not has_markers(cur[1]) and cur[1] != lcontent):
                if pins_same:
                    files_out[rel] = ('file', cur[1])
                    detail['manual_resolutions'].append(rel)
                    continue
                detail['stale_edits_dropped'].append(rel)
            files_out[rel] = (lkind, lcontent)
            detail['local_only'].append(rel)
            continue

        if in_new and n_mode == '120000':
            nb_target = repo.link_target(new_rev, rel)
            if lkind == 'link' and lcontent == nb_target:
                files_out[rel] = ('link', nb_target)
                detail['merged_clean'].append(rel)
            elif lkind is None:
                files_out[rel] = ('link', nb_target)
                detail['upstream_added'].append(rel)
            elif lkind == 'link':
                files_out[rel] = ('link', lcontent)
                detail['merged_clean'].append(rel)
            else:
                files_out[rel] = ('link', nb_target)
                detail['merged_clean'].append(rel)
            continue
        if link_side:
            if lkind == 'link':
                files_out[rel] = ('link', lcontent)
                detail['merged_clean'].append(rel)
            elif in_new:
                nb = get_nb()
                if is_binary(nb):
                    detail['excluded_binary'].append(rel)
                else:
                    files_out[rel] = ('file', nb)
                    detail['upstream_added'].append(rel)
            continue

        if lkind is None:
            if advancing and in_new and is_binary(get_nb()):
                detail['excluded_binary'].append(rel)
                continue
            if in_base and in_new:
                # marble deleted, keep absent — unless a staged file appeared
                cur = staged_current.get(rel)
                if (pins_same and cur and cur[0] == 'file'
                        and not has_markers(cur[1])):
                    files_out[rel] = ('file', cur[1])
                    detail['manual_resolutions'].append(rel)
                else:
                    detail['marble_removed'].append(rel)
            elif in_new:
                nb = get_nb()
                if is_binary(nb):
                    detail['excluded_binary'].append(rel)
                else:
                    take(nb, kind='upstream_added')
            continue

        # local file present
        l_sha = blob_sha(lcontent)
        if in_base and not in_new:
            if l_sha == base_tree[rel][2]:
                cur = staged_current.get(rel)
                if (pins_same and cur and cur[0] == 'file'
                        and not has_markers(cur[1])):
                    files_out[rel] = ('file', cur[1])
                    detail['manual_resolutions'].append(rel)
                else:
                    detail['deleted_upstream'].append(rel)  # untouched: delete
            else:
                cur = staged_current.get(rel)
                if (cur and cur[0] == 'file' and not has_markers(cur[1])
                        and cur[1] != lcontent):
                    if pins_same:
                        files_out[rel] = ('file', cur[1])
                        detail['manual_resolutions'].append(rel)
                        continue
                    detail['stale_edits_dropped'].append(rel)
                files_out[rel] = ('file', lcontent)
                detail['upstream_removed_kept'].append(rel)
            continue

        # ancestor: base blob, else pending blob when the file arrived via the
        # earlier partial sync, else none
        if in_base:
            anc_sha, anc_rev = base_tree[rel][2], base_rev
        elif rel in pend_tree:
            anc_sha, anc_rev = pend_tree[rel][2], pend_rev
        else:
            anc_sha = anc_rev = None
        n_sha = new_tree[rel][2] if in_new else None

        if in_new and l_sha == n_sha:
            take(lcontent)
        elif in_new and n_sha == anc_sha:
            take(lcontent)  # upstream unchanged
        elif anc_sha is not None and l_sha == anc_sha:
            nb = get_nb()
            if is_binary(nb):
                detail['excluded_binary'].append(rel)
            else:
                take(nb)  # local untouched: take upstream
        elif anc_sha is not None:
            ancestor = repo.blob(anc_rev, anc_sha, rel)
            nb = get_nb()
            if is_binary(nb):
                detail['excluded_binary'].append(rel)
                continue
            merged, n_conf = merge_text(ancestor, lcontent, nb, rel)
            if n_conf == 0 and not has_markers(merged):
                take(merged)
            else:
                cur = staged_current.get(rel)
                if (pins_same and cur and cur[0] == 'file'
                        and not has_markers(cur[1]) and cur[1] != merged):
                    files_out[rel] = ('file', cur[1])
                    detail['manual_resolutions'].append(rel)
                else:
                    if cur and cur[0] == 'file' and not has_markers(cur[1]):
                        detail['stale_edits_dropped'].append(rel)
                    files_out[rel] = ('file', merged)
                    detail['conflicts'].append(
                        {'path': rel, 'kind': 'text', 'hunks': n_conf})
        else:
            # added on both sides with different content, no ancestor
            cur = staged_current.get(rel)
            if (pins_same and cur and cur[0] == 'file'
                    and not has_markers(cur[1])):
                files_out[rel] = ('file', cur[1])
                detail['manual_resolutions'].append(rel)
            else:
                if cur and cur[0] == 'file' and not has_markers(cur[1]):
                    detail['stale_edits_dropped'].append(rel)
                files_out[rel] = ('file', lcontent)
                detail['conflicts'].append({'path': rel, 'kind': 'added_both'})

    # Scoped components: local files outside the recorded upstream scope that
    # are verbatim copies of some upstream revision are refreshed to `new`.
    if scopes:
        extra_shas = {}
        for tree in extra_sets:
            extra_shas.update(tree)
        for rel in list(detail['local_only']):
            lcontent = local[rel][1]
            if local[rel][0] != 'file':
                continue
            if rel not in any_new:
                continue  # truly marble-authored
            known = {any_new[rel][2]}
            if rel in extra_shas:
                known.add(extra_shas[rel][2])
            if blob_sha(lcontent) in known:
                cur = staged_current.get(rel)
                nblob = repo.blob(new_rev, any_new[rel][2], rel)
                if (pins_same and cur and cur[0] == 'file'
                        and not has_markers(cur[1]) and cur[1] != nblob):
                    files_out[rel] = ('file', cur[1])
                    detail['local_only'].remove(rel)
                    detail['manual_resolutions'].append(rel)
                else:
                    files_out[rel] = ('file', nblob)
                    detail['local_only'].remove(rel)
                    detail['refreshed_copy'].append(rel)
            else:
                detail['conflicts'].append(
                    {'path': rel, 'kind': 'out_of_scope_modified'})
                detail['local_only'].remove(rel)

    staged_files = {}
    for rel, (kind, content) in files_out.items():
        if kind == 'link':
            staged_files[rel] = ('link', content)
        else:
            staged_files[rel] = ('file', content)

    # Executable-bit set: from upstream/base tree mode, else from any
    # existing executable copy in the local or staged trees. os.replace()
    # would otherwise silently drop the mode on rewritten scripts.
    exec_paths = set()
    for rel, (kind, _content) in staged_files.items():
        if kind != 'file':
            continue
        tree_mode = new_tree.get(rel, base_tree.get(rel, ('',)))[0]
        if tree_mode == '100755':
            exec_paths.add(rel)
            continue
        for root in (LOCAL_ROOT / name, OUT_DIR / name):
            p = root / rel
            if p.is_file() and not p.is_symlink() and os.access(p, os.X_OK):
                exec_paths.add(rel)
                break

    if write:
        staged_root = OUT_DIR / name
        previous = set(staged_current)
        for rel in sorted(previous - set(staged_files)):
            target = staged_root / rel
            if target.is_symlink() or target.is_file():
                target.unlink()
        for rel, (kind, content) in sorted(staged_files.items()):
            dest = staged_root / rel
            if kind == 'link':
                dest.parent.mkdir(parents=True, exist_ok=True)
                if not (dest.is_symlink() and os.readlink(dest) == content):
                    if dest.is_symlink() or dest.exists():
                        dest.unlink()
                    os.symlink(content, dest)
            else:
                if not (dest.is_file() and not dest.is_symlink()
                        and dest.read_bytes() == content):
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    tmp = dest.with_name('.' + dest.name + '.stage')
                    tmp.write_bytes(content)
                    os.replace(tmp, dest)
            if kind == 'file' and rel in exec_paths:
                os.chmod(dest, 0o755)
        # drop directories left empty (prune excluded dirs from the walk)
        empty_dirs = []
        for root, dirs, _names in os.walk(staged_root):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS
                       and not (Path(root) == staged_root and d in ROOT_BUILD_DIRS)]
            empty_dirs.append(root)
        for root in reversed(empty_dirs):
            try:
                Path(root).rmdir()
            except OSError:
                pass

    unresolved = [c for c in detail['conflicts']]
    gaps = list(unresolved)
    for rel in detail['upstream_removed_kept']:
        gaps.append({'path': rel, 'kind': 'upstream_removed_local_modified'})
    for rel in detail['marble_removed']:
        gaps.append({'path': rel, 'kind': 'marble_removed_upstream_kept'})
    status = 'conflict' if unresolved or detail['upstream_removed_kept'] else 'staged'
    counts = {k: (len(v) if isinstance(v, list) else len(v)) for k, v in detail.items()}
    return {
        'name': name, 'status': status, 'files': len(staged_files),
        'repo': row['path'], 'url': origin['upstream_url'],
        'base_rev': base_rev, 'new_rev': new_rev, 'pending_rev': pend_rev,
        'origin_base_rev': origin_base_rev,
        'branch': origin.get('upstream_branch'), 'scoped': bool(scopes),
        'staged_sha256': tree_digest(staged_files),
        'counts': counts, 'detail': detail,
        'semantic_gaps': gaps,
    }


def write_markdown(report):
    lines = ['# iOS 27.2 (24B119) source integration', '',
             f"generated: {report['generated']}",
             f"status: **{report['status']}**", '',
             '| component | base | new | files | conflicts | manual | local-only | added |',
             '| --- | --- | --- | --- | --- | --- | --- | --- |']
    for c in report['components']:
        if 'counts' not in c:
            lines.append(f"| {c['name']} | - | - | 0 | gap | - | - | - |")
            continue
        d = c['counts']
        lines.append(f"| {c['name']} | `{c['base_rev'][:8]}` | `{c['new_rev'][:8]}` "
                     f"| {c['files']} | {d['conflicts']} | {d['manual_resolutions']} "
                     f"| {d['local_only']} | {d['upstream_added']} |")
    lines += ['', '## semantic gaps', '']
    if report['semantic_gaps']:
        for g in report['semantic_gaps']:
            extra = f" ({g['hunks']} hunks)" if g.get('hunks') else ''
            lines.append(f"- `{g['component']}` {g['path']}: {g['kind']}{extra}")
    else:
        lines.append('none')
    if report['pin_changes']:
        lines += ['', '## pin changes', '']
        for p in report['pin_changes']:
            lines.append(f"- {p['component']}: `{p['from'][0][:8]}`/`{p['from'][1][:8]}`"
                         f" -> `{p['to'][0][:8]}`/`{p['to'][1][:8]}`")
    (RECORD_DIR / 'source-integration.md').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='compute merges and report only; do not write staged trees')
    parser.add_argument('--component', action='append',
                        help='limit to named component(s)')
    parser.add_argument('--advance', action='store_true',
                        help='merge fetched changes into the existing staged source using its recorded upstream pin')
    args = parser.parse_args()

    SCRATCH.mkdir(parents=True, exist_ok=True)
    origins = {s['name']: s for s in
               load_json(DEVICE / 'source_origins.json')['sources']}
    revisions_by_url = {}
    for row in load_json(DEVICE / 'upstream_revisions.json')['repositories']:
        revisions_by_url.setdefault(normalize_url(row['url']), []).append(row)
    prior = load_json(RECORD_DIR / 'source-integration.json', {}) or {}
    prior_pins = {c['name']: (c.get('base_rev'), c.get('new_rev'))
                  for c in prior.get('components', [])}

    report = {'target': {'os': f'iOS{TARGET}', 'build': BUILD},
              'generated': datetime.now(timezone.utc).astimezone().isoformat(),
              'components': [], 'semantic_gaps': [], 'pin_changes': []}
    failures = 0
    for spec in COMPONENTS:
        if args.component and spec['dir'] not in args.component:
            continue
        comp = run_component(spec, origins, revisions_by_url,
                             write=not args.check,
                             prior=prior_pins.get(spec['dir']), advance=args.advance)
        report['components'].append(comp)
        old = prior_pins.get(comp['name'])
        now = (comp.get('base_rev'), comp.get('new_rev'))
        if old and all(now) and tuple(old) != now:
            report['pin_changes'].append({'component': comp['name'],
                                          'from': list(old), 'to': list(now)})
        for gap in comp.get('semantic_gaps', []):
            report['semantic_gaps'].append(dict(component=comp['name'], **gap))
        if comp['status'] != 'staged':
            failures += 1

    report['status'] = 'failed' if failures else 'staged'
    report['components_with_gaps'] = failures
    if not args.check:
        RECORD_DIR.mkdir(parents=True, exist_ok=True)
        (RECORD_DIR / 'source-integration.json').write_text(
            json.dumps(report, indent=2) + '\n')
        write_markdown(report)
    print(f"status={report['status']} components={len(report['components'])}"
          f" gaps={len(report['semantic_gaps'])}")
    for c in report['components']:
        d = c.get('counts', {})
        print(f"  {c['name']:20s} {c['status']:9s} files={c.get('files', 0):5d}"
              f" clean={d.get('merged_clean', 0):4d}"
              f" conflict={d.get('conflicts', 0):3d}"
              f" manual={d.get('manual_resolutions', 0):3d}"
              f" local={d.get('local_only', 0):3d}"
              f" added={d.get('upstream_added', 0):3d}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
