#!/usr/bin/env bash
# Prints, comma separated, the fan module files a program needs: the modules it imports and, through their
# sources and _impl.cpp files, every fan module those import in turn. fan's xmake option FAN_ONLY takes the list
# and builds only those, instead of all of fan's core and fan.ixx.
#
# Usage: fan-modules.sh <fan-src> <source files...>
set -euo pipefail

fan_src=$1
shift

# `import fan.x;` and `export import fan.x;` in the given files
imports_of() {
  grep -hoE '(^|[^A-Za-z0-9_])import[[:space:]]+[A-Za-z0-9_.]+[[:space:]]*;' "$@" 2>/dev/null |
    sed -E 's/.*import[[:space:]]+([^ ;]+).*/\1/' || true
}

# every module's name and file, from its `export module` line
declare -A file_of
while IFS= read -r file; do
  name=$(grep -m1 -oE '^[[:space:]]*export[[:space:]]+module[[:space:]]+[A-Za-z0-9_.]+[[:space:]]*;' "$fan_src/$file" |
    sed -E 's/.*module[[:space:]]+([^ ;]+).*/\1/' || true)
  if [[ -n $name ]]; then
    file_of[$name]=$file
  fi
done < <(cd "$fan_src" && find fan -name '*.ixx' | sort)

declare -A seen
queue=()
for name in $(imports_of "$@" | sort -u); do
  [[ -n ${file_of[$name]:-} ]] && queue+=("$name")
done
[[ ${#queue[@]} -gt 0 ]] || { echo "fan-modules.sh: no fan module imported by $*" >&2; exit 1; }

files=()
while [[ ${#queue[@]} -gt 0 ]]; do
  name=${queue[-1]}
  unset 'queue[-1]'
  [[ -n ${seen[$name]:-} ]] && continue
  seen[$name]=1
  file=${file_of[$name]}
  files+=("$file")
  impl="$fan_src/${file%.ixx}_impl.cpp"
  sources=("$fan_src/$file")
  [[ -f $impl ]] && sources+=("$impl")
  for dep in $(imports_of "${sources[@]}" | sort -u); do
    if [[ -n ${file_of[$dep]:-} && -z ${seen[$dep]:-} ]]; then
      queue+=("$dep")
    fi
  done
done

printf '%s\n' "${files[@]}" | sort | paste -sd, -
