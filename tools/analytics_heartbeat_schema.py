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
