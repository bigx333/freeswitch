# Debian Buster packages

Tooling to build FreeSWITCH `.deb` packages for Debian 10 (Buster, amd64) from
this branch, plus the SignalWire-only dependencies that have no Buster
packages: sofia-sip 1.13.17, spandsp 3.1.0, libks 2.0.5,
signalwire-client-c 2.0.3 and classic iLBC.

Everything runs in Docker (`sudo docker buildx`); the host needs only Docker,
git and curl.

## Build

```sh
./build.sh                      # all .debs -> debs/
./build.sh --bundle             # also dist/freeswitch-<version>-<commit>.tar.gz
./build.sh --target deps-out    # dependency .debs only
PKG_VERSION=1.10.12-4~buster1 ./build.sh
```

`build.sh` builds the **committed** `HEAD` (a shallow clone in `.build/`), so
the commit hash ends up in the FreeSWITCH version string. Bump `PKG_VERSION`
(default `1.10.12-4~buster1`) for every release you ship so `apt`/`dpkg` see an
upgrade. `1.10.12-2~buster1` was the first Opus backport build.

`--bundle` writes a transfer tarball with the `.debs`, `SHA256SUMS` and
`INSTALL.txt` (from `INSTALL.txt.in`).

## Test

```sh
./test.sh                       # switch_opus switch_rtp switch_core_codec
./test.sh switch_opus
```

Builds the Dockerfile's `deps` stage as the `fs-buster-deps` image, then does a
minimal core build (console, commands, dptools, spandsp, opus, loopback) of the
committed `HEAD` in `.build/test-src` and runs the given unit tests. Later runs
are incremental.

## Layout

| Path | Purpose |
|---|---|
| `Dockerfile` | `base` → `deps` (dependency .debs) → `fs` (FreeSWITCH .debs); `deps-out` / `export` stages copy the results out |
| `build.sh` | Stages the source and lib tarballs, runs the build, optional bundle |
| `fetch-libs.sh` | Downloads the `libs/` tarballs (sphinx, freeradius-client, communicator model) from files.freeswitch.org into `dl/`, checked by SHA-256 |
| `test.sh`, `run-unit-tests.sh` | Unit tests in the Buster deps image |
| `pkg/` | Local `debian/` directories for libks2, signalwire-client-c2 and ilbc-classic (source tarball extracted from FreeSWITCH history) |
| `INSTALL.txt.in` | Install notes shipped in the bundle |

Buster is EOL: apt uses `archive.debian.org` with `Check-Valid-Until` disabled.
