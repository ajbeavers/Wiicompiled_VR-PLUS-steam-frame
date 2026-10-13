#!/usr/bin/env bash
# Publish upstream's translator as a self-contained single-file linux-arm64 executable, so the
# end user never needs a dotnet SDK. Run once by the maintainer (needs the .NET 8 SDK).
#
#   publish-translator.sh <outdir>      writes <outdir>/Translator.Cli and <outdir>/Translator.Cli.sha256
#
# The SDK is found on PATH or in $DOTNET_ROOT (default ~/.local/opt/dotnet). Same publish flags as
# upstream's Launcher/build-appimage.sh.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
[[ $# -eq 1 ]] || { echo "usage: publish-translator.sh <outdir>" >&2; exit 2; }
out=$1
export DOTNET_ROOT=${DOTNET_ROOT:-$HOME/.local/opt/dotnet}
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
dotnet=$DOTNET_ROOT/dotnet
[[ -x $dotnet ]] || dotnet=$(command -v dotnet) || { echo "no .NET 8 SDK found" >&2; exit 1; }

mkdir -p "$out"
tmp=$(mktemp -d "${TMPDIR:-$HOME/.cache}/kf-publish.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
"$dotnet" publish "$repo/translator/src/Translator.Cli" -c Release -r linux-arm64 \
    --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$tmp"
install -m 0755 "$tmp/Translator.Cli" "$out/Translator.Cli"
(cd "$out" && sha256sum Translator.Cli > Translator.Cli.sha256)
echo "published: $out/Translator.Cli ($(du -h "$out/Translator.Cli" | cut -f1)) $(cut -d' ' -f1 "$out/Translator.Cli.sha256")"
