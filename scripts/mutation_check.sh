#!/usr/bin/env bash
# Shows the differential fuzzer can fail: applies realistic bugs, one at a time, to a copy of
# matching_engine.hpp, rebuilds fuzz_differential against it and requires a MISMATCH. Also
# requires the unmutated engine to pass. Prints one line per case.
#   CXX=g++-13 scripts/mutation_check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=${CXX:-c++}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/nexus/core"

check() {  # name, original text, replacement, expected outcome (fail|pass)
  python3 - "$2" "$3" "$work/include/nexus/core/matching_engine.hpp" <<'PY'
import sys, pathlib
old, new, dst = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
src = pathlib.Path("include/nexus/core/matching_engine.hpp").read_text()
if old not in src:
    sys.exit(f"mutation target not found: {old}")
dst.write_text(src.replace(old, new, 1))
PY
  # shellcheck disable=SC2086
  "$CXX" -std=c++20 -O2 ${CXXFLAGS:-} -I"$work/include" -Iinclude -I. \
    tests/fuzz_differential.cpp -o "$work/fuzz"
  if "$work/fuzz" --seeds 8 --commands 20000 > "$work/out.txt" 2>&1; then got=pass; else got=fail; fi
  if [[ "$got" != "$4" ]]; then
    echo "FAILED: $1 expected $4, got $got"
    exit 1
  fi
  echo "ok: $1 -> $got ($(grep -m1 -E 'MISMATCH|all identical' "$work/out.txt"))"
}

check "unmutated engine" "namespace nexus {" "namespace nexus {" pass
check "modify keeps priority on quantity increase" "c.qty <= o->remaining()" "c.qty <= o->qty" fail
check "FOK fillability off by one" "if (v >= need) return true;" "if (v > need) return true;" fail
check "market remainder not cancelled" \
  "emit(Event::cancelled(c.id, c.side, shown, left));" "(void)shown;" fail
check "sweep stops early" "if (qty == 0) break;" "if (qty < 3) break;" fail
