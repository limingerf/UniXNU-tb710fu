#!/usr/bin/env python3
"""Verify a built userdata image, sign its pinned windows, and add VROOT tables."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tomllib
import uuid
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PROFILE = "macos-27.3-26C5035r"
MIB = 1 << 20
CHUNK = 8 << 20


def sha_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(CHUNK), b""):
            digest.update(data)
    return digest.hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(args):
    print("+ " + " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def write_zeroes(stream, count, digest):
    zeroes = bytes(CHUNK)
    while count:
        size = min(count, len(zeroes))
        block = zeroes[:size]
        stream.write(block)
        digest.update(block)
        count -= size


def extract_windows(archive, image_entry, report, output):
    paths = {window["name"]: output / (window["name"] + ".raw")
             for window in report["windows"]}
    if set(paths) != {"OS", "SystemOS", "App"}:
        raise SystemExit("Expected exactly OS, SystemOS and App windows")
    for path in paths.values():
        if path.exists():
            raise SystemExit(f"Refusing to overwrite extracted window: {path}")

    full_hash = hashlib.sha256()
    window_hashes = {name: hashlib.sha256() for name in paths}
    window_bytes = {name: 0 for name in paths}
    ordered = sorted(report["windows"], key=lambda window: window["offset"])
    streams = {name: path.open("xb") for name, path in paths.items()}
    try:
        with zipfile.ZipFile(archive) as bundle, bundle.open(image_entry) as image:
            position = 0
            while True:
                block = image.read(CHUNK)
                if not block:
                    break
                full_hash.update(block)
                block_end = position + len(block)
                for window in ordered:
                    start = window["offset"]
                    end = start + window["size"]
                    left, right = max(position, start), min(block_end, end)
                    if left < right:
                        piece = block[left - position:right - position]
                        name = window["name"]
                        streams[name].write(piece)
                        window_hashes[name].update(piece)
                        window_bytes[name] += len(piece)
                position = block_end
                if position % (1 << 30) < CHUNK:
                    print(f"Verified source image stream: {position} bytes", flush=True)
    finally:
        for stream in streams.values():
            stream.close()
    if position != report["image_bytes"] or full_hash.hexdigest() != report["image_sha256"]:
        raise SystemExit("The downloaded root image does not match userdata-root.json")
    for window in ordered:
        name = window["name"]
        if (window_bytes[name] != window["size"] or
                window_hashes[name].hexdigest() != window["sha256"]):
            raise SystemExit(f"{name}: extracted window size/hash differs from the built manifest")
        print(f"Verified {name}: {window_bytes[name]} bytes, sha256 {window_hashes[name].hexdigest()}", flush=True)
    return paths


def compile_host_verifier(source, tree):
    package = source / "kits/VerifiedRoot"
    host = tree / "build/verified-root-host"
    host.mkdir(parents=True, exist_ok=True)
    library = host / "verified-root.so"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                          "-I", str(package / "include"), str(package / "src/verified_root.c"),
                          str(package / "tests/openssl_provider.c"), "-lcrypto", "-o", str(library)]
    run(command)
    return host, library


def protect_owner_path(path, directory=False):
    path.chmod(0o700 if directory else 0o600)
    if stat.S_IMODE(path.stat().st_mode) & 0o077 == 0:
        return

    icacls = shutil.which("icacls.exe") or shutil.which("icacls")
    wslpath = shutil.which("wslpath")
    whoami = shutil.which("whoami.exe") or shutil.which("whoami")
    if not icacls or not whoami or (os.name != "nt" and not wslpath):
        raise SystemExit(f"Cannot enforce private owner-key ACL on {path}")
    native_path = (subprocess.check_output([wslpath, "-w", str(path)], text=True).strip()
                   if wslpath else str(path))
    identity = subprocess.check_output([whoami, "/user", "/fo", "csv", "/nh"])
    match = re.search(rb"S-1-\d+(?:-\d+)+", identity)
    if not match:
        raise SystemExit("Cannot resolve the current Windows user SID for owner-key ACL protection")
    identity = "*" + match.group(0).decode("ascii")
    permission = "(OI)(CI)(F)" if directory else "(F)"
    subprocess.run([icacls, native_path, "/inheritance:r"], check=True, capture_output=True)
    subprocess.run([icacls, native_path, "/grant:r", f"{identity}:{permission}",
                    f"*S-1-5-18:{permission}", f"*S-1-5-32-544:{permission}"],
                   check=True, capture_output=True)


def ensure_owner_key(host):
    keys = host / "keys"
    keys.mkdir(mode=0o700, parents=True, exist_ok=True)
    protect_owner_path(keys, directory=True)
    private, public = keys / "owner.pem", keys / "owner-public.pem"
    if private.exists() != public.exists():
        raise SystemExit("Owner key pair is incomplete; preserve it and repair the matching key")
    if not private.exists():
        old_mask = os.umask(0o077)
        try:
            run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072",
                 "-out", private])
        finally:
            os.umask(old_mask)
        run(["openssl", "pkey", "-in", private, "-pubout", "-out", public])
    protect_owner_path(private)
    return private, public


def generate_policies(source, tree, windows, policies, library):
    root = source / "profiles/os" / PROFILE / "profile.toml"
    profile = tomllib.loads(root.read_text())
    expected = profile["root"]
    manifest_tool = source / "kits/VerifiedRoot/tools/manifest.py"
    verifier_tool = source / "kits/VerifiedRoot/tools/verify_image.py"
    policies.mkdir(parents=True, exist_ok=False)
    key_directory = tree / "build/verified-root-host/keys"
    private, public = ensure_owner_key(key_directory.parent)
    anchors = set()

    for name in ("OS", "SystemOS", "App"):
        image = windows[name]
        uuid_value = expected[name.lower() + "_volume_uuid"].upper()
        expected_sha = expected[name.lower() + "_image_sha256"]
        destination = tree / f"build/verified-root-{name}"
        if destination.exists():
            raise SystemExit(f"Refusing to overwrite existing root policy build output: {destination}")
        run([sys.executable, manifest_tool, "--image", image, "--private-key", private,
             "--public-key", public, "--volume-uuid", uuid_value, "--generation", "1",
             "--output", destination])
        metadata = destination / "verification.json"
        run([sys.executable, verifier_tool, "--library", library,
             "--manifest", destination / "manifest.bin", "--trusted-key", destination / "trusted-key.bin",
             "--image", image, "--volume-uuid", uuid_value, "--minimum-generation", "1",
             "--output", metadata])
        verified = json.loads(metadata.read_text())
        if verified["image_sha256"] != expected_sha or verified["volume_uuid"] != uuid_value:
            raise SystemExit(f"{name}: verified image identity differs from the selected OS profile")
        envelope = (destination / "manifest.bin").read_bytes()
        if len(envelope) != 544 + 32 * verified["blocks_verified"]:
            raise SystemExit(f"{name}: unexpected signed manifest size")
        policy_dir = policies / name
        policy_dir.mkdir()
        (policy_dir / "manifest.header.bin").write_bytes(envelope[:544])
        shutil.copyfile(destination / "trusted-key.bin", policy_dir / "trusted-key.bin")
        shutil.copyfile(metadata, policy_dir / "verification.json")
        anchors.add(hashlib.sha256((policy_dir / "trusted-key.bin").read_bytes()).hexdigest())
        print(f"Policy {name}: {verified['blocks_verified']} verified blocks, sha256 {verified['image_sha256']}", flush=True)

    if len(anchors) != 1:
        raise SystemExit("All root windows must share one owner public key")
    return {name: tree / f"build/verified-root-{name}/hashes.bin" for name in windows}


def hash_object(path):
    return sha_file(path)


def build_userdata(report, windows, tables, output):
    if output.exists():
        raise SystemExit(f"Refusing to overwrite userdata output: {output}")
    userdata_capacity = report["userdata_bytes"]
    cursor = MIB
    rows = []
    ordered_windows = sorted(report["windows"], key=lambda item: item["offset"])
    for window in ordered_windows:
        if cursor != window["offset"]:
            raise SystemExit("The original image layout is not canonical SHNGROOT v1 order")
        rows.append((window["name"], window["size"], 512, 1, bytes.fromhex(window["sha256"]), windows[window["name"]]))
        cursor = ((window["offset"] + window["size"] + MIB - 1) // MIB) * MIB
    for name in ("OS", "SystemOS", "App"):
        table = tables[name]
        digest = hashlib.sha256()
        with table.open("rb") as stream:
            for block in iter(lambda: stream.read(CHUNK), b""):
                digest.update(block)
        size = table.stat().st_size
        rows.append((name + ".vroot", size, 4096, 3, digest.digest(), table))
        cursor = ((cursor + size + MIB - 1) // MIB) * MIB
    total = cursor
    if total > userdata_capacity or len(rows) > 8:
        raise SystemExit("Policy tables exceed userdata capacity or SHNGROOT row limits")

    header = bytearray(4096)
    header[:8] = b"SHNGROOT"
    struct.pack_into("<IIQ", header, 8, 2, len(rows), total)
    offsets = []
    cursor = MIB
    for index, (name, size, block_size, flags, digest, _) in enumerate(rows):
        if index < len(ordered_windows):
            expected_offset = ordered_windows[index]["offset"]
            if cursor != expected_offset:
                raise SystemExit(f"{name}: computed offset differs from the original window layout")
            cursor = expected_offset
        else:
            cursor = ((cursor + MIB - 1) // MIB) * MIB
        offsets.append(cursor)
        struct.pack_into("<32sQQII32s40s", header, 24 + 128 * index,
                         name.encode("ascii"), cursor, size, block_size, flags, digest, bytes(40))
        cursor += size
        cursor = ((cursor + MIB - 1) // MIB) * MIB
    header[4064:4096] = hashlib.sha256(header[:4064]).digest()

    digest = hashlib.sha256()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as destination:
        destination.write(header)
        digest.update(header)
        cursor = len(header)
        for index, row in enumerate(rows):
            name, size, _, _, _, source = row
            target_offset = offsets[index]
            write_zeroes(destination, target_offset - cursor, digest)
            with source.open("rb") as stream:
                written = 0
                row_digest = hashlib.sha256()
                while True:
                    block = stream.read(CHUNK)
                    if not block:
                        break
                    destination.write(block)
                    digest.update(block)
                    row_digest.update(block)
                    written += len(block)
                if written != size or row_digest.digest() != row[4]:
                    raise SystemExit(f"{name}: source size or hash changed while writing userdata")
            cursor = target_offset + size
        write_zeroes(destination, total - cursor, digest)
    if output.stat().st_size != total:
        raise SystemExit("Rebuilt userdata image size mismatch")
    with output.open("rb") as stream:
        written_header = stream.read(len(header))
    if (written_header != header or written_header[:8] != b"SHNGROOT" or
            hashlib.sha256(written_header[:4064]).digest() != written_header[4064:4096]):
        raise SystemExit("Rebuilt userdata manifest read-back failed")
    for index, row in enumerate(rows):
        name, size, block_size, flags, row_hash, _ = row
        stored_name, stored_offset, stored_size, stored_block, stored_flags, stored_hash, _ = struct.unpack_from(
            "<32sQQII32s40s", written_header, 24 + 128 * index)
        if (stored_name.split(b"\0", 1)[0].decode("ascii") != name or
                (stored_offset, stored_size, stored_block, stored_flags, stored_hash) !=
                (offsets[index], size, block_size, flags, row_hash) or stored_offset % MIB):
            raise SystemExit(f"{name}: rebuilt userdata manifest row failed read-back")
    result = {
        "image_bytes": total,
        "image_sha256": digest.hexdigest(),
        "manifest_sha256": hashlib.sha256(header).hexdigest(),
        "windows": report["windows"],
        "objects": [
            {"name": row[0], "offset": offsets[index], "size": row[1],
             "sha256": row[4].hex()}
            for index, row in enumerate(rows) if index >= 3
        ],
        "userdata_bytes": userdata_capacity,
        "hardware_tested": False,
        "source_run_id": 43724852936,
        "source_image_sha256": report["image_sha256"],
        "flash_command": f"fastboot flash userdata {output}",
    }
    output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


def normalize_userdata_image(report, windows, tables, image):
    if not image.is_file():
        raise SystemExit(f"Existing userdata image is missing: {image}")
    ordered_windows = sorted(report["windows"], key=lambda item: item["offset"])
    rows = []
    cursor = MIB
    for window in ordered_windows:
        if cursor != window["offset"]:
            raise SystemExit("The existing image windows do not match the canonical SHNGROOT layout")
        rows.append((window["name"], window["size"], 512, 1,
                     bytes.fromhex(window["sha256"]), windows[window["name"]]))
        cursor = ((window["offset"] + window["size"] + MIB - 1) // MIB) * MIB
    for name in ("OS", "SystemOS", "App"):
        table = tables[name]
        size = table.stat().st_size
        rows.append((name + ".vroot", size, 4096, 3, bytes.fromhex(sha_file(table)), table))
        cursor = ((cursor + size + MIB - 1) // MIB) * MIB
    total = cursor
    old_total = cursor - ((rows[-1][1] + MIB - 1) // MIB) * MIB + rows[-1][1]
    if total > report["userdata_bytes"] or total - old_total >= MIB:
        raise SystemExit("The normalized userdata size exceeds its partition or padding bounds")

    with image.open("rb") as stream:
        header = bytearray(stream.read(4096))
    if (len(header) != 4096 or header[:8] != b"SHNGROOT" or
            hashlib.sha256(header[:4064]).digest() != header[4064:4096]):
        raise SystemExit("Existing userdata manifest is invalid")
    version, count, declared_size = struct.unpack_from("<IIQ", header, 8)
    if (version, count) != (2, len(rows)) or declared_size not in (old_total, total):
        raise SystemExit("Existing userdata manifest version, row count or size is unexpected")

    offsets = []
    cursor = MIB
    for index, row in enumerate(rows):
        name, size, block_size, flags, row_hash, _ = row
        if index < len(ordered_windows):
            target_offset = ordered_windows[index]["offset"]
            if cursor != target_offset:
                raise SystemExit(f"{name}: existing window offset differs from the pinned userdata layout")
            cursor = target_offset
        else:
            cursor = ((cursor + MIB - 1) // MIB) * MIB
        offsets.append(cursor)
        stored_name, stored_offset, stored_size, stored_block, stored_flags, stored_hash, _ = struct.unpack_from(
            "<32sQQII32s40s", header, 24 + 128 * index)
        if (stored_name.split(b"\0", 1)[0].decode("ascii") != name or
                (stored_offset, stored_size, stored_block, stored_flags, stored_hash) !=
                (cursor, size, block_size, flags, row_hash)):
            raise SystemExit(f"{name}: existing userdata row differs from its verified source")
        cursor = ((cursor + size + MIB - 1) // MIB) * MIB
    raw_end = offsets[-1] + rows[-1][1]
    if declared_size == old_total and raw_end != old_total:
        raise SystemExit("Existing userdata tail does not match the expected pre-normalized size")
    if image.stat().st_size not in (old_total, total):
        raise SystemExit("Existing userdata file size does not match its manifest")

    if image.stat().st_size == old_total and total > old_total:
        with image.open("ab") as stream:
            remaining = total - old_total
            zeroes = bytes(min(CHUNK, remaining))
            while remaining:
                size = min(remaining, len(zeroes))
                stream.write(zeroes[:size])
                remaining -= size
    struct.pack_into("<Q", header, 16, total)
    header[4064:4096] = hashlib.sha256(header[:4064]).digest()
    with image.open("r+b") as stream:
        stream.seek(0)
        stream.write(header)

    zero_ranges = []
    cursor = len(header)
    for offset, row in zip(offsets, rows):
        if cursor < offset:
            zero_ranges.append((cursor, offset))
        cursor = offset + row[1]
    if cursor < total:
        zero_ranges.append((cursor, total))
    row_hashes = [hashlib.sha256() for _ in rows]
    whole_hash = hashlib.sha256()
    position = 0
    with image.open("rb") as stream:
        while True:
            block = stream.read(CHUNK)
            if not block:
                break
            block_end = position + len(block)
            whole_hash.update(block)
            for index, (offset, row) in enumerate(zip(offsets, rows)):
                left, right = max(position, offset), min(block_end, offset + row[1])
                if left < right:
                    row_hashes[index].update(block[left - position:right - position])
            for start, end in zero_ranges:
                left, right = max(position, start), min(block_end, end)
                if left < right and any(block[left - position:right - position]):
                    raise SystemExit("Existing userdata alignment padding is not zero-filled")
            position = block_end
            if position % (1 << 30) < CHUNK:
                print(f"Read back normalized userdata: {position} bytes", flush=True)
    if position != total or any(hasher.digest() != row[4] for hasher, row in zip(row_hashes, rows)):
        raise SystemExit("Normalized userdata read-back size or row hash failed")

    result = {
        "image_bytes": total,
        "image_sha256": whole_hash.hexdigest(),
        "manifest_sha256": hashlib.sha256(header).hexdigest(),
        "windows": report["windows"],
        "objects": [
            {"name": row[0], "offset": offsets[index], "size": row[1], "sha256": row[4].hex()}
            for index, row in enumerate(rows) if index >= len(ordered_windows)
        ],
        "userdata_bytes": report["userdata_bytes"],
        "hardware_tested": False,
        "source_run_id": 43724852936,
        "source_image_sha256": report["image_sha256"],
        "flash_command": f"fastboot flash userdata {image}",
    }
    image.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / ".work/UniXNU")
    parser.add_argument("--tree", type=Path)
    parser.add_argument("--run-id", type=int, default=43724852936)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--rebuild-only", action="store_true",
                      help="rebuild userdata from already extracted windows and verified policy tables")
    mode.add_argument("--normalize-existing", action="store_true",
                      help="add official tail padding and verify an already built userdata image")
    parser.add_argument("--output", type=Path,
                        help="new userdata image path (used with --rebuild-only; never overwritten)")
    args = parser.parse_args()
    if args.output and not args.rebuild_only:
        parser.error("--output can only be used with --rebuild-only")
    source = args.source.resolve()
    resolved = Path(args.tree).resolve() if args.tree else Path((ROOT / ".work/resolved-work.txt").read_text().strip()) / "tree"
    profile_path = source / "profiles/os" / PROFILE / "profile.toml"
    report_name = "userdata-root.json"
    image_name = "userdata-root.img"
    output = ROOT / ".work" / f"root-policy-run-{args.run_id}"
    if args.rebuild_only or args.normalize_existing:
        if not output.is_dir():
            raise SystemExit(f"Existing policy preparation directory is missing: {output}")
        with zipfile.ZipFile(args.archive) as bundle:
            if report_name not in bundle.namelist():
                raise SystemExit("Artifact ZIP does not contain userdata-root.json")
            report = json.loads(bundle.read(report_name))
        profile = tomllib.loads(profile_path.read_text())
        roots = profile["root"]
        windows_by_name = {window["name"]: window for window in report["windows"]}
        windows = {}
        for window in report["windows"]:
            name = window["name"]
            expected = roots[name.lower() + "_image_sha256"]
            if window["sha256"] != expected:
                raise SystemExit(f"{name}: source window differs from the pinned OS profile")
            path = output / (name + ".raw")
            if not path.is_file() or path.stat().st_size != window["size"]:
                raise SystemExit(f"{name}: the previously extracted window is missing or has changed size")
            windows[name] = path
        tables = {name: resolved / "build" / f"verified-root-{name}" / "hashes.bin"
                  for name in ("OS", "SystemOS", "App")}
        anchors = set()
        policy_root = ROOT / "scripts/policies" / "26C5035r"
        for name, table in tables.items():
            build = resolved / "build" / f"verified-root-{name}"
            metadata_path = build / "verification.json"
            key_path = build / "trusted-key.bin"
            public_pem = resolved / "build/verified-root-host/keys/owner-public.pem"
            header_path = build / "header.bin"
            signature_path = build / "signature.bin"
            manifest_path = build / "manifest.bin"
            if not all(path.is_file() for path in
                       (table, metadata_path, key_path, public_pem, header_path, signature_path, manifest_path)):
                raise SystemExit(f"{name}: verified policy table or report is missing")
            verified = json.loads(metadata_path.read_text())
            window = windows_by_name[name]
            expected_blocks = (window["size"] + 4095) // 4096
            header_bytes = header_path.read_bytes()
            signature_bytes = signature_path.read_bytes()
            key_bytes = key_path.read_bytes()
            policy_dir = policy_root / name
            if not all(path.is_file() for path in
                       (policy_dir / "manifest.header.bin", policy_dir / "trusted-key.bin",
                        policy_dir / "verification.json")):
                raise SystemExit(f"{name}: owner policy sidecar is missing")
            with manifest_path.open("rb") as manifest_stream:
                manifest_header = manifest_stream.read(544)
            if (not verified.get("real_c_signature_and_block_verification") or
                    verified.get("image_sha256") != roots[name.lower() + "_image_sha256"] or
                    verified.get("volume_uuid") != roots[name.lower() + "_volume_uuid"].upper() or
                    verified.get("blocks_verified") != expected_blocks or len(header_bytes) != 160 or
                    len(signature_bytes) != 384 or len(key_bytes) != 395 or
                    table.stat().st_size != expected_blocks * 32 or
                    manifest_path.stat().st_size != 544 + table.stat().st_size or
                    sha_file(table) != header_bytes[88:120].hex() or
                    hashlib.sha256(key_bytes).digest() != header_bytes[120:152] or
                    header_bytes[56:88].hex() != roots[name.lower() + "_image_sha256"] or
                    header_bytes[40:56] != uuid.UUID(roots[name.lower() + "_volume_uuid"]).bytes or
                    struct.unpack_from("<Q", header_bytes, 32)[0] != expected_blocks or
                    manifest_header != header_bytes + signature_bytes or
                    (policy_dir / "manifest.header.bin").read_bytes() != header_bytes + signature_bytes or
                    (policy_dir / "trusted-key.bin").read_bytes() != key_bytes or
                    json.loads((policy_dir / "verification.json").read_text()) != verified):
                raise SystemExit(f"{name}: existing policy verification does not match the pinned profile")
            verify = subprocess.run(["openssl", "dgst", "-sha256", "-verify", public_pem,
                                     "-signature", signature_path, header_path],
                                    capture_output=True, text=True)
            if verify.returncode:
                raise SystemExit(f"{name}: owner signature read-back failed: {verify.stderr.strip()}")
            anchors.add(hashlib.sha256(key_bytes).hexdigest())
        if len(anchors) != 1:
            raise SystemExit("All root windows must share one verified owner public key")
        if args.normalize_existing:
            image = output / "userdata-root-vroot.img"
            normalize_userdata_image(report, windows, tables, image)
        else:
            image = args.output.resolve() if args.output else output / "userdata-root-vroot-final.img"
            build_userdata(report, windows, tables, image)
        print(f"Verified policies: {ROOT / 'scripts/policies' / '26C5035r'}")
        print(f"Final userdata image: {image}")
        return
    if output.exists():
        raise SystemExit(f"Refusing to overwrite existing policy preparation directory: {output}")
    output.mkdir(parents=True)
    with zipfile.ZipFile(args.archive) as bundle:
        names = set(bundle.namelist())
        if report_name not in names or image_name not in names:
            raise SystemExit("Artifact ZIP does not contain userdata-root.json and userdata-root.img")
        report = json.loads(bundle.read(report_name))
        image_info = bundle.getinfo(image_name)
        if image_info.file_size != report["image_bytes"]:
            raise SystemExit("Artifact ZIP image size differs from its userdata report")
        if report["source"] != "remote" or report["hardware_tested"] is not False:
            raise SystemExit("Artifact metadata does not describe the expected unflashed remote build")
        profile = tomllib.loads(profile_path.read_text())
        roots = profile["root"]
        for window in report["windows"]:
            name = window["name"].lower()
            if (window["sha256"] != roots[name + "_image_sha256"] or
                    window.get("volume_uuid") not in (None, roots[name + "_volume_uuid"])):
                raise SystemExit(f"{window['name']}: artifact window differs from pinned profile")
        windows = extract_windows(args.archive, image_name, report, output)

    host, library = compile_host_verifier(source, resolved)
    private_key = host / "keys/owner.pem"
    public_key = host / "keys/owner-public.pem"
    policy_dir = ROOT / "scripts/policies" / "26C5035r"
    tables = generate_policies(source, resolved, windows, policy_dir, library)
    image = output / "userdata-root-vroot.img"
    build_userdata(report, windows, tables, image)
    print(f"Owner key stays under ignored build output: {private_key}")
    print(f"Public key: {public_key}")
    print(f"Policies: {policy_dir}")
    print(f"New userdata image: {image}")


if __name__ == "__main__":
    main()
