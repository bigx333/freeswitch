#!/bin/bash
# Runs inside the deps image (see test.sh): minimal core build + unit tests.
set -euo pipefail
cd /fs

apt-get -o Acquire::Check-Valid-Until=false -qq update >/dev/null
apt-get -yqq install --no-install-recommends libtool-bin libsqlite3-dev libcurl4-openssl-dev \
  libspeexdsp-dev libspeex-dev libldns-dev libedit-dev libopus-dev yasm nasm pkg-config \
  libpcre3-dev >/dev/null

cat > modules.conf <<'MODS'
loggers/mod_console
applications/mod_commands
applications/mod_dptools
applications/mod_spandsp
applications/mod_test
codecs/mod_opus
dialplans/mod_dialplan_xml
endpoints/mod_loopback
endpoints/mod_sofia
formats/mod_tone_stream
MODS

if [ ! -f config.status ]; then
  ./bootstrap.sh -j >/tmp/bootstrap.log 2>&1 || { tail -30 /tmp/bootstrap.log; exit 1; }
  ./configure --prefix=/usr/local/freeswitch --disable-libvpx --disable-libyuv \
    >/tmp/configure.log 2>&1 || { tail -30 /tmp/configure.log; exit 1; }
fi
make -j"$(nproc)" >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
make install >/tmp/install.log 2>&1 || { tail -20 /tmp/install.log; exit 1; }
status=0
for t in "$@"; do
  echo "== $t"
  if [ "$t" = sofia ]; then
    make -C src/mod/endpoints/mod_sofia -j"$(nproc)" test/test_sofia_funcs >/tmp/tests-make.log 2>&1 || { tail -40 /tmp/tests-make.log; exit 1; }
    (cd src/mod/endpoints/mod_sofia/test && timeout 90 ./test_sofia_funcs) > "/tmp/$t.log" 2>&1 || status=1
  else
    make -C tests/unit -j"$(nproc)" "$t" >/tmp/tests-make.log 2>&1 || { tail -40 /tmp/tests-make.log; exit 1; }
    (cd tests/unit && timeout 90 ./"$t") > "/tmp/$t.log" 2>&1 || status=1
  fi
  grep -E "^(PASSED|FAILED)|TEST FAIL" "/tmp/$t.log" || { tail -20 "/tmp/$t.log"; status=1; }
done
exit $status
