#!/bin/sh

set -eux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

. "${srcdir}"/pgo/options

pgo_data=$(mktemp)
trap "rm -f '${pgo_data}'" EXIT INT HUP TERM

rm -f "${blddir}"/pgo-ok

# To ensure profiling data is generated in the build directory
cd "${blddir}"

LC_CTYPE=en_US.UTF-8 "${blddir}"/footclient --version
LC_CTYPE=en_US.UTF-8 "${blddir}"/foot \
        --config=/dev/null \
        --term=xterm \
        sh -c "
          set -eux

         '${srcdir}/scripts/generate-alt-random-writes.py' \
            ${script_options} \"${pgo_data}\"

         cat \"${pgo_data}\"
         "
touch "${blddir}"/pgo-ok
