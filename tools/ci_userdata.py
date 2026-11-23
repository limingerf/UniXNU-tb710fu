#!/usr/bin/env python3
"""Build the macOS 27 userdata root image on a pinned macOS runner.

The image holds the unchanged OS/SystemOS/App disk images from the Apple IPSW
behind a SHNGROOT manifest. Only those three members are read from the remote
IPSW (HTTP range requests, ~13 GiB), not the whole 25.3 GiB archive. Geometry
comes from board.toml [userdata], measured on the device in TWRP.

Modes:
  dry-run   resolve the config, verify the IPSW members exist and fit capacity
  build     extract/decrypt the disk images and write userdata-root.img + .json
  seed      build the writable Data volume seed (data.dbr) from the same OS image
  collect   copy reports and small evidence into the artifact directory
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "upstream.lock.json").read_text())
PROFILE = "macos-27.3-26C5035r"

# The Data volume fills the unixnu partition behind the root image. These are the
# geometry facts the packager and the driver both use:
#   sgdisk --print /dev/block/sda on the device  -> 4096-byte sectors,
#   unixnu = LBA 23455144..61390842 (144.71 GiB)
#   root image 17,603,493,888 B (16.39 GiB) as measured in the SHNGROOT report
#   sheng-log-reserve default 1024 MiB, plus the driver's 4 MiB image guard
# The seed's logical size is what matters here: the packed seed is only its
# metadata, so a 127 GiB volume still packs into a few MiB.
PARTITION_FIRST_LBA = 23455144
PARTITION_LAST_LBA = 61390842
PARTITION_SECTOR = 4096
ROOT_IMAGE_BYTES = 17603493888
LOG_RESERVE_MIB = 1024
IMAGE_GUARD_MIB = 4


def default_data_gib():
    """Largest whole-GiB Data volume the unixnu partition can hold."""
    total = (PARTITION_LAST_LBA - PARTITION_FIRST_LBA + 1) * PARTITION_SECTOR
    spare = total - ROOT_IMAGE_BYTES - (LOG_RESERVE_MIB + IMAGE_GUARD_MIB) * 1024 ** 2
    return int(spare // 1024 ** 3)


def command(cmd, cwd, log_path):
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w") as stream:
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=stream,
                           stderr=subprocess.STDOUT)
    if r.returncode:
        sys.stderr.write(f"--- {log_path} ---\n" + log_path.read_text(errors="replace")[-4000:])
        raise SystemExit(f"command failed ({r.returncode}): {cmd[:3]}")


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=["dry-run", "build", "seed", "collect"])
    ap.add_argument("--source", type=Path, default=ROOT / ".work/UniXNU")
    ap.add_argument("--ipsw")
    ap.add_argument("--unique-chip-id")
    ap.add_argument("--data-gib", type=int, default=0,
                    help="Data volume logical size in GiB; 0 derives the largest size the "
                         "unixnu partition can hold behind the root image and log reserve")
    ap.add_argument("--data-gib-default", type=int, default=0,
                    help="override the derived default (0 = derive from the partition geometry)")
    ap.add_argument("--data-user", default="",
                    help="NAME:PASSWORD for the local administrator; empty keeps the seed "
                         "accountless and relies on --skip-setup only")
    ap.add_argument("--data-seed-in", type=Path,
                    help="grow this existing packed seed instead of creating one")
    ap.add_argument("--data-seed-max-bytes", type=int, default=0,
                    help="refuse a seed larger than this (0 = no limit); the payload FAT "
                         "has only about 9.4 MiB spare in the current baseline")
    args = ap.parse_args()
    source = args.source.resolve()
    output = ROOT / ".work/artifacts"
    output.mkdir(parents=True, exist_ok=True)

    work = source / "out/tb710fu" / PROFILE / "tb710fu_macos"
    driver = [sys.executable, str(source / "unixnu")]
    common = ["-D", str(ROOT), "--os", PROFILE]

    if args.mode == "collect":
        # All logs and reports land in output/ directly; just make sure the
        # resolved work path is recorded for the workflow's image staging step.
        resolved = ROOT / ".work/resolved-work.txt"
        if resolved.is_file():
            shutil.copy2(resolved, output / "resolved-work.txt")
        for name in ("upstream.lock.json", "unixnu.lock"):
            p = ROOT / name
            if p.is_file():
                shutil.copy2(p, output / name)
        return

    # Resolve the real work path like ci_kernel.py does.
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != LOCK["unixnu"]["commit"]:
        raise SystemExit("Official source revision differs from upstream.lock.json")
    command(driver + ["defconfig", "tb710fu_macos_defconfig"] + common, source, output / "defconfig.log")
    sys.path.insert(0, str(source / "scripts"))
    from unixnu.cli import parser as cli_parser
    from unixnu.config import Context
    ctx = Context(cli_parser().parse_args(["status"] + common))
    work = ctx.work
    (ROOT / ".work/resolved-work.txt").write_text(str(work) + "\n")

    # userdata.build runs before prepare() in cmd_build, so no fetch is needed.
    # defconfig establishes the selected configuration and work directory.
    seed = args.unique_chip_id or os.environ.get("UNIXNU_UNIQUE_CHIP_ID")
    if not seed:
        raise SystemExit("set UNIXNU_UNIQUE_CHIP_ID (device serial as hex) for the payload ADT")
    env = dict(os.environ, UNIXNU_UNIQUE_CHIP_ID=seed)

    # dry-run: verify the windows exist in the IPSW and the image fits capacity
    cmd = driver + ["build", "userdata", "--dry-run"] + common
    log = output / "userdata-dry-run.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as s:
        r = subprocess.run([str(c) for c in cmd], cwd=source, stdout=s,
                           stderr=subprocess.STDOUT, env=env)
    if r.returncode:
        sys.stderr.write(log.read_text(errors="replace")[-4000:])
        raise SystemExit("userdata dry-run failed")
    print("dry-run PASS: IPSW members resolvable and image fits the partition", flush=True)

    if args.mode == "dry-run":
        return

    if args.mode == "seed":
        build_data_seed(args, source, work, output, env, common, driver)
        return

    cmd = driver + ["build", "userdata"] + common
    log = output / "userdata-build.log"
    with log.open("w") as s:
        r = subprocess.run([str(c) for c in cmd], cwd=source, stdout=s,
                           stderr=subprocess.STDOUT, env=env)
    if r.returncode:
        sys.stderr.write(log.read_text(errors="replace")[-4000:])
        raise SystemExit("userdata build failed")

    image = work / "userdata" / "userdata-root.img"
    report = work / "userdata" / "userdata-root.json"
    if not image.is_file() or not report.is_file():
        raise SystemExit("userdata-root.img / .json missing after build")
    data = json.loads(report.read_text())
    if data.get("hardware_tested") is not False:
        raise SystemExit("unexpected hardware_tested value in report")
    if data.get("image_sha256") != sha256(image):
        raise SystemExit("image sha256 does not match the report")
    print(json.dumps({"image_bytes": data["image_bytes"],
                      "image_sha256": data["image_sha256"],
                      "windows": [w["name"] for w in data["windows"]],
                      "userdata_bytes": data["userdata_bytes"]}, indent=2), flush=True)

    # Copy the big image next to the reports for the workflow to upload.
    shutil.copy2(report, output / "userdata-root.json")


def build_data_seed(args, source, work, output, env, common, driver):
    """Create the writable Data volume seed (data.dbr).

    The seed is a small packed APFS OwnerData container, not a whole disk. The
    kernel mounts it as the ramdisk behind /System/Volumes/Data and the ShengUfs
    persistent window carries the writes. Without it launchd starts and then
    restarts, because there is no writable Data volume to attach.

    Input is the OS disk image this run already produced, so the seed is built
    from exactly the same OS revision as the root image it will be paired with.
    """
    # The OS window of the root image is written to <work>/userdata/OS.raw (or
    # the window is read straight out of userdata-root.img). Prefer the raw
    # window; fall back to the packed root image's first window.
    os_image = work / "userdata" / "OS.raw"
    root_image = work / "userdata" / "userdata-root.img"
    if not os_image.is_file():
        if not root_image.is_file():
            raise SystemExit("build the userdata root image first: OS.raw / userdata-root.img missing")
        report = work / "userdata" / "userdata-root.json"
        if not report.is_file():
            raise SystemExit("userdata-root.json missing; cannot locate the OS window")
        data = json.loads(report.read_text())
        windows = {w["name"]: w for w in data.get("windows", [])}
        if "OS" not in windows:
            raise SystemExit("userdata-root.json has no OS window")
        os_image = slice_window(root_image, windows["OS"], work / "userdata" / "OS.raw")

    gib = args.data_gib or args.data_gib_default or default_data_gib()
    if not 3 <= gib <= 1024:
        raise SystemExit(f"derived Data size {gib} GiB is outside the 3..1024 range the "
                         "seed tool accepts; pass --data-gib explicitly")
    dh, sep, pw = args.data_user.partition(":") if args.data_user else ("", "", "")
    if args.data_user and (not dh or not sep or not pw):
        raise SystemExit("--data-user must be NAME:PASSWORD")

    seed_dir = work / "data-seed"
    seed_dir.mkdir(parents=True, exist_ok=True)
    seed = seed_dir / "data.dbr"

    if args.data_seed_in:
        # Grow an existing seed: keeps its Data volume UUID, so an installation
        # that already persisted data keeps the same identity. The input and
        # output are both packed DBRDISK1 seeds.
        grow = source / "scripts" / "seed-data-grow.py"
        if not grow.is_file():
            raise SystemExit(f"seed grow tool missing: {grow}")
        cmd = [sys.executable, str(grow), "--gib", str(gib),
               str(args.data_seed_in), str(seed)]
        log = output / "data-seed-grow.log"
        with log.open("w") as s:
            r = subprocess.run([str(c) for c in cmd], cwd=source, stdout=s,
                               stderr=subprocess.STDOUT, env=env)
        if r.returncode:
            sys.stderr.write(log.read_text(errors="replace")[-4000:])
            raise SystemExit("data seed grow failed")
    else:
        # make-data-seed.py builds the raw APFS OwnerData container, packs it
        # (DBRDISK1, zero pages dropped) and grows it to the requested size. That
        # packed form is what the payload carries; a raw multi-GiB seed would not
        # fit in the payload FAT, whose spare space is only a few MiB.
        make = source / "scripts" / "make-data-seed.py"
        if not make.is_file():
            raise SystemExit(f"seed tool missing: {make}")
        # The OS build number seeds the migration record. PROFILE is
        # "<name>-<version>-<build>", so the last dash-separated field is it.
        build = os.environ.get("UNIXNU_BUILD") or PROFILE.rsplit("-", 1)[-1]
        # --skip-setup marks the Setup Assistant complete so the seed reaches a
        # login rather than the Hello pages. An account is created by the raw-seed
        # tool (make-macos-data-seed.py), not by this packed builder, so the flag
        # is always on here: a seed with no account still needs Setup done.
        cmd = [sys.executable, str(make), str(os_image), str(seed),
               "--gib", str(gib), "--build", build, "--skip-setup"]
        log = output / "data-seed-build.log"
        with log.open("w") as s:
            r = subprocess.run([str(c) for c in cmd], cwd=source, stdout=s,
                               stderr=subprocess.STDOUT, env=env)
        if r.returncode:
            sys.stderr.write(log.read_text(errors="replace")[-6000:])
            raise SystemExit("data seed build failed")

    if not seed.is_file():
        raise SystemExit("data.dbr missing after seed build")

    # The payload FAT is a fixed-size filesystem and mcopy does not grow it: the
    # reviewed baseline has 5,752,320 bytes (5.49 MiB) free after the kernel is in
    # place. An oversized seed would only fail inside mcopy, so check it here
    # where the number and the cause are both visible.
    seed_bytes_size = seed.stat().st_size
    if args.data_seed_max_bytes and seed_bytes_size > args.data_seed_max_bytes:
        raise SystemExit(
            f"data.dbr is {seed_bytes_size:,} bytes, over the {args.data_seed_max_bytes:,}-byte "
            "payload FAT limit; the packed seed has to fit inside the existing vendor_boot")

    record = {
        "seed_bytes": seed_bytes_size,
        "seed_sha256": sha256(seed),
        "data_gib": gib,
        "format": "DBRDISK1 packed",
        "os_image": str(os_image),
        "os_image_bytes": os_image.stat().st_size,
        "os_image_sha256": sha256(os_image),
        "grew_from": str(args.data_seed_in) if args.data_seed_in else None,
        "skip_setup": bool(dh),
        "user": dh or None,
    }
    (output / "data-seed.json").write_text(json.dumps(record, indent=2) + "\n")
    shutil.copy2(seed, output / "data.dbr")
    print(json.dumps(record, indent=2), flush=True)


def slice_window(root_image, window, destination):
    """Write one SHNGROOT window out of a packed root image to its own file."""
    if destination.is_file() and destination.stat().st_size == window["size"]:
        return destination
    offset, size = window["offset"], window["size"]
    h = hashlib.sha256()
    with Path(root_image).open("rb") as src, destination.open("wb") as dst:
        src.seek(offset)
        left = size
        while left:
            chunk = src.read(min(1 << 22, left))
            if not chunk:
                raise SystemExit("root image truncated while slicing the OS window")
            h.update(chunk)
            dst.write(chunk)
            left -= len(chunk)
    if h.hexdigest() != window["sha256"]:
        destination.unlink(missing_ok=True)
        raise SystemExit("sliced OS window sha256 does not match the manifest")
    return destination


if __name__ == "__main__":
    main()
