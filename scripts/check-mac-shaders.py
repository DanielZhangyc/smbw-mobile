#!/usr/bin/env python3
"""Read-only validation of a complete native Mac shader cache against its map."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re
import sys

STAGES = {"顶点": "vs", "像素": "ps", "计算": "cs", "几何": "gs", "外壳": "hs", "域": "ds"}


class CacheError(Exception):
    pass


def unique_object(pairs):
    # 拒绝 JSON 重复字段，避免后一个值静默覆盖校验依据。
    # Reject duplicate JSON members rather than silently overriding validation evidence.
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("Duplicate JSON member")
        result[name] = value
    return result


def read_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=unique_object)


def read_map(path):
    expected = {}
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.reader((line for line in source if not line.startswith("#")), delimiter="\t", strict=True)
        if next(reader, None) != ["package", "index", "stage", "dm_ctl_sha16", "source"]:
            raise ValueError("Shader map has an invalid header")
        for number, row in enumerate(reader, 2):
            if len(row) != 5 or any(not field for field in row):
                raise ValueError(f"Malformed shader map row {number}")
            _, index, label, key, _ = row
            if not index.isdecimal() or label not in STAGES or not re.fullmatch(r"[0-9a-f]{16}", key):
                raise ValueError(f"Invalid shader map field at row {number}")
            stage = STAGES[label]
            if key in expected and expected[key] != stage:
                raise ValueError(f"Conflicting stages in shader map at row {number}")
            expected[key] = stage
    if not expected:
        raise ValueError("Shader map contains no shaders")
    return expected


def validate(cache, expected, chain):
    if cache.name != "b-" + chain:
        raise CacheError("Cache directory does not match the expected chain")
    records = read_json(cache / "_b_build.json")
    if not isinstance(records, list):
        raise ValueError("Build manifest must be a JSON array")
    seen = set()
    failures = Counter()
    for row in records:
        if not isinstance(row, dict) or not isinstance(row.get("key"), str) or not isinstance(row.get("st"), str):
            raise ValueError("Invalid build-manifest record")
        key = row["key"]
        if not re.fullmatch(r"[0-9a-f]{16}", key):
            raise ValueError("Invalid shader identity in build manifest")
        if key in seen:
            raise CacheError("Duplicate shader identity in build manifest")
        seen.add(key)
        if row["st"] != "ok":
            failures[row["st"]] += 1
    if seen != set(expected):
        raise CacheError(f"Build-manifest coverage mismatch: missing={len(set(expected) - seen)}, unexpected={len(seen - set(expected))}")
    if failures:
        # 只报告失败数量，不打印游戏来源、原始反射或身份键。
        # Report failure counts only, without game sources, raw reflection, or identity keys.
        raise CacheError(f"Build manifest contains {sum(failures.values())} failed shaders; validation stopped")
    for key, stage in expected.items():
        reflection = read_json(cache / (key + "_b.json"))
        if not isinstance(reflection, dict):
            raise ValueError("Shader reflection must be a JSON object")
        identity = {"key": key + "_b", "dm_ctl": key, "chain": chain, "stage": stage, "route": "B"}
        if any(reflection.get(name) != value for name, value in identity.items()):
            raise CacheError("Shader reflection identity, route, chain or stage mismatch")
        library = cache / (key + "_b.metallib")
        with library.open("rb") as stream:
            if stream.read(4) != b"MTLB" or library.stat().st_size <= 4:
                raise CacheError("Shader library is empty or lacks compiled Metal MTLB magic")
    return Counter(expected.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True, help="Complete b-<chain> directory")
    parser.add_argument("--map", type=Path, required=True, dest="shader_map")
    parser.add_argument("--expected-chain", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{12}", args.expected_chain):
        parser.error("Expected chain must contain 12 lowercase hexadecimal characters")
    try:
        expected = read_map(args.shader_map)
        stages = validate(args.cache, expected, args.expected_chain)
    except CacheError as error:
        print(f"Shader cache invalid: {error}", file=sys.stderr)
        return 1
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        # 不打印包含游戏身份键的文件路径或解析异常内容。
        # Do not print file paths or parser exception contents that might contain game identity keys.
        print(f"Shader input unreadable or malformed ({type(error).__name__}); validation stopped", file=sys.stderr)
        return 2
    print(f"Shader cache valid: {len(expected)} shaders; " + ", ".join(f"{stage}={count}" for stage, count in sorted(stages.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
