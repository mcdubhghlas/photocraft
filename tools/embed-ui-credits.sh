#!/bin/sh
# Compile the recorded contributor data and consent records into the native app.
set -eu
printf '#include "ui/credits.h"\n'
for entry in 'contributors_json contributors/contributors.json' 'people_toml contributors/people.toml'; do
    set -- $entry
    printf 'const unsigned char pc_ui_%s[] = {\n' "$1"
    od -An -v -tu1 "$2" | awk '{for(i=1;i<=NF;i++) printf "%s,",$i;printf "\n"}'
    printf '};\nconst size_t pc_ui_%s_len = sizeof pc_ui_%s;\n' "$1" "$1"
done
