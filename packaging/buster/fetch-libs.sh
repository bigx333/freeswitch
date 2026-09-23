#!/bin/bash
# Download the third-party tarballs the FreeSWITCH Debian build unpacks into
# libs/, verified by SHA-256. Seeding them avoids network fetches mid-build.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p dl

BASE=https://files.freeswitch.org/downloads/libs
while read -r sum file; do
  if [ -f "dl/$file" ] && echo "$sum  dl/$file" | sha256sum -c --quiet - 2>/dev/null; then
    continue
  fi
  echo "Fetching $file"
  curl -fsSL --retry 3 -o "dl/$file.part" "$BASE/$file"
  echo "$sum  dl/$file.part" | sha256sum -c --quiet -
  mv "dl/$file.part" "dl/$file"
done <<'SUMS'
dbb5e9fb85000a7cb97d6958a3ef8d77532dc55fc730ac6979705e8645cb0c18 communicator_semi_6000_20080321.tar.gz
eada2861b8f4928e3ac6b5bbfe11e92cd6cdcacfce40cae1085e77c1b6add0e9 freeradius-client-1.1.7.tar.gz
874c4c083d91c8ff26a2aec250b689e537912ff728923c141c4dac48662cce7a pocketsphinx-0.8.tar.gz
55708944872bab1015b8ae07b379bf463764f469163a8fd114cbb16c5e486ca8 sphinxbase-0.8.tar.gz
SUMS
