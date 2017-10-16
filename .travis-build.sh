#!/bin/bash

set -e
set -x

if grep -nr '\s$' src *.yml *.txt *.md Doxyfile .gitignore .gitmodules .travis* dist/*.desktop \
                 dist/*.svg dist/*.xml; then
    echo Trailing whitespace found, aborting
    exit 1
fi

# Only run clang-format on Linux because we don't have 4.0 on OS X images
if [ "$TRAVIS_OS_NAME" = "linux" ]; then
    # Default clang-format points to default 3.5 version one
    CLANG_FORMAT=clang-format-3.9
    $CLANG_FORMAT --version

    if [ "$TRAVIS_EVENT_TYPE" = "pull_request" ]; then
        # Get list of every file modified in this pull request
        files_to_lint="$(git diff --name-only --diff-filter=ACMRTUXB $TRAVIS_COMMIT_RANGE | grep '^src/[^.]*[.]\(cpp\|h\)$' || true)"
    else
        # Check everything for branch pushes
        files_to_lint="$(find src/ -name '*.cpp' -or -name '*.h')"
    fi

    # Turn off tracing for this because it's too verbose
    set +x

    for f in $files_to_lint; do
        d=$(diff -u "$f" <($CLANG_FORMAT "$f") || true)
        if ! [ -z "$d" ]; then
            echo "!!! $f not compliant to coding style, here is the fix:"
            echo "$d"
            fail=1
        fi
    done

    set -x

    if [ "$fail" = 1 ]; then
        exit 1
    fi
fi

#if OS is linux or is not set
if [ "$TRAVIS_OS_NAME" = "linux" -o -z "$TRAVIS_OS_NAME" ]; then
    docker run -v $(pwd):/citra ubuntu:16.04 /bin/bash /citra/.travis-build-docker.sh
elif [ "$TRAVIS_OS_NAME" = "osx" ]; then
    set -o pipefail

    export MACOSX_DEPLOYMENT_TARGET=10.9
    export Qt5_DIR=$(brew --prefix)/opt/qt5

    mkdir build && cd build
    cmake .. -DUSE_SYSTEM_CURL=ON -DCMAKE_OSX_ARCHITECTURES="x86_64;x86_64h" -DCMAKE_BUILD_TYPE=Release
    make -j4

    ctest -VV -C Release
fi
