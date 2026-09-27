# FreeSWITCH Debian packages for Debian Buster, built from this branch.
#
# Buster is EOL, so apt points at archive.debian.org and the
# SignalWire-only dependencies (sofia-sip, spandsp, libks,
# signalwire-client-c, classic iLBC) are rebuilt from source as local .debs.
#
# Do not build this directly; use ./build.sh, which stages the source
# (.build/freeswitch) and the seeded lib tarballs (dl/) in this directory.
#
#   ./build.sh                  # full build -> ./debs/
#   ./build.sh --target deps-out  # dependency .debs only -> ./debs/

ARG BUSTER_IMAGE=debian:buster-20240513
ARG NPROC=16

# ---------------------------------------------------------------- base
FROM ${BUSTER_IMAGE} AS base
SHELL ["/bin/bash", "-c"]
ENV DEBIAN_FRONTEND=noninteractive

RUN printf 'deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian buster-updates main\ndeb http://archive.debian.org/debian-security buster/updates main\n' > /etc/apt/sources.list \
 && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99-no-check-valid \
 && apt-get -q update \
 && apt-get -yq install --no-install-recommends \
        autoconf \
        automake \
        apt-transport-https \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        debhelper \
        devscripts \
        dh-autoreconf \
        dos2unix \
        doxygen \
        equivs \
        git \
        graphviz \
        libglib2.0-dev \
        libssl-dev \
        libtool \
        lsb-release \
        patch \
        pkg-config \
        wget \
 && apt-get clean \
 && update-ca-certificates --fresh \
 && git config --global --add safe.directory '*'

# Pre-install the JDK in two steps: installing default-jdk together with
# ca-certificates-java in one transaction trips a trigger-ordering bug
# (the jks-keystore hook runs before the JRE is configured). Installing
# -headless first avoids it; the full JDK on top then configures cleanly.
RUN apt-get -yq install --no-install-recommends default-jdk-headless \
 || (dpkg --configure -a && apt-get -yq install -f && apt-get -yq install --no-install-recommends default-jdk-headless)
RUN apt-get -yq install --no-install-recommends default-jdk \
 || (dpkg --configure -a && apt-get -yq install -f && apt-get -yq install --no-install-recommends default-jdk)

# ---------------------------------------------------------------- deps
FROM base AS deps
ARG NPROC
ENV DEB_BUILD_OPTIONS="parallel=${NPROC}"
WORKDIR /src

# sofia-sip 1.13.17 (ships its own debian/ dir)
RUN git clone -q --depth 1 --branch v1.13.17 https://github.com/freeswitch/sofia-sip.git sofia-sip \
 && cd sofia-sip \
 && mk-build-deps --install --remove --tool "apt-get --yes --no-install-recommends" \
 && dpkg-buildpackage -b -us -uc \
 && cd /src \
 && dpkg -i libsofia-sip-ua0_*_amd64.deb libsofia-sip-ua-dev_*_amd64.deb

# spandsp 3.1.0 -> libspandsp3* (ships its own debian/ dir)
RUN git clone -q --depth 1 --branch v3.1.0 https://github.com/freeswitch/spandsp.git spandsp \
 && cd spandsp \
 && mk-build-deps --install --remove --tool "apt-get --yes --no-install-recommends" \
 && dpkg-buildpackage -b -us -uc \
 && cd /src \
 && dpkg -i libspandsp3_*_amd64.deb libspandsp3-dev_*_amd64.deb

# libks 2.0.5 (local debian/ dir, see pkg/libks2)
COPY pkg/libks2/debian /pkg/libks2/debian
RUN git clone -q https://github.com/signalwire/libks.git libks \
 && cd libks && git checkout -q v2.0.5 && cd /src \
 && cp -r /pkg/libks2/debian libks/debian \
 && cd libks \
 && mk-build-deps --install --remove --tool "apt-get --yes --no-install-recommends" \
 && dpkg-buildpackage -b -us -uc \
 && cd /src \
 && dpkg -i libks2_*_amd64.deb

# signalwire-client-c 2.0.3 (local debian/ dir, see pkg/signalwire-client-c2)
COPY pkg/signalwire-client-c2/debian /pkg/signalwire-client-c2/debian
RUN git clone -q https://github.com/signalwire/signalwire-c.git signalwire-c \
 && cd signalwire-c && git checkout -q v2.0.3 && cd /src \
 && cp -r /pkg/signalwire-client-c2/debian signalwire-c/debian \
 && cd signalwire-c \
 && mk-build-deps --install --remove --tool "apt-get --yes --no-install-recommends" \
 && dpkg-buildpackage -b -us -uc \
 && cd /src \
 && dpkg -i signalwire-client-c2_*_amd64.deb

# Classic iLBC (the API mod_ilbc needs), extracted from FreeSWITCH's own
# history (libs/ilbc as of the commit that moved it to a system lib).
# Fresh local debian/ dir; the stale 2008 one in the tarball is dropped.
COPY pkg/ilbc-classic/ilbc-classic.tar.gz /pkg/ilbc-classic.tar.gz
COPY pkg/ilbc-classic/debian /pkg/ilbc-classic/debian
COPY pkg/ilbc-classic/ilbc.pc /pkg/ilbc-classic/ilbc.pc
RUN mkdir -p ilbc && tar -xzf /pkg/ilbc-classic.tar.gz -C ilbc --strip-components=2 \
 && rm -rf ilbc/debian \
 && cp -r /pkg/ilbc-classic/debian ilbc/debian \
 && cp /pkg/ilbc-classic/ilbc.pc ilbc/ilbc.pc \
 && cd ilbc \
 && mk-build-deps --install --remove --tool "apt-get --yes --no-install-recommends" \
 && dpkg-buildpackage -b -us -uc \
 && cd /src \
 && dpkg -i libilbc0_*_amd64.deb libilbc-dev_*_amd64.deb

RUN mkdir -p /out && cp /src/*.deb /out/ && ls /out

FROM scratch AS deps-out
COPY --from=deps /out /

# ---------------------------------------------------------------- freeswitch
FROM deps AS fs
ARG NPROC
ARG PKG_VERSION=1.10.12-4~buster1
ARG PKG_CHANGELOG="Rebuild for Debian Buster from the v1.10.12-opus-3064 branch"
ENV DEB_BUILD_OPTIONS="parallel=${NPROC}"
WORKDIR /data
# Shallow clone of the branch made by build.sh; keeping .git lets the build
# stamp the commit into the FreeSWITCH version string.
COPY .build/freeswitch/ /data/
COPY dl/ /seed/

RUN cp /seed/*.tar.gz libs/ \
 && cd debian && ./bootstrap.sh -c buster && cd /data \
 && mk-build-deps --install --remove debian/control --tool "apt-get --yes --no-install-recommends" \
 && apt-get --yes --fix-broken install \
 && dch -b -M -v "${PKG_VERSION}" --force-distribution -D buster "${PKG_CHANGELOG}" \
 && dpkg-buildpackage -b -us -uc \
 && cp /*.deb /out/ && ls /out

FROM scratch AS export
COPY --from=fs /out /
