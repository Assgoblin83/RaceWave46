#!/usr/bin/env bash
# Builds and installs the RaceWave46 Flatpak for the current user from this
# source tree and your own ROM. See BUILDING.md.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
manifest="$here/io.github.DomazinUS.RaceWave46.yml"
distributable_manifest="$here/io.github.Assgoblin83.RaceWave46.distributable.yml"
rom=''
install=1
distributable=0
app_id=''
builder_arguments=()

usage() {
    cat <<'EOF'
Usage: packaging/linux/flatpak/build.sh --rom FILE [options]
       packaging/linux/flatpak/build.sh --distributable [options]

  --rom FILE        Your unmodified Wave Race 64 USA Rev 1 ROM (.z64)
  --distributable   Build the package without any ROM-derived code. The player
                    supplies the ROM on first start and the game module is
                    generated locally. --rom is not accepted.
  --app-id ID       Application id for the package. A copy of the manifest and
                    of the desktop/metainfo files is generated for ID; the
                    checked-in files are not modified.
  --no-install      Build only; do not install for the current user

The ROM is copied into the ignored .flatpak-input directory for the build and
is used only to generate code; it is not installed. The default package
contains code generated from your ROM, so do not redistribute it. Use
--distributable for a package that can be shared.

Pass flatpak-builder options after --.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --rom) rom="$2"; shift 2 ;;
        --distributable) distributable=1; shift ;;
        --app-id) app_id="$2"; shift 2 ;;
        --no-install) install=0; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; builder_arguments+=("$@"); break ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$distributable" -eq 1 ]; then
    if [ -n "$rom" ]; then
        echo '--rom cannot be combined with --distributable.' >&2
        exit 2
    fi
    manifest="$distributable_manifest"
elif [ -z "$rom" ]; then
    usage >&2
    exit 2
fi

if [ -n "$app_id" ] && ! [[ "$app_id" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_-]+)+$ ]]; then
    echo "Invalid application id: $app_id" >&2
    exit 2
fi

if [ "$distributable" -eq 0 ]; then
    python3 "$root/tools/verify_rom.py" "$rom" ||
        { echo 'The ROM must be an unmodified, big-endian USA Rev 1 ROM (.z64).' >&2; exit 1; }

    input="$root/.flatpak-input"
    mkdir -p "$input"
    trap 'rm -f "$input/rom.z64"' EXIT
    install -m 600 "$rom" "$input/rom.z64"
fi

# A different application id needs matching desktop, metainfo and icon names.
# Generate them beside the ignored build input so the checked-in files keep
# the upstream id, and point a manifest copy at the generated names.
if [ -n "$app_id" ]; then
    baseline="$(sed -n 's/^app-id:[[:space:]]*//p' "$manifest" | head -n 1)"
    if [ -z "$baseline" ]; then
        echo "Cannot read app-id from $manifest." >&2
        exit 1
    fi
    generated="$root/.flatpak-input/appid"
    rm -rf "$generated"
    mkdir -p "$generated"

    for extension in desktop metainfo.xml; do
        sed "s/io\\.github\\.DomazinUS\\.RaceWave46/$app_id/g" \
            "$root/packaging/linux/io.github.DomazinUS.RaceWave46.$extension" \
            > "$generated/$app_id.$extension"
    done

    generated_manifest="$root/.flatpak-input/$app_id.yml"
    sed -e "s#packaging/linux/io\\.github\\.DomazinUS\\.RaceWave46\\.desktop#$app_id.desktop#g" \
        -e "s#packaging/linux/io\\.github\\.DomazinUS\\.RaceWave46\\.metainfo\\.xml#$app_id.metainfo.xml#g" \
        -e "s/${baseline//./\\.}/$app_id/g" \
        -e "s#path: \\.\\./\\.\\./\\.\\.#path: $root#g" \
        -e "s#^      - librashader-cargo-sources\\.json#      - $root/packaging/linux/flatpak/librashader-cargo-sources.json#" \
        "$manifest" > "$generated_manifest"
    cat >> "$generated_manifest" <<EOF
      - type: file
        path: appid/$app_id.desktop
        dest-filename: $app_id.desktop
      - type: file
        path: appid/$app_id.metainfo.xml
        dest-filename: $app_id.metainfo.xml
EOF
    manifest="$generated_manifest"
fi

for ref in org.freedesktop.Sdk//26.08 org.freedesktop.Platform//26.08 \
    org.freedesktop.Sdk.Extension.rust-stable//26.08 org.freedesktop.Sdk.Extension.llvm22//26.08; do
    flatpak info "$ref" >/dev/null 2>&1 ||
        flatpak install --user -y --noninteractive flathub "$ref"
done

arguments=(--force-clean --state-dir="$root/.flatpak-builder" "${builder_arguments[@]}")
if [ "$install" -eq 1 ]; then arguments+=(--user --install); fi
flatpak-builder "${arguments[@]}" "$root/build-flatpak" "$manifest"
