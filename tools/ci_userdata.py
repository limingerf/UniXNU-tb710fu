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
import contextlib
import hashlib
import json
import os
import plistlib
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
    ap.add_argument("--os-image", type=Path,
                    help="reuse this already-extracted OS disk image instead of "
                         "fetching the OS component from the IPSW")
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
        # All logs and reports land in output/ directly (seed mode writes
        # data-seed-build.log, data-seed-attach.log, data-seed.json and data.dbr
        # there), so collecting only has to record the resolved work path and the
        # pinned revisions for the artifact.
        resolved = ROOT / ".work/resolved-work.txt"
        if resolved.is_file():
            shutil.copy2(resolved, output / "resolved-work.txt")
        for name in ("upstream.lock.json", "unixnu.lock"):
            p = ROOT / name
            if p.is_file():
                shutil.copy2(p, output / name)
        return

    # seed needs the framework's checkout revision but not its build: it reads the
    # OS component from the IPSW directly, so defconfig, the dry-run and the root
    # image build are all skipped. That keeps a seed-only run to one component
    # download instead of a full 16.4 GiB image build.
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != LOCK["unixnu"]["commit"]:
        raise SystemExit("Official source revision differs from upstream.lock.json")

    seed_value = args.unique_chip_id or os.environ.get("UNIXNU_UNIQUE_CHIP_ID")
    if not seed_value:
        raise SystemExit("set UNIXNU_UNIQUE_CHIP_ID (device serial as hex) for the payload ADT")
    env = dict(os.environ, UNIXNU_UNIQUE_CHIP_ID=seed_value)

    if args.mode == "seed":
        sys.path.insert(0, str(source / "scripts"))
        from unixnu.cli import parser as cli_parser
        from unixnu.config import Context
        ctx = Context(cli_parser().parse_args(["status"] + common))
        (ROOT / ".work/resolved-work.txt").write_text(str(ctx.work) + "\n")
        build_data_seed(args, source, ctx.work, output, env, common, driver)
        return

    command(driver + ["defconfig", "tb710fu_macos_defconfig"] + common, source, output / "defconfig.log")
    sys.path.insert(0, str(source / "scripts"))
    from unixnu.cli import parser as cli_parser
    from unixnu.config import Context
    ctx = Context(cli_parser().parse_args(["status"] + common))
    work = ctx.work
    (ROOT / ".work/resolved-work.txt").write_text(str(work) + "\n")

    # userdata.build runs before prepare() in cmd_build, so no fetch is needed.
    # defconfig establishes the selected configuration and work directory.
    seed = seed_value

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

    Input is the OS disk image straight from the IPSW, exactly as the framework's
    own seed recipe uses it. The root image is not involved: the seed only needs a
    readable OS volume (it copies private/var and checks /sbin/launchd), so
    building the 16.4 GiB root image first would be pure waste.
    """
    gib = args.data_gib or args.data_gib_default or default_data_gib()
    if not 3 <= gib <= 1024:
        raise SystemExit(f"derived Data size {gib} GiB is outside the 3..1024 range the "
                         "seed tool accepts; pass --data-gib explicitly")
    dh, sep, pw = args.data_user.partition(":") if args.data_user else ("", "", "")
    if args.data_user and (not dh or not sep or not pw):
        raise SystemExit("--data-user must be NAME:PASSWORD")

    os_image = os_disk_image(args, source, work)

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
        #
        # It takes a mounted volume, not an image: it reads private/var and
        # checks /sbin/launchd directly under that path. Attach the image here and
        # hand over the mount point.
        make = source / "scripts" / "make-data-seed.py"
        if not make.is_file():
            raise SystemExit(f"seed tool missing: {make}")
        build = os.environ.get("UNIXNU_BUILD") or PROFILE.rsplit("-", 1)[-1]
        log = output / "data-seed-build.log"
        with mount_os_image(os_image, output / "data-seed-attach.log") as volume:
            cmd = [sys.executable, str(make), str(volume), str(seed),
                   "--gib", str(gib), "--build", build, "--skip-setup"]
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


def os_disk_image(args, source, work):
    """The OS disk image the seed is built from.

    Prefers the IPSW directly (the framework's own recipe), which needs no root
    image. Falls back to slicing the OS window out of a root image that a
    previous run already built, so an existing artefact can still be reused.
    """
    cache = work / "userdata"
    cache.mkdir(parents=True, exist_ok=True)
    os_image = cache / "OS.raw"

    if args.os_image:
        candidate = Path(args.os_image).expanduser().resolve(strict=True)
        require_os_volume(candidate)
        return candidate

    if os_image.is_file() and os_image.stat().st_size > 0:
        require_os_volume(os_image)
        return os_image

    # Ask the pinned framework for the component; it downloads by range, decrypts
    # AEA and caches the plain image beside the archive.
    try:
        sys.path.insert(0, str(source / "scripts"))
        from unixnu.cli import parser as cli_parser
        from unixnu.config import Context
        from unixnu.ipsw import Ipsw
        ctx = Context(cli_parser().parse_args(["status", "-D", str(ROOT), "--os", PROFILE]))
        plain = Path(Ipsw(ctx).disk_image("OS"))
    except Exception as exc:                                   # noqa: BLE001
        raise SystemExit(f"could not obtain the OS disk image from the IPSW: {exc}\n"
                         "Pass --os-image FILE to reuse an extracted OS image instead")
    require_os_volume(plain)
    return plain


@contextlib.contextmanager
def mount_os_image(image, log_path):
    """Attach an OS disk image read-only and yield its system volume mount point.

    make-data-seed.py reads private/var and checks /sbin/launchd under the path it
    is given, so it needs the mounted volume, not the image.

    The IPSW OS image is an APFS container holding several volumes, so no single
    -mountpoint can be assumed: hdiutil mounts them under /Volumes and the system
    volume is whichever entity carries sbin/launchd. Passing -mountpoint would
    pin only the first volume and hide the system one.
    """
    image = Path(image).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["hdiutil", "attach", "-readonly", "-nobrowse", "-noverify", "-plist", str(image)]
    with log_path.open("w") as s:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=s, text=True)
    if r.returncode:
        raise SystemExit(f"hdiutil attach failed for {image}: see {log_path}")
    attached = plistlib.loads(r.stdout)
    device = attached["system-entities"][0]["dev-entry"]
    try:
        volumes = [Path(item["mount-point"]) for item in attached["system-entities"]
                   if item.get("mount-point")]
        system = [p for p in volumes
                  if (p / "sbin" / "launchd").is_file() and (p / "private" / "var").is_dir()]
        if len(system) != 1:
            listing = ", ".join(str(p) for p in volumes) or "none"
            raise SystemExit(
                f"the attached OS image has {len(system)} system volumes; expected exactly one "
                f"with sbin/launchd and private/var. Mounted: {listing}")
        yield system[0]
    finally:
        subprocess.run(["hdiutil", "detach", device], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def require_os_volume(image):
    """Refuse an image that cannot be an IPSW OS volume.

    make-data-seed.py mounts the file and then needs /sbin/launchd plus
    private/var; checking the file looks like a disk image here turns a confusing
    mount error into a clear one.
    """
    if not image.is_file() or image.stat().st_size == 0:
        raise SystemExit(f"OS image is missing or empty: {image}")
    with Path(image).open("rb") as stream:
        head = stream.read(4096)
    if len(head) < 4096:
        raise SystemExit(f"OS image is too small to be a disk image: {image}")
    # APFS containers carry NXSB at offset 32; HFS+ carries H+/HX at 1024.
    if head[32:36] != b"NXSB" and head[1024:1026] not in (b"H+", b"HX"):
        raise SystemExit(f"OS image is neither an APFS nor an HFS+ container: {image}")


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
