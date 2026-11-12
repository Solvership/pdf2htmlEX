#!/bin/bash
#
# Local development helper for pdf2htmlEX (runs everything inside Docker).
#
#   scripts/dev.sh image      build the dev docker image
#   scripts/dev.sh deps       download + build Poppler and FontForge (once)
#   scripts/dev.sh build      (re)build pdf2htmlEX incrementally
#   scripts/dev.sh run ARGS   run the freshly built pdf2htmlEX
#   scripts/dev.sh test       run the semantic table tests
#   scripts/dev.sh shell      interactive shell in the container
#   scripts/dev.sh exec CMD   run an arbitrary command in the container
#   scripts/dev.sh runtime-image [TAG]
#                             docker image with the built binary (docker/Dockerfile.runtime),
#                             default tag pdf2htmlex:semantic-<git short sha>
#
# VARIANT selects the tool chain / library versions:
#   VARIANT=bookworm (default)  Debian 12, Poppler 24.06.1, FontForge 20230101
#   VARIANT=trixie              Debian 13, Poppler 26.11.0, FontForge 20251009
#
# Everything is built inside the repository (git-ignored dirs), as the
# current user, so the build tree is reusable between runs.

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VARIANT="${VARIANT:-bookworm}"

case "$VARIANT" in
    bookworm)
        POPPLER_VERSION=24.06.1
        FONTFORGE_VERSION=20230101
        BUILD_DIR="pdf2htmlEX/build"
        ;;
    trixie)
        POPPLER_VERSION=26.11.0
        FONTFORGE_VERSION=20251009
        BUILD_DIR="pdf2htmlEX/build-trixie"
        ;;
    *)
        echo "unknown VARIANT $VARIANT" >&2
        exit 1
        ;;
esac
IMAGE="${PDF2HTMLEX_DEV_IMAGE:-pdf2htmlex-dev:$VARIANT}"
POPPLER_DIR="poppler-$POPPLER_VERSION"
FONTFORGE_DIR="fontforge-$FONTFORGE_VERSION"
DATA_DIR="/work/$BUILD_DIR/install/usr/local/share/pdf2htmlEX"

in_docker() {
    local tty=""
    [ -t 0 ] && tty="-it"
    docker run --rm $tty \
        --user "$(id -u):$(id -g)" \
        -v "$REPO:/work" -w /work \
        -e MAKE_PARALLEL="-j $(nproc)" \
        -e PDF2HTMLEX_PREFIX=/usr/local \
        -e PDF2HTMLEX_BRANCH="$(git -C "$REPO" rev-parse --abbrev-ref HEAD)" \
        -e SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}" \
        -e P2H_BUILD_DIR="/work/$BUILD_DIR" \
        "$IMAGE" bash -c "$*"
}

case "$1" in
    image)
        docker build --build-arg BASE="debian:$VARIANT" -t "$IMAGE" -f "$REPO/docker/Dockerfile.dev" "$REPO/docker"
        ;;
    deps)
        # the build scripts work on ./poppler and ./fontforge: point them to versioned directories
        in_docker "
            set -e
            if [ ! -f $POPPLER_DIR/build/libpoppler.a ]; then
                rm -rf $POPPLER_DIR
                wget -q https://poppler.freedesktop.org/poppler-$POPPLER_VERSION.tar.xz
                tar xf poppler-$POPPLER_VERSION.tar.xz && rm poppler-$POPPLER_VERSION.tar.xz
                sed -n '/^cmake/,/^make/p' buildScripts/buildPoppler > /tmp/poppler-cmake.sh
                (cd $POPPLER_DIR && mkdir -p build && cd build && PDF2HTMLEX_PREFIX=/usr/local bash -e /tmp/poppler-cmake.sh)
            fi
            if [ ! -d poppler-data ]; then
                wget -q https://poppler.freedesktop.org/poppler-data-0.4.12.tar.gz
                tar xf poppler-data-0.4.12.tar.gz && rm poppler-data-0.4.12.tar.gz && mv poppler-data-0.4.12 poppler-data
            fi
            if [ ! -f $FONTFORGE_DIR/build/lib/libfontforge.a ]; then
                rm -rf $FONTFORGE_DIR
                wget -q https://github.com/fontforge/fontforge/archive/$FONTFORGE_VERSION.tar.gz
                tar xf $FONTFORGE_VERSION.tar.gz && rm $FONTFORGE_VERSION.tar.gz
                mv fontforge-$FONTFORGE_VERSION $FONTFORGE_DIR 2>/dev/null || true
                sed -n '/^cmake/,/^make/p' buildScripts/buildFontforge > /tmp/fontforge-cmake.sh
                (cd $FONTFORGE_DIR && mkdir -p build && cd build && PDF2HTMLEX_PREFIX=/usr/local bash -e /tmp/fontforge-cmake.sh)
            fi
        "
        ;;
    build)
        in_docker "
            set -e
            . ./buildScripts/versionEnvs >/dev/null
            mkdir -p $BUILD_DIR && cd $BUILD_DIR
            [ -f Makefile ] || cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
                -DPOPPLER_DIR=/work/$POPPLER_DIR -DFONTFORGE_DIR=/work/$FONTFORGE_DIR ..
            make \$MAKE_PARALLEL
            # install into the build tree, so that the data dir is complete
            make install DESTDIR=\$PWD/install >/dev/null
            [ -d install/usr/local/share/pdf2htmlEX/poppler ] || make -C /work/poppler-data install DESTDIR=\$PWD/install prefix=/usr/local datadir=/usr/local/share/pdf2htmlEX >/dev/null
        "
        ;;
    run)
        shift
        in_docker "$BUILD_DIR/pdf2htmlEX --data-dir $DATA_DIR --poppler-data-dir $DATA_DIR/poppler $*"
        ;;
    test)
        in_docker "python3 pdf2htmlEX/test/tables/test_tables.py"
        ;;
    shell)
        docker run --rm -it --user "$(id -u):$(id -g)" -v "$REPO:/work" -w /work \
            -e P2H_BUILD_DIR="/work/$BUILD_DIR" "$IMAGE" bash
        ;;
    exec)
        shift
        in_docker "$*"
        ;;
    runtime-image)
        TAG="${2:-pdf2htmlex:semantic-$(git -C "$REPO" rev-parse --short HEAD)}"
        [ "$VARIANT" = bookworm ] || echo "note: VARIANT=$VARIANT, the runtime image needs BASE=debian:$VARIANT-slim" >&2
        [ -x "$REPO/$BUILD_DIR/pdf2htmlEX" ] || { echo "no binary, run: scripts/dev.sh build" >&2; exit 1; }
        CTX="$(mktemp -d "$REPO/.runtime-image.XXXXXX")"
        trap 'rm -rf "$CTX"' EXIT
        cp "$REPO/$BUILD_DIR/pdf2htmlEX" "$CTX/"
        cp -r "$REPO/$BUILD_DIR/install/usr/local/share/pdf2htmlEX" "$CTX/share"
        docker build --build-arg BASE="debian:$VARIANT-slim" -t "$TAG" -f "$REPO/docker/Dockerfile.runtime" "$CTX"
        # smoke test: convert one of the table test documents with the semantic layer
        SMOKE="$(mktemp -d "$REPO/.runtime-smoke.XXXXXX")"
        trap 'rm -rf "$CTX" "$SMOKE"' EXIT
        cp "$REPO/pdf2htmlEX/test/tables/financial.pdf" "$SMOKE/"
        docker run --rm --user "$(id -u):$(id -g)" -v "$SMOKE:/pdf" -w /pdf "$TAG" \
            --semantic-tables 1 --semantic-output attrs,inline --quiet 1 financial.pdf
        grep -q 'data-tbl=' "$SMOKE/financial.html" || { echo "smoke test failed: no data-tbl in the output" >&2; exit 1; }
        echo "built $TAG (smoke test OK)"
        ;;
    *)
        sed -n '3,20p' "$0"
        exit 1
        ;;
esac
