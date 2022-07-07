#!/bin/bash
if [[ $ANDROID_NDK_HOME == "" ]];then
    echo "ndk not found!"
    exit
elif [[ -z $ANDROID_NDK_HOME ]];then
    echo "ndk not exist!"
    exit
fi

ROOT_PATH=$(pwd)

LIBCURL=https://github.com/curl/curl/releases/download/curl-7_84_0/curl-7.84.0.tar.gz
OPENSSL=https://www.openssl.org/source/openssl-3.0.0.tar.gz

ANDROID_ABI=26
ANDROID_ARCH=aarch64
TOOLCHAIN=${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64
TOOLCHAIN_BIN=${TOOLCHAIN}/bin
CC=$TOOLCHAIN_BIN/${ANDROID_ARCH}-linux-android${ANDROID_ABI}-clang
CXX=$TOOLCHAIN_BIN/${ANDROID_ARCH}-linux-android${ANDROID_ABI}-clang++
RANLIB=$TOOLCHAIN_BIN/llvm-ranlib
AR=$TOOLCHAIN_BIN/llvm-ar
LD=$TOOLCHAIN_BIN/ld.lld
PREFIX=/home/usr
CFLAGS="-march=armv8-a -mtune=cortex-a78 -fuse-ld=lld"
CXXFLAGS="-I${PREFIX}/include"
LDFLAGS="-L${PREFIX}/lib"
PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig/
HOST="aarch64-linux-android"
TARGET="aarch64-linux-android"
BUILD="x86_64-unknown-linux-gnu"

sudo mkdir -p ${PREFIX}
chmod 755 -R ${PREFIX}

wget ${OPENSSL}
tar xvf openssl-3.0.0.tar.gz
cd openssl-3.0.0
PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./Configure linux-aarch64 -fuse-ld=lld --prefix=${PREFIX} shared zlib zlib-dynamic
make -j8
make install_sw

cd ${ROOT_PATH}
wget ${LIBCURL}
tar xvf curl-7.84.0.tar.gz
cd curl-7.84.0
PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./configure --prefix=${PREFIX} --host=${HOST} --target=${TARGET} --build=${BUILD} --with-openssl --enable-static
make -j8
make install


cd ${ROOT_PATH}
cd liblzma
PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./configure --prefix=${PREFIX} --host=${HOST} --target=${TARGET} --build=${BUILD} --enable-static
make -j8
make install
