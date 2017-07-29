#!/bin/bash
if [[ !( -f Makefile ) ]]; then
    mkdir -p linux-host || exit 1
    dst="`pwd`/linux-host"
    ./configure "--prefix=$dst" --enable-static --disable-shared --enable-extra-encodings || exit 1
fi
make -j5 || exit 1
make install || exit 1
