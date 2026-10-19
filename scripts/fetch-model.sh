#!/usr/bin/env bash
# Downloads all-MiniLM-L6-v2 (ONNX, fp32) and its WordPiece vocab at a pinned HuggingFace revision
# and verifies SHA-256. Usage: scripts/fetch-model.sh [dest]   (default: models/)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/models}"
repo="sentence-transformers/all-MiniLM-L6-v2"
revision="1110a243fdf4706b3f48f1d95db1a4f5529b4d41"

fetch() {  # <path in repo> <local name> <sha256>
  local out="$dest/$2"
  if [[ -f "$out" ]] && echo "$3  $out" | sha256sum --check --status; then
    return
  fi
  curl -fsSL --retry 3 -o "$out.tmp" "https://huggingface.co/$repo/resolve/$revision/$1"
  echo "$3  $out.tmp" | sha256sum --check --quiet
  mv "$out.tmp" "$out"
}

mkdir -p "$dest"
fetch onnx/model.onnx all-MiniLM-L6-v2.onnx 6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452
fetch vocab.txt vocab.txt 07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3
echo "model ready in $dest ($repo@$revision)"
