#!/usr/bin/env python3
"""Merge genuine local PSO records after validating the current arm64 descriptor ABI."""
import argparse
import collections
import csv
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import tempfile


# 用 ctypes 描述当前 C 结构布局；位域只占其已声明的整型存储，不重新解析 C 源码。
# Describe the current C layout with ctypes; bitfields occupy their declared integer storage, without parsing C source.
class Stream(ctypes.LittleEndianStructure):
    _fields_ = [('stride', ctypes.c_uint32), ('divisor', ctypes.c_uint32)]


class PipelineKey(ctypes.LittleEndianStructure):
    _fields_ = [('vs', ctypes.c_uint64), ('fs', ctypes.c_uint64), ('cfmt', ctypes.c_uint32 * 8),
                ('dfmt', ctypes.c_uint32), ('sfmt', ctypes.c_uint32), ('samples', ctypes.c_uint32),
                ('blend_en', ctypes.c_uint32), ('chmask', ctypes.c_uint32), ('a2c', ctypes.c_uint32),
                ('blend', ctypes.c_uint64 * 8), ('attr', ctypes.c_uint32 * 16), ('stream', Stream * 16),
                ('lmap', ctypes.c_uint8 * 8), ('fsfn', ctypes.c_uint64)]


class Specialization(ctypes.LittleEndianStructure):
    _fields_ = [('kind', ctypes.c_uint8), ('fbmask', ctypes.c_uint8), ('fx', ctypes.c_int8 * 32)]


class RecordedKey(ctypes.LittleEndianStructure):
    _fields_ = [('vs', ctypes.c_char * 24), ('fs', ctypes.c_char * 24),
                ('vs_stage', ctypes.c_uint8), ('fs_stage', ctypes.c_uint8),
                ('merged', ctypes.c_uint8), ('fz', ctypes.c_uint8),
                ('fz_desc', Specialization), ('k', PipelineKey)]


EXPECTED_HEADERS = {
    'nvn_pso_skey.h': '17a07f06bf8904803a28b062118cbb8f3ba45325a4e462b60cd80375f5efd2d5',
    'nvn_obj.h': 'f4a1937d2f577a5d4c2c67fd13022ceaa9cabb4550b19ea0d78d94a534b95696',
}
STAGES = {'顶点': 0, '像素': 1, '外壳': 3, '域': 4, '计算': 5}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def profile_stages(path):
    result = {}
    with path.open(encoding='utf-8', newline='') as source:
        reader = csv.DictReader((line for line in source if not line.startswith('#')), delimiter='\t')
        if reader.fieldnames != ['package', 'index', 'stage', 'dm_ctl_sha16', 'source']:
            raise ValueError('Unexpected current shader-map columns')
        for row in reader:
            key, stage = row['dm_ctl_sha16'], STAGES[row['stage']]
            if not re.fullmatch('[0-9a-f]{16}', key):
                raise ValueError('Invalid current shader-map key')
            name = key + '_b'
            if name in result and result[name] != stage:
                raise ValueError('Conflicting stages for current shader-map key')
            result[name] = stage
    return result


def key_name(raw):
    # 持久化名称应为零终止且尾部清零，避免旧 ABI 字节被误读为当前名称。
    # Persisted names must be zero terminated and padded, preventing old ABI bytes from masquerading as current names.
    head, separator, tail = raw.partition(b'\0')
    if not separator or any(tail):
        raise ValueError('Incompatible fixed shader-name field')
    return head.decode('ascii')


def incompatibility(row, stages):
    if row[0] in ('S', 'C'):
        name = row[1]
        stage = int(row[2]) if row[0] == 'S' else 5
        return None if stages.get(name) == stage else 'shader key/stage absent from current profile'
    raw = bytes.fromhex(row[1])
    if len(raw) != ctypes.sizeof(RecordedKey):
        return 'render descriptor length differs from current ABI'
    record = RecordedKey.from_buffer_copy(raw)
    vs, fs = key_name(raw[:24]), key_name(raw[24:48])
    if stages.get(vs) != 0 or record.vs_stage != 0:
        return 'render vertex key/stage incompatible with current profile'
    if fs and (stages.get(fs) != 1 or record.fs_stage != 1):
        return 'render fragment key/stage incompatible with current profile'
    if record.k.vs or record.k.fs or record.k.fsfn:
        return 'persisted runtime pointer is nonzero'
    if record.merged not in (0, 1) or record.fz not in (0, 1):
        return 'invalid render descriptor flags'
    if record.fz and record.fz_desc.kind != 1:
        return 'specialization kind unsupported by current warm worker'
    if record.k.samples not in (1, 2, 4, 8, 16) or record.k.a2c not in (0, 1):
        return 'invalid sample count or alpha-to-coverage flag'
    return None


def load_manifest(path, stages):
    data = path.read_bytes()
    rows, rejected = set(), collections.Counter()
    for line, row in enumerate(csv.reader(io.StringIO(data.decode('utf-8'), newline=''), delimiter='\t'), 1):
        if not row or row[0].startswith('#'):
            continue
        if row[0] not in ('S', 'R', 'C') or len(row) != (3 if row[0] == 'S' else 2):
            raise ValueError(f'Malformed manifest columns: {path}:{line}')
        if row[0] == 'R':
            if not re.fullmatch('[0-9a-fA-F]+', row[1]):
                raise ValueError(f'Malformed descriptor hex: {path}:{line}')
            row[1] = bytes.fromhex(row[1]).hex()
        elif row[0] == 'S':
            row[2] = str(int(row[2]))
        reason = incompatibility(row, stages)
        if reason:
            rejected[reason] += 1
        rows.add(tuple(row))
    return rows, {'path': str(path), 'sha256': digest(data), 'bytes': len(data), 'unique_records': len(rows),
                  'kinds': dict(collections.Counter(row[0] for row in rows)), 'incompatibilities': dict(rejected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--provenance', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    headers = {name: digest((repo / 'dm/gfx/nvn' / name).read_bytes()) for name in EXPECTED_HEADERS}
    if headers != EXPECTED_HEADERS:
        raise ValueError('Current ABI headers changed; review ctypes layout before merging')
    if ctypes.sizeof(PipelineKey) != 344 or ctypes.sizeof(RecordedKey) != 432 or RecordedKey.k.offset != 88:
        raise ValueError('Unexpected local ctypes ABI layout')
    stages = profile_stages(repo / 'dm/games/smbw/shader_map.tsv')
    run = args.workspace.resolve() / 'run'
    target = run / 'pso_user.tsv'
    original = target.read_bytes()
    union, files = set(), []
    for path in sorted(run.rglob('pso_user*.tsv')):
        rows, info = load_manifest(path, stages)
        # 一份来源只要发现旧布局或无效引用就整份剔除；格式错误则立即失败，不吞掉。
        # Exclude an entire source with incompatible layout/references; malformed formats fail immediately, never silently disappear.
        info['included'] = not info['incompatibilities']
        if info['included']:
            union.update(rows)
        files.append(info)
    if not union:
        raise ValueError('No compatible genuine records to merge')
    output = io.StringIO(newline='')
    writer = csv.writer(output, delimiter='\t', lineterminator='\n')
    writer.writerows(sorted(union))
    merged = output.getvalue().encode('utf-8')
    args.backup.parent.mkdir(parents=True, exist_ok=True)
    if not args.backup.exists():
        shutil.copy2(target, args.backup)
    if target.read_bytes() != original:
        raise RuntimeError('Shared manifest changed during validation; refusing to overwrite')
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.pso-union-', delete=False) as temp:
        temporary = Path(temp.name)
        temp.write(merged)
        temp.flush()
        os.fsync(temp.fileno())
    os.chmod(temporary, target.stat().st_mode & 0o777)
    os.replace(temporary, target)
    report = {'target': str(target), 'backup': str(args.backup.resolve()),
              'before_sha256': digest(original), 'backup_sha256': digest(args.backup.read_bytes()),
              'after_sha256': digest(merged), 'union_records': len(union),
              'union_kinds': dict(collections.Counter(row[0] for row in union)),
              'abi': {'header_sha256': headers, 'pso_key_bytes': 344, 'pso_skey_bytes': 432, 'k_offset': 88,
                      'source': 'Descriptor layouts match fe9bab9; reviewed nvn_obj.h change only adds period_ns to mtl_link_wait_tick and its comments, outside descriptor types'},
              'files': files,
              'semantics': 'Exact set union of accepted real records, no synthesized PSOs. Old chains are not cached binaries: current keys/stages and descriptor ABI are validated, and current runtime rebuilds objects. No GPU or compile validation was run.'}
    args.provenance.parent.mkdir(parents=True, exist_ok=True)
    args.provenance.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'union_records': len(union), 'kinds': report['union_kinds'],
                      'included_files': sum(info['included'] for info in files),
                      'excluded_files': sum(not info['included'] for info in files),
                      'provenance': str(args.provenance.resolve())}, ensure_ascii=False))


if __name__ == '__main__':
    main()
