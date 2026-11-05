#!/usr/bin/env bash
# Build the AGS extension release package from a configured and built tree.
#
#   tools/package.sh <build-dir> <out-dir>
#
# Installs <build-dir> into a private staging folder, checks it against the AGS package rules
# (docs/extensions/STANDARD.md section 3), and writes into <out-dir>:
#   kytyps5-ags-<version>-linux-<arch>.tar.zst   (contents at the archive root)
#   kytyps5-ags-<version>-linux-<arch>.tar.zst.sha256   ("<lowercase hex>  <file name>")
set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <build-dir> <out-dir>" >&2
	exit 2
fi
build=$(realpath -e -- "$1")
mkdir -p -- "$2"
out=$(realpath -e -- "$2")

manifest="$build/ags-extension.toml"
[ -f "$manifest" ] || { echo "error: $manifest not found; configure the build first" >&2; exit 1; }
field() { sed -n "s/^$1 = \"\\([^\"]*\\)\"\$/\\1/p" "$manifest" | head -n 1; }
id=$(field id)
version=$(field version)
adapter=$(field adapter)
[ "$id" = kytyps5 ] && [ -n "$version" ] && [ -n "$adapter" ] || { echo "error: unexpected manifest $manifest" >&2; exit 1; }

arch=$(uname -m)
case "$arch" in x86_64 | aarch64) ;; *) echo "error: unsupported architecture $arch" >&2; exit 1 ;; esac
asset="kytyps5-ags-$version-linux-$arch.tar.zst"

# The only directory removed below is the one mktemp just created.
work=$(mktemp -d "${TMPDIR:-/tmp}/kytyps5-ags-package.XXXXXXXX")
[ -n "$work" ] && [ -d "$work" ] || { echo "error: mktemp failed" >&2; exit 1; }
trap 'rm -rf -- "$work"' EXIT
tree="$work/tree"

cmake --install "$build" --prefix "$tree" >&2
root=$(realpath -e -- "$tree")

# Only regular files, directories and relative links that resolve inside the tree.
special=$(find "$root" ! -type f ! -type d ! -type l)
[ -z "$special" ] || { echo "error: special files in the package:" >&2; echo "$special" >&2; exit 1; }
find "$root" -type l -print0 > "$work/links" || { echo "error: scanning links failed" >&2; exit 1; }
while IFS= read -r -d '' link; do
	target=$(readlink -- "$link")
	case "$target" in /*) echo "error: absolute link $link -> $target" >&2; exit 1 ;; esac
	real=$(realpath -e -- "$link") || { echo "error: dangling or looping link $link" >&2; exit 1; }
	case "$real/" in "$root"/*) ;; *) echo "error: link escapes the package: $link -> $target" >&2; exit 1 ;; esac
done < "$work/links"

# The adapter, the assets and the manifest, as AGS checks them after extraction.
[ -f "$root/$adapter" ] && [ -x "$root/$adapter" ] && [ ! -L "$root/$adapter" ] ||
	{ echo "error: $adapter is not an executable file" >&2; exit 1; }
for key in icon logo badge; do
	path=$(sed -n "s/^$key = \"\\([^\"]*\\)\"\$/\\1/p" "$manifest" | head -n 1)
	[ -n "$path" ] && [ -f "$root/$path" ] && [ ! -L "$root/$path" ] ||
		{ echo "error: asset $key ($path) is not a regular file" >&2; exit 1; }
	[ "$(stat -c %s -- "$root/$path")" -le 2097152 ] || { echo "error: asset $path is larger than 2 MiB" >&2; exit 1; }
done
cmp -s -- "$manifest" "$root/ags-extension.toml" || { echo "error: installed manifest differs from the build manifest" >&2; exit 1; }

# Deterministic archive: sorted names, fixed owner, times and permission bits, GNU format (no pax time headers).
epoch=${SOURCE_DATE_EPOCH:-0}
tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner --mode=u+rwX,go+rX,go-w --mtime="@$epoch" \
	-C "$root" -cf - . | zstd -19 -T0 -q -f -o "$work/$asset"
(cd "$work" && sha256sum -- "$asset" > "$asset.sha256")
mv -f -- "$work/$asset" "$work/$asset.sha256" "$out/"
echo "$out/$asset"
echo "$out/$asset.sha256"
