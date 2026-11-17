import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import prepare_userdata_owner_policies as policies


def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


def main():
    with tempfile.TemporaryDirectory(prefix="userdata-vroot-layout-") as temporary:
        root = Path(temporary)
        raw_windows = {}
        report_windows = []
        cursor = policies.MIB
        for name, size in (("OS", 3 * 512), ("SystemOS", 4 * 512), ("App", 13 * 512)):
            path = root / (name + ".raw")
            contents = bytes([len(name)]) * size
            path.write_bytes(contents)
            report_windows.append({
                "name": name,
                "offset": cursor,
                "size": size,
                "sha256": hashlib.sha256(contents).hexdigest(),
            })
            raw_windows[name] = path
            cursor = align(cursor + size, policies.MIB)

        tables = {}
        for name, size in (("OS", 64), ("SystemOS", 96), ("App", 160)):
            path = root / (name + ".hashes")
            path.write_bytes(bytes([size]) * size)
            tables[name] = path

        output = root / "userdata-root-vroot.img"
        report = {
            "userdata_bytes": 16 * policies.MIB,
            "image_sha256": "00" * 32,
            "windows": report_windows,
        }
        policies.build_userdata(report, raw_windows, tables, output)

        expected_image = output.read_bytes()
        expected_metadata = json.loads(output.with_suffix(".json").read_text())
        expected_offsets = [window["offset"] for window in report_windows]
        cursor = align(report_windows[-1]["offset"] + report_windows[-1]["size"], policies.MIB)
        for name in ("OS", "SystemOS", "App"):
            expected_offsets.append(cursor)
            cursor = align(cursor + tables[name].stat().st_size, policies.MIB)

        header = output.read_bytes()[:4096]
        version, count, image_bytes = struct.unpack_from("<IIQ", header, 8)
        assert (version, count, image_bytes) == (2, 6, cursor)
        assert output.stat().st_size == cursor
        assert hashlib.sha256(header[:4064]).digest() == header[4064:4096]
        for index, expected_offset in enumerate(expected_offsets):
            row = struct.unpack_from("<32sQQII32s40s", header, 24 + 128 * index)
            assert row[1] == expected_offset
            assert expected_offset % policies.MIB == 0
        assert expected_metadata["image_bytes"] == cursor

        legacy_size = expected_metadata["objects"][-1]["offset"] + expected_metadata["objects"][-1]["size"]
        legacy_header = bytearray(header)
        struct.pack_into("<Q", legacy_header, 16, legacy_size)
        legacy_header[4064:4096] = hashlib.sha256(legacy_header[:4064]).digest()
        with output.open("r+b") as stream:
            stream.seek(0)
            stream.write(legacy_header)
            stream.truncate(legacy_size)
        policies.normalize_userdata_image(report, raw_windows, tables, output)
        assert output.read_bytes() == expected_image
        assert json.loads(output.with_suffix(".json").read_text())["image_bytes"] == cursor
        print("PASS: aligned SHNGROOT rows and normalized final userdata match the official layout")


if __name__ == "__main__":
    main()
