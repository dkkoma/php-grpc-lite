#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path


KNOWN_METADATA_KEYS = [
    "user-agent",
    ":authority",
    ":path",
    "grpc-timeout",
    "WaitForReady",
    "te",
    "content-type",
    ":scheme",
    ":method",
    "grpc-accept-encoding",
    "x-goog-api-client",
    "x-goog-user-project",
    "x-goog-request-params",
    "x-goog-spanner-route-to-leader",
    "google-cloud-resource-prefix",
    "authorization",
]


KEY_PATTERN = re.compile(
    r"(?:(?<=\{)|(?<=, ))("
    + "|".join(re.escape(key) for key in KNOWN_METADATA_KEYS)
    + r"): "
)


def parse_metadata(metadata: str) -> list[dict[str, int | str]]:
    matches = list(KEY_PATTERN.finditer(metadata))
    entries: list[dict[str, int | str]] = []

    for index, match in enumerate(matches):
        key = match.group(1)
        start = match.end()
        end = matches[index + 1].start() - 2 if index + 1 < len(matches) else len(metadata)
        value = metadata[start:end]
        display_value = "<redacted>" if key == "authorization" else value
        name_len = len(key.encode())
        value_len = len(value.encode())

        entries.append(
            {
                "name": key,
                "name_len": name_len,
                "value": display_value,
                "value_len": value_len,
                "bytes": name_len + value_len,
            }
        )

    return entries


def parse_trace(path: Path) -> dict[str, object]:
    lines = path.read_text(errors="replace").splitlines()
    records = []
    seen = set()

    for line in lines:
        if (
            "perform_stream_op[" not in line
            or "SEND_INITIAL_METADATA{" not in line
            or "SEND_MESSAGE:" not in line
        ):
            continue
        if "perform_stream_op_locked[" in line:
            continue

        try:
            metadata_start = line.index("SEND_INITIAL_METADATA{") + len("SEND_INITIAL_METADATA{")
            metadata_end = line.index("} SEND_MESSAGE:", metadata_start)
        except ValueError:
            continue

        message_match = re.search(r"SEND_MESSAGE:flags=0x[0-9a-fA-F]+:len=(\d+)", line)
        stream_match = re.search(r"perform_stream_op\[s=([^;\]]+)", line)
        entries = parse_metadata(line[metadata_start:metadata_end])
        path_entry = next((entry for entry in entries if entry["name"] == ":path"), None)

        message_len = int(message_match.group(1)) if message_match else None
        stream_ptr = stream_match.group(1) if stream_match else None
        rpc_path = path_entry["value"] if path_entry else None
        dedupe_key = (stream_ptr, rpc_path, message_len)
        if dedupe_key in seen:
            continue
        seen.add(dedupe_key)

        records.append(
            {
                "stream_ptr": stream_ptr,
                "path": rpc_path,
                "message_len": message_len,
                "metadata_count": len(entries),
                "metadata_name_value_bytes": sum(int(entry["bytes"]) for entry in entries),
                "entries": entries,
            }
        )

    incoming_frames = [
        line
        for line in lines
        if "INCOMING[" in line
        and any(
            frame_type in line
            for frame_type in ["SETTINGS", "WINDOW_UPDATE", "HEADERS", "DATA", "PING", "GOAWAY"]
        )
    ]
    incoming_settings = []
    for line in lines:
        setting_match = re.search(r"got setting ([A-Z_]+) = (\d+)", line)
        if setting_match:
            incoming_settings.append(
                {
                    "name": setting_match.group(1),
                    "value": int(setting_match.group(2)),
                }
            )

    return {
        "file": str(path),
        "send_initial_metadata_records": records,
        "incoming_settings": incoming_settings,
        "incoming_frame_lines": incoming_frames[:200],
        "incoming_frame_line_count": len(incoming_frames),
        "note": (
            "Stock ext-grpc/grpc-core trace exposes metadata list and incoming frame sizes, "
            "but not exact outgoing HPACK HEADERS frame payload size."
        ),
    }


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <php.err>", file=sys.stderr)
        return 2

    print(json.dumps(parse_trace(Path(sys.argv[1])), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
