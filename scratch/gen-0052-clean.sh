#!/usr/bin/env bash
# Generate patch 0052 from a clean pinned XNU worktree.
#
# .work/xnu-patch-lf-clean has a dirty working tree (leftovers from earlier
# patch authoring), so a plain `git diff` there picks up unrelated changes.
# Export HEAD to a fresh worktree instead and diff only our edit.
set -euo pipefail

REPO=/mnt/d/unixnu-tb710fu-clean/.work/xnu-patch-lf-clean
WORK=/tmp/xnu-0052-clean
OUT=/mnt/d/unixnu-tb710fu-clean/patches/xnu/0052-tb710fu-shared-region-map-trace.patch

rm -rf "$WORK"
cd "$REPO"
git worktree remove --force "$WORK" 2>/dev/null || true
git worktree add --detach --force "$WORK" HEAD >/dev/null

cd "$WORK"
# Prove the anchors are unique before touching anything.
FILE=bsd/vm/vm_unix.c
test "$(grep -c 'shared_region_map_and_slide_2_np(' $FILE)" = "2" || { echo "entry anchor count changed" >&2; exit 1; }

python3 - <<'PY'
import io, sys

path = 'bsd/vm/vm_unix.c'
with io.open(path, 'r', encoding='utf-8', errors='surrogateescape') as fh:
    text = fh.read()

# The syscall body: locate the definition, then its local declarations.
defn = "shared_region_map_and_slide_2_np(\n\tstruct proc                                  *p,"
if text.count(defn) != 1:
    sys.exit('definition anchor count %d' % text.count(defn))
start = text.index(defn)
tail = text[start:]

decl = "\tkern_return_t                 kr = KERN_SUCCESS;\n\n\tfiles_count = uap->files_count;"
if tail.count(decl) != 1:
    sys.exit('decl anchor count %d' % tail.count(decl))

decl_new = (
    "\tkern_return_t                 kr = KERN_SUCCESS;\n"
    "\n"
    "#if defined(__arm64__)\n"
    "\t/* TB710FU: unconditional trace. The release kernel does not emit\n"
    "\t * SHARED_REGION_TRACE_*, so a stalled boot cannot otherwise tell\n"
    "\t * \"dyld never called map_and_slide\" from \"it was called and failed\". */\n"
    "\tprintf(\"TB710FU SRMAP[%d]: enter files=%u mappings=%u\\n\",\n"
    "\t    proc_getpid(p), uap->files_count, uap->mappings_count);\n"
    "#endif\n"
    "\n"
    "\tfiles_count = uap->files_count;"
)
tail = tail.replace(decl, decl_new, 1)

exit_anchor = "\t\t(uint64_t)uap->mappings_u, kr));\n\n\treturn kr;\n}\n"
if tail.count(exit_anchor) != 1:
    sys.exit('exit anchor count %d' % tail.count(exit_anchor))
exit_new = (
    "\t\t(uint64_t)uap->mappings_u, kr));\n"
    "\n"
    "#if defined(__arm64__)\n"
    "\t/* TB710FU: one line per call, covering every failure path above. */\n"
    "\tprintf(\"TB710FU SRMAP[%d]: exit kr=0x%x files=%u mappings=%u\\n\",\n"
    "\t    proc_getpid(p), kr, files_count, mappings_count);\n"
    "#endif\n"
    "\n"
    "\treturn kr;\n}\n"
)
tail = tail.replace(exit_anchor, exit_new, 1)

text = text[:start] + tail
with io.open(path, 'w', encoding='utf-8', errors='surrogateescape') as fh:
    fh.write(text)
print('patched', path)
PY

git diff -- $FILE > /tmp/0052-body.patch
test -s /tmp/0052-body.patch || { echo "ERROR: empty diff" >&2; exit 1; }

{
  echo "Log every shared_region_map_and_slide_2_np call and result to COM5."
  echo
  echo "A stalled boot shows launchd reaching dyld, getting"
  echo "\"result from check_np(): -1, errno 12\" (the documented normal"
  echo "\"shared region is empty\" response, also present in every successful"
  echo "boot), and then never logging \"dyld cache mapped system-wide\"."
  echo
  echo "The syscall that would establish the mapping is"
  echo "shared_region_map_and_slide_2_np() (536). Its tracing is behind"
  echo "SHARED_REGION_TRACE_DEBUG/_ERROR, which the release kernel does not"
  echo "emit, so the log cannot distinguish \"never called\" from \"called and"
  echo "failed\". This prints once on entry and once on exit, including the"
  echo "failure return code. Side-effect free: printf only, no state."
  cat /tmp/0052-body.patch
} > "$OUT"

echo "=== patch hunks ==="
grep -n '^@@' "$OUT"
echo "=== hunk count: $(grep -c '^@@' "$OUT") (expect 2) ==="
echo "=== verify it applies to a pristine tree ==="
rm -rf /tmp/xnu-verify && cd "$REPO" && git worktree add --detach --force /tmp/xnu-verify HEAD >/dev/null
cd /tmp/xnu-verify && git apply --check "$OUT" && echo "APPLY_CHECK_PASS"
cd "$REPO" && git worktree remove --force "$WORK" 2>/dev/null || true
git worktree remove --force /tmp/xnu-verify 2>/dev/null || true
