#!/usr/bin/env bash
# Generate the Black/Pink/Yellow/Blue/WhiteCream panel SVG variants for one
# or more AmbientModules modules from a single finished reference theme, by
# substituting only the background/foreground colors.
#
# Usage: generate-theme-svgs.sh [--dry-run] <ReferenceTheme> <Module> [Module...]
#   e.g. generate-theme-svgs.sh WhiteCream Solar50Drone SolarLFO SolarVCO Blank
#
# Assumption (checked once, see plan history): with the color table below, no
# theme's bg equals another theme's fg, so a direct sequential replace
# (bg then fg) never clobbers itself. If the table is edited later and that
# stops being true, re-check the 5x4 reference->target matrix, or fall back
# to a two-stage placeholder-token substitution instead of a direct one.
set -euo pipefail

declare -A THEME_BG=(
  [WhiteCream]="#e9dfd2"
  [Black]="#0d0d0d"
  [Pink]="#ffc0cb"
  [Yellow]="#fbc50c"
  [Blue]="#122e57"
)
declare -A THEME_FG=(
  [WhiteCream]="#000"
  [Black]="#fff"
  [Pink]="#000"
  [Yellow]="#000"
  [Blue]="#fff"
)
THEME_ORDER=(WhiteCream Black Pink Yellow Blue)

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
res_dir="$script_dir/../res"

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
  dry_run=1
  shift
fi

usage() {
  echo "Usage: $0 [--dry-run] <ReferenceTheme> <Module> [Module...]" >&2
  echo "  ReferenceTheme: one of ${THEME_ORDER[*]}" >&2
  exit 1
}

[ $# -ge 2 ] || usage

ref_theme="$1"; shift
[ -n "${THEME_BG[$ref_theme]+x}" ] || { echo "Unknown reference theme: $ref_theme" >&2; usage; }

# #abc -> #aabbcc ; leaves an already-6-digit value untouched.
expand_hex() {
  local h="$1"
  if [[ "$h" =~ ^#([0-9a-fA-F])([0-9a-fA-F])([0-9a-fA-F])$ ]]; then
    printf '#%s%s%s%s%s%s' \
      "${BASH_REMATCH[1]}" "${BASH_REMATCH[1]}" \
      "${BASH_REMATCH[2]}" "${BASH_REMATCH[2]}" \
      "${BASH_REMATCH[3]}" "${BASH_REMATCH[3]}"
  else
    printf '%s' "$h"
  fi
}

ref_bg="${THEME_BG[$ref_theme]}"; ref_bg6="$(expand_hex "$ref_bg")"
ref_fg="${THEME_FG[$ref_theme]}"; ref_fg6="$(expand_hex "$ref_fg")"

# #1a1a1a is a near-black leftover from earlier Inkscape work, used
# interchangeably with pure black (#000000) for foreground elements. Fold it
# into the foreground match whenever the reference foreground is black, so
# it gets themed too instead of staying stuck at near-black forever.
fg_pattern="${ref_fg}|${ref_fg6}"
if [ "${ref_fg6,,}" = "#000000" ]; then
  fg_pattern="${fg_pattern}|#1a1a1a"
fi

status=0

for module in "$@"; do
  ref_file="$res_dir/${module}_${ref_theme}.svg"
  if [ ! -f "$ref_file" ]; then
    echo "SKIP $module: reference file not found: $ref_file" >&2
    status=1
    continue
  fi

  # grep exits 1 on zero matches, which is an expected/informative case here
  # (not a script error) — the `|| true` keeps that from tripping `set -e`.
  bg_hits=$(grep -Eio "${ref_bg}|${ref_bg6}" "$ref_file" | wc -l || true)
  fg_hits=$(grep -Eio "${fg_pattern}" "$ref_file" | wc -l || true)
  echo "== $module (reference $ref_theme): bg matches=$bg_hits, fg matches=$fg_hits =="
  if [ "$bg_hits" -eq 0 ] || [ "$fg_hits" -eq 0 ]; then
    echo "   WARNING: reference color(s) not found literally in $ref_file — check its actual hex codes (nothing will be substituted for a color with 0 matches)." >&2
  fi

  for theme in "${THEME_ORDER[@]}"; do
    [ "$theme" = "$ref_theme" ] && continue
    out_file="$res_dir/${module}_${theme}.svg"
    bg="${THEME_BG[$theme]}"
    fg="${THEME_FG[$theme]}"

    if [ "$dry_run" -eq 1 ]; then
      echo "   [dry-run] would write $out_file (bg->${bg}, fg->${fg})"
      continue
    fi

    # "@" delimiter for the fg substitution since fg_pattern itself contains
    # literal "|" alternation characters that would collide with "|" as a
    # sed delimiter.
    sed -E \
      -e "s|${ref_bg}|${bg}|Ig" \
      -e "s|${ref_bg6}|${bg}|Ig" \
      -e "s@${fg_pattern}@${fg}@Ig" \
      "$ref_file" > "$out_file"
    echo "   wrote $out_file"
  done
done

exit "$status"
