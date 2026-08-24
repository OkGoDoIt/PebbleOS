# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import re
from dataclasses import dataclass
from typing import Optional


HEADER_FIELDS = (
    ("version", 1),
    ("timestamp", 8),
    ("build_id", 20),
)

_METRIC_RE = re.compile(r"^PBL_ANALYTICS_METRIC_DEFINE_([A-Z_]+)\((.*)\)$")


@dataclass(frozen=True)
class Field:
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class Metric:
    kind: str
    name: str
    arg: Optional[int] = None


def _strip_comment(line):
    return line.split("//", 1)[0].strip()


def parse_analytics_def(path):
    metrics = []
    with open(path, encoding="utf-8") as f:
        for line_no, raw_line in enumerate(f, 1):
            line = _strip_comment(raw_line)
            if not line or line.startswith("/*") or line.startswith("*"):
                continue

            match = _METRIC_RE.match(line)
            if not match:
                raise ValueError(f"{path}:{line_no}: unsupported analytics.def line: {raw_line.rstrip()}")

            kind, args = match.groups()
            parts = [p.strip() for p in args.split(",")]
            if kind in ("UNSIGNED", "SIGNED", "TIMER"):
                if len(parts) != 1:
                    raise ValueError(f"{path}:{line_no}: expected one argument for {kind}")
                metrics.append(Metric(kind, parts[0]))
            elif kind in ("SCALED_UNSIGNED", "SCALED_SIGNED", "STRING"):
                if len(parts) != 2:
                    raise ValueError(f"{path}:{line_no}: expected two arguments for {kind}")
                metrics.append(Metric(kind, parts[0], int(parts[1], 0)))
            else:
                raise ValueError(f"{path}:{line_no}: unsupported metric kind: {kind}")
    return metrics


def native_heartbeat_layout(metrics):
    fields = []
    offset = 0

    for name, size in HEADER_FIELDS:
        fields.append(Field(name, offset, size))
        offset += size

    for metric in metrics:
        name = f"metric_{metric.name}"
        if metric.kind in ("UNSIGNED", "SIGNED", "TIMER"):
            fields.append(Field(name, offset, 4))
            offset += 4
        elif metric.kind in ("SCALED_UNSIGNED", "SCALED_SIGNED"):
            fields.append(Field(name, offset, 4))
            offset += 4
            fields.append(Field(f"{name}_scale", offset, 2))
            offset += 2
        elif metric.kind == "STRING":
            fields.append(Field(name, offset, metric.arg + 1))
            offset += metric.arg + 1
        else:
            raise ValueError(f"unsupported metric kind: {metric.kind}")

    return fields


def field_offsets(fields):
    return {field.name: field.offset for field in fields}


def wire_size(fields):
    if not fields:
        return 0
    last = fields[-1]
    return last.offset + last.size


_DEFINE_RE = re.compile(r"^#define\s+(NATIVE_HEARTBEAT_[A-Z0-9_]+)\s+(\S+)")


def parse_native_wire_constants(path):
    """Resolve the NATIVE_HEARTBEAT_* #defines in native.c to ints."""
    raw = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            match = _DEFINE_RE.match(line)
            if match:
                raw[match.group(1)] = match.group(2)

    resolved = {}
    for name, value in raw.items():
        seen = set()
        while value in raw and value not in seen:
            seen.add(value)
            value = raw[value]
        resolved[name] = int(value, 0)
    return resolved


_DLS_CREATE_RE = re.compile(
    r"dls_create\s*\(\s*DlsSystemTagAnalyticsNativeHeartbeat\s*,(?P<args>.*?)\)\s*;",
    re.DOTALL,
)


def parse_heartbeat_dls_create_buffered(path):
    """Return the `buffered` argument of native.c's heartbeat dls_create call as a bool."""
    with open(path, encoding="utf-8") as f:
        match = _DLS_CREATE_RE.search(f.read())
    if not match:
        raise ValueError("no dls_create(DlsSystemTagAnalyticsNativeHeartbeat, ...) call found")
    # Args after the tag: item_type, item_size, buffered, resume, uuid.
    args = [a.strip() for a in match.group("args").split(",")]
    if len(args) != 5 or args[2] not in ("true", "false"):
        raise ValueError(f"unexpected dls_create argument shape: {args}")
    return args[2] == "true"


def parse_dls_session_size_caps(dls_private_h_path, comm_protocol_h_path):
    """Return (max_buffered_item_size, max_unbuffered_item_size) from the DLS headers."""
    with open(dls_private_h_path, encoding="utf-8") as f:
        dls_src = f.read()
    match = re.search(r"DLS_SESSION_MAX_BUFFERED_ITEM_SIZE\s*=\s*(\d+)", dls_src)
    if not match:
        raise ValueError("DLS_SESSION_MAX_BUFFERED_ITEM_SIZE not found")
    max_buffered = int(match.group(1))

    with open(comm_protocol_h_path, encoding="utf-8") as f:
        comm_src = f.read()
    match = re.search(r"#define\s+COMM_MAX_OUTBOUND_PAYLOAD_SIZE\s+(\d+)", comm_src)
    if not match:
        raise ValueError("COMM_MAX_OUTBOUND_PAYLOAD_SIZE not found")
    # DLS_ENDPOINT_MAX_PAYLOAD = COMM_MAX_OUTBOUND_PAYLOAD_SIZE - sizeof(DataLoggingSendDataMessage)
    # (command u8 + session_id u8 + items_left u32 + crc32 u32 = 10 bytes of header).
    max_unbuffered = int(match.group(1)) - 10
    return max_buffered, max_unbuffered
