#!/bin/sh
# Embed the existing, attributed icon assets; no Rust files or network access are build inputs.
set -eu
export LC_ALL=C
printf '#include "ui/icon_data.h"\n'
icon_names=$(
    for path in assets/icons/*.svg; do
        name=${path##*/}
        name=${name%.svg}
        case "$name" in *[!A-Za-z0-9_-]*) echo "invalid icon name: $name" >&2; exit 1;; esac
        printf '%s\n' "$name"
    done | sort
)
index=0
for name in $icon_names; do
    printf 'static const uint8_t icon_%s[] = {\n' "$index"
    od -An -v -tu1 "assets/icons/$name.svg" | awk '{for(i=1;i<=NF;i++) printf "%s,",$i;printf "\n"}'
    printf '};\n'
    index=$((index+1))
done
printf 'const pc_ui_embedded_icon pc_ui_embedded_icons[] = {\n'
index=0
for name in $icon_names; do
    printf '{"%s",icon_%s,sizeof icon_%s},\n' "$name" "$index" "$index"
    index=$((index+1))
done
printf '};\nconst size_t pc_ui_embedded_icons_count = sizeof pc_ui_embedded_icons / sizeof *pc_ui_embedded_icons;\n'
