#!/usr/bin/env python3
"""Build the macOS 27 userdata root image on a pinned macOS runner.

The image holds the unchanged OS/SystemOS/App disk images from the Apple IPSW
behind a SHNGROOT manifest. Only those three members are read from the remote
IPSW (HTTP range requests, ~13 GiB), not the whole 25.3 GiB archive. Geometry
comes from board.toml [userdata], measured on the device in TWRP.

Modes:
  dry-run   resolve the config, verify the IPSW members exist and fit capacity
  build     extract/decrypt the disk images and write userdata-root.img + .json
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
    ap.add_argument("mode", choices=["dry-run", "build", "collect"])
    ap.add_argument("--source", type=Path, default=ROOT / ".work/UniXNU")
    ap.add_argument("--ipsw")
    ap.add_argument("--unique-chip-id")
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


if __name__ == "__main__":
    main()
