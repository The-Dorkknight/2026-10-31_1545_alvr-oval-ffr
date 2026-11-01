#!/usr/bin/env bash
# OvalFFR - run ONE program on the discrete ("gaming") GPU of a Linux laptop with two graphics cards.
# Nothing is installed or changed on the system: the variables below only apply to this one launch.
#
#   ./ovalffr-gpu-linux.sh alvr_dashboard
#   ./ovalffr-gpu-linux.sh --list                  show the graphics cards found and what would be set
#   ./ovalffr-gpu-linux.sh --dry-run steam         print the command instead of running it
#   ./ovalffr-gpu-linux.sh --vendor nvidia steam   force the NVIDIA variables  (or: mesa)
#
# NVIDIA (proprietary driver): __NV_PRIME_RENDER_OFFLOAD=1, __GLX_VENDOR_LIBRARY_NAME=nvidia,
#   __VK_LAYER_NV_optimus=NVIDIA_only   (NVIDIA's PRIME render offload README)
# AMD / Intel discrete (Mesa): DRI_PRIME=1
# Flatpak apps (e.g. a Flatpak Steam) do not inherit these; use
#   flatpak override --user --env=__NV_PRIME_RENDER_OFFLOAD=1 --env=__VK_LAYER_NV_optimus=NVIDIA_only <app-id>
#   (or --env=DRI_PRIME=1), and undo with: flatpak override --user --reset <app-id>
set -u

vendor=auto; dry=0; list=0
while [ $# -gt 0 ]; do
  case "$1" in
    --vendor) vendor="${2:-}"; shift 2 ;;
    --dry-run) dry=1; shift ;;
    --list) list=1; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    --) shift; break ;;
    *) break ;;
  esac
done

# graphics cards: one line per VGA / 3D / Display controller
gpus=""
if command -v lspci >/dev/null 2>&1; then
  gpus="$(lspci -nn 2>/dev/null | grep -Ei 'VGA compatible controller|3D controller|Display controller' || true)"
fi
count=0; [ -n "$gpus" ] && count=$(printf '%s\n' "$gpus" | wc -l)
has_nvidia=0; printf '%s' "$gpus" | grep -qi 'nvidia' && has_nvidia=1

if [ "$vendor" = auto ]; then
  if [ "$count" -lt 2 ]; then vendor=none
  elif [ "$has_nvidia" = 1 ]; then vendor=nvidia
  else vendor=mesa; fi
fi

case "$vendor" in
  nvidia) vars=(__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia __VK_LAYER_NV_optimus=NVIDIA_only) ;;
  mesa)   vars=(DRI_PRIME=1) ;;
  none)   vars=() ;;
  *) echo "unknown --vendor '$vendor' (use nvidia, mesa or auto)" >&2; exit 2 ;;
esac

if [ "$list" = 1 ] || [ $# -eq 0 ]; then
  echo "Graphics cards found ($count):"; [ -n "$gpus" ] && printf '%s\n' "$gpus" | sed 's/^/  /' || echo "  (lspci not available or no cards listed)"
  if [ "${#vars[@]}" -gt 0 ]; then echo "Would set: ${vars[*]}"; else echo "Would set nothing (fewer than two graphics cards, or --vendor none)."; fi
  [ $# -eq 0 ] && [ "$list" = 0 ] && echo "Usage: $0 [--vendor nvidia|mesa] [--dry-run] <program> [arguments]"
  exit 0
fi

if [ "${#vars[@]}" -eq 0 ]; then
  echo "[ovalffr-gpu] only $count graphics card(s) found, starting '$1' unchanged." >&2
else
  echo "[ovalffr-gpu] $count graphics cards found; starting '$1' with: ${vars[*]}" >&2
fi
if [ "$dry" = 1 ]; then echo env "${vars[@]}" "$@"; exit 0; fi
exec env "${vars[@]}" "$@"
