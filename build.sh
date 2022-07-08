#!/bin/bash
if [[ $ANDROID_NDK_HOME == "" ]];then
    echo "ndk not found!"
    exit
elif [[ -z $ANDROID_NDK_HOME ]];then
    echo "ndk not exist!"
    exit
fi

ROOT_PATH=$(pwd)

PREFIX=${ROOT_PATH}/install/usr
mkdir -p ${PREFIX}

LIBCURL=https://github.com/curl/curl/releases/download/curl-7_84_0/curl-7.84.0.tar.gz
OPENSSL=https://www.openssl.org/source/openssl-3.0.0.tar.gz

ANDROID_ABI=26
ANDROID_ARCH=aarch64
TOOLCHAIN=${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64
TOOLCHAIN_BIN=${TOOLCHAIN}/bin
CC=$TOOLCHAIN_BIN/${ANDROID_ARCH}-linux-android${ANDROID_ABI}-clang
CXX=$TOOLCHAIN_BIN/${ANDROID_ARCH}-linux-android${ANDROID_ABI}-clang++
RANLIB=$TOOLCHAIN_BIN/llvm-ranlib
STRIP=$TOOLCHAIN_BIN/llvm-strip
AR=$TOOLCHAIN_BIN/llvm-ar
LD=$TOOLCHAIN_BIN/ld.lld
CFLAGS="-march=armv8-a -mtune=cortex-a78 -fuse-ld=lld"
CXXFLAGS="-I${PREFIX}/include"
LDFLAGS="-L${PREFIX}/lib"
PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig/
HOST="aarch64-linux-android"
TARGET="aarch64-linux-android"
BUILD="x86_64-unknown-linux-gnu"



function build_liblzma(){
    sudo apt install autopoint -y
    cd ${ROOT_PATH}
    cd liblzma
    file ./autogen.sh
    bash autogen.sh
    sudo chmod 755 ./configure
    PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./configure --prefix=${PREFIX} --host=${HOST} --target=${TARGET} --build=${BUILD} --enable-static
    make -j8
    make install
}
 
function build_openssl(){
    wget ${OPENSSL}
    tar xvf openssl-3.0.0.tar.gz
    cd openssl-3.0.0
    PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./Configure linux-aarch64 -fuse-ld=lld --prefix=${PREFIX} shared zlib zlib-dynamic
    make -j8
    make install_sw
}

function build_libcurl(){ 
    cd ${ROOT_PATH}
    wget ${LIBCURL}
    tar xvf curl-7.84.0.tar.gz
    cd curl-7.84.0
    PKG_CONFIG_PATH=${PKG_CONFIG_PATH} CFLAGS=${CFLAGS} CXXFLAGS=${CXXFLAGS} LDFLAGS=${LDFLAGS} CC=${CC} RANLIB=${RANLIB} CXX=${CXX} AR=${AR} LD=${LD} ./configure --prefix=${PREFIX} --host=${HOST} --target=${TARGET} --build=${BUILD} --with-openssl --enable-static
    make -j8
    make install
}


function build_mongod(){ 
    cd ${ROOT_PATH}
    cd mongodb-r5.0.3
    sudo apt install ninja-build -y
    python3 -m pip install -r etc/pip/compile-requirements.txt
    PKG_CONFIG_PATH={PKG_CONFIG_PATH} python3 buildscripts/scons.py install-mongod CC=${CC} CXX=${CXX} CCFLAGS=${CXXFLAGS}  LINKFLAGS="-L${PREFIX}/lib -ldl -lz -static -ffunction-sections -fdata-sections -Wl,--gc-sections" AR=${AR} --linker=lld --link-model=static DESTDIR=${PREFIX} --disable-warnings-as-errors TARGET_ARCH="aarch64" HOST_ARCH="x86_64" MONGO_VERSION="5.0.3" --ninja build.ninja
    ninja -j8
}

function strip_mongod(){
    cd ${ROOT_PATH}
    ${STRIP} install/usr/bin/mongod
}


case "$1" in
    build_liblzma)
        build_liblzma
        ;;
    build_openssl)
        build_openssl
        ;;
    build_libcurl)
        build_libcurl
        ;;
    build_mongod)
        build_mongod
        ;;
    strip_mongod)
        strip_mongod
        ;;
esac


#cat build/scons/config.log