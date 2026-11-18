#!/bin/bash
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install all build and test dependencies of Manifold on the current distribution.
# This script runs as root when the CI container image is built.
#
set -euo pipefail

# the BLAKE3 C library, which we build from source if the distribution has no package for it
BLAKE3_VERSION=1.8.7
BLAKE3_SHA256=c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c

# libfyaml for the AppStream subproject, which we build from source if the distribution
# has no package for it
LIBFYAML_VERSION=0.9.6
LIBFYAML_SHA256=a59cc3331e2eb903ec36933ad52a45888041cac31e44f553a00511131242c483

# build and install a CMake project from a tarball, $3 is the source directory in the tarball
install_cmake_tarball()
{
    local url=$1
    local sha256=$2
    local source_dir=$3
    local workdir

    shift 3
    workdir=$(mktemp -d)
    curl -fsSL -o "$workdir/source.tar.gz" "$url"
    echo "$sha256  $workdir/source.tar.gz" | sha256sum --check --quiet
    tar -xzf "$workdir/source.tar.gz" -C "$workdir" --strip-components=1

    cmake -S "$workdir/$source_dir" -B "$workdir/build" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        "$@"
    cmake --build "$workdir/build"
    cmake --install "$workdir/build"
    ldconfig

    rm -rf "$workdir"
}

# shellcheck source=/dev/null
. /etc/os-release

case " $ID ${ID_LIKE:-} " in
    *" debian "* | *" ubuntu "*)
        export DEBIAN_FRONTEND=noninteractive
        install=(apt-get install -yq --no-install-recommends)
        packages=(
            build-essential
            meson
            ninja-build
            pkgconf
            git
            ca-certificates
            python3
            systemd
            systemd-dev
            libsystemd-dev
            libglib2.0-dev
            libcurl4-openssl-dev
            libappstream-dev
            libzip-dev
            libzck-dev
            libzstd-dev
            libcryptsetup-dev
            libelf-dev
            gobject-introspection
            libgirepository1.0-dev
            b3sum
            openssh-client
            # for the AppStream subproject, if the distribution's version is too old
            gettext
            gperf
            itstool
            libfyaml-dev
            libxml2-dev
            libxmlb-dev
            shared-mime-info
            # the tests make real images, and read from them
            erofs-utils
            # the tests make certificates and compress repository files with these
            openssl
            zstd
        )
        source_build_packages=(cmake curl)

        apt-get update -q
        # not every release has a BLAKE3 package, we build the library from source if it is missing
        if apt-cache show libblake3-dev > /dev/null 2>&1; then
            packages+=(libblake3-dev)
        fi
        ;;

    *" fedora "* | *" rhel "*)
        install=(dnf install -y)
        # we request libraries by their pkg-config name, which is the same on all of these distributions
        packages=(
            gcc
            meson
            ninja-build
            pkgconfig
            git
            python3
            systemd
            util-linux
            shadow-utils
            'pkgconfig(glib-2.0)'
            'pkgconfig(gobject-2.0)'
            'pkgconfig(gio-2.0)'
            'pkgconfig(libcurl)'
            'pkgconfig(appstream)'
            'pkgconfig(libsystemd)'
            'pkgconfig(systemd)'
            'pkgconfig(udev)'
            'pkgconfig(libzip)'
            'pkgconfig(zck)'
            'pkgconfig(libzstd)'
            'pkgconfig(libcryptsetup)'
            'pkgconfig(libelf)'
            'pkgconfig(gobject-introspection-1.0)'
            'pkgconfig(libblake3)'
            b3sum
            openssh-clients
            gcc-c++
            # for the AppStream subproject, if the distribution's version is too old
            gettext
            gperf
            itstool
            'pkgconfig(libxml-2.0)'
            'pkgconfig(xmlb)'
            # the tests make real images, and read from them
            erofs-utils
            # the tests make certificates and compress repository files with these
            openssl
            zstd
        )
        source_build_packages=(cmake curl)

        # the enterprise distributions only have some of the packages in CRB and EPEL,
        # and provide a newer GCC than their default one as a toolset (see build-and-test.sh)
        if [ "$ID" != fedora ]; then
            "${install[@]}" dnf-plugins-core epel-release
            dnf config-manager --set-enabled crb
            # libblake3 of the enterprise distributions needs the libstdc++ of the toolset for linking
            packages+=(gcc-toolset-15-gcc gcc-toolset-15-libstdc++-devel)
        else
            # the enterprise distributions have no libfyaml package, we build it from source there
            packages+=('pkgconfig(libfyaml)')
        fi
        ;;

    *" arch "*)
        install=(pacman -S --noconfirm --needed)
        packages=(
            base-devel
            meson
            git
            python
            glib2
            glib2-devel
            curl
            appstream
            systemd
            libzip
            zchunk
            zstd
            cryptsetup
            libelf
            gobject-introspection
            libblake3
            b3sum
            openssh
            erofs-utils
            openssl
        )
        source_build_packages=(cmake)

        pacman -Syu --noconfirm
        ;;

    *)
        echo "Unknown distribution: $ID" >&2
        exit 1
        ;;
esac

"${install[@]}" "${packages[@]}"

if ! pkg-config --exists libblake3 libfyaml; then
    "${install[@]}" "${source_build_packages[@]}"
fi
if ! pkg-config --exists libblake3; then
    install_cmake_tarball \
        "https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/$BLAKE3_VERSION.tar.gz" \
        "$BLAKE3_SHA256" \
        c
fi
if ! pkg-config --exists libfyaml; then
    install_cmake_tarball \
        "https://github.com/pantoniou/libfyaml/releases/download/v$LIBFYAML_VERSION/libfyaml-$LIBFYAML_VERSION.tar.gz" \
        "$LIBFYAML_SHA256" \
        . \
        -DBUILD_TESTING=OFF \
        -DENABLE_LIBCLANG=OFF \
        -DENABLE_NETWORK=OFF
fi
