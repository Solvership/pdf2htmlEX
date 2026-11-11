#!/bin/bash
#
# Local development helper for pdf2htmlEX (runs everything inside Docker).
#
#   scripts/dev.sh image      build the dev docker image
#   scripts/dev.sh deps       download + build Poppler and FontForge (once)
#   scripts/dev.sh build      (re)build pdf2htmlEX incrementally
#   scripts/dev.sh run ARGS   run the freshly built pdf2htmlEX
#   scripts/dev.sh shell      interactive shell in the container
#   scripts/dev.sh exec CMD   run an arbitrary command in the container
#
# Everything is built inside the repository (git-ignored dirs), as the
# current user, so the build tree is reusable between runs.

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PDF2HTMLEX_DEV_IMAGE:-pdf2htmlex-dev:bookworm}"
BUILD_DIR="pdf2htmlEX/build"
DATA_DIR="/work/pdf2htmlEX/build/install/usr/local/share/pdf2htmlEX"

in_docker() {
    local tty=""
    [ -t 0 ] && tty="-it"
    docker run --rm $tty \
        --user "$(id -u):$(id -g)" \
        -v "$REPO:/work" -w /work \
        -e MAKE_PARALLEL="-j $(nproc)" \
        -e PDF2HTMLEX_PREFIX=/usr/local \
        -e PDF2HTMLEX_BRANCH="$(git -C "$REPO" rev-parse --abbrev-ref HEAD)" \
        "$IMAGE" bash -c "$*"
}

case "$1" in
    image)
        docker build -t "$IMAGE" -f "$REPO/docker/Dockerfile.dev" "$REPO/docker"
        ;;
    deps)
        in_docker '
            set -e
            ./buildScripts/versionEnvs >/dev/null
            if [ ! -f poppler/build/libpoppler.a ]; then
                rm -rf poppler poppler-data
                ./buildScripts/getPoppler >/dev/null
                ./buildScripts/buildPoppler
            fi
            if [ ! -f fontforge/build/lib/libfontforge.a ]; then
                rm -rf fontforge
                ./buildScripts/getFontforge >/dev/null
                ./buildScripts/buildFontforge
            fi
        '
        ;;
    build)
        in_docker "
            set -e
            . ./buildScripts/versionEnvs >/dev/null
            mkdir -p $BUILD_DIR && cd $BUILD_DIR
            [ -f Makefile ] || cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..
            make \$MAKE_PARALLEL
            # install into the build tree, so that the data dir is complete
            make install DESTDIR=\$PWD/install >/dev/null
            [ -d install/usr/local/share/pdf2htmlEX/poppler ] || make -C /work/poppler-data install DESTDIR=\$PWD/install prefix=/usr/local datadir=/usr/local/share/pdf2htmlEX >/dev/null
        "
        ;;
    run)
        shift
        in_docker "pdf2htmlEX/build/pdf2htmlEX --data-dir $DATA_DIR --poppler-data-dir $DATA_DIR/poppler $*"
        ;;
    shell)
        docker run --rm -it --user "$(id -u):$(id -g)" -v "$REPO:/work" -w /work "$IMAGE" bash
        ;;
    exec)
        shift
        in_docker "$*"
        ;;
    *)
        sed -n '3,12p' "$0"
        exit 1
        ;;
esac
