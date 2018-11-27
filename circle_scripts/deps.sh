#! /bin/bash

_HOME_="$(pwd)"
export _HOME_

export _SRC_=$_HOME_/src/
export _INST_=$_HOME_/inst/


if [ "$1""x" == "cachex" ]; then
  echo "option: *CACHE*"
else
  echo "option: +NOcache+"
fi

export CF2=" -O3 -g"
export CF3=" "
export VV1=" VERBOSE=1 V=1 "

if [ "$1""x" != "cachex" ]; then
  echo "option: +NOcache+"
  # rm -Rfv $_SRC_
  # rm -Rfv $_INST_
fi

mkdir -p $_SRC_
mkdir -p $_INST_

export LD_LIBRARY_PATH=$_INST_/lib/
export PKG_CONFIG_PATH=$_INST_/lib/pkgconfig




# ------- ASTYLE 3.1 -------
cd "$_SRC_"
mkdir -p astyle
cd astyle/
wget -O astyle.tgz https://downloads.sourceforge.net/project/astyle/astyle/astyle%203.1/astyle_3.1_linux.tar.gz?r=https%3A%2F%2Fsourceforge.net%2Fprojects%2Fastyle%2Ffiles%2Fastyle%2Fastyle%25203.1%2Fastyle_3.1_linux.tar.gz%2Fdownload

tar -xzvf astyle.tgz > /dev/null 2>&1
mkdir -p build_astyle > /dev/null 2>&1
cd build_astyle/ > /dev/null 2>&1
cmake ../astyle/ > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1 || exit 1

export astyle_bin="$(pwd)/astyle"
"$astyle_bin" --version || exit 1
# ------- ASTYLE 3.1 -------



if [ "$1""x" != "cachex" ]; then

  echo "option: +NOcache+"

cd $_SRC_
# rm -Rf libav
git clone https://github.com/libav/libav
cd libav
git checkout v12.3
./configure --prefix=$_INST_ --disable-devices --disable-programs \
--disable-doc --disable-avdevice --disable-avformat \
--disable-swscale \
--disable-avfilter --disable-network --disable-everything \
--disable-bzlib \
--disable-libxcb-shm \
--disable-libxcb-xfixes \
--enable-parser=h264 \
--enable-runtime-cpudetect \
--enable-gpl --enable-decoder=h264
#make clean
make -j$(nproc) || exit 1
make install


cd $_SRC_
# rm -Rf x264
git clone git://git.videolan.org/x264.git
cd x264
git checkout 0a84d986e7020f8344f00752e3600b9769cc1e85 # stable
./configure --prefix=$_INST_ --disable-opencl --enable-shared --enable-static \
--disable-avs --disable-cli
#make clean
make -j$(nproc) || exit 1
make install


cd $_SRC_
git clone --depth=1 --branch=1.0.16 https://github.com/jedisct1/libsodium.git
cd libsodium
./autogen.sh
export CFLAGS=" $CF2 "
./configure --prefix=$_INST_ --disable-shared --disable-soname-versions
make -j$(nproc) || exit 1
make install

cd $_SRC_
git clone --depth=1 --branch=v1.7.0 https://github.com/webmproject/libvpx.git
cd libvpx
make clean
export CFLAGS=" $CF2 $CF3 "
export CXXFLAGS=" $CF2 $CF3 "
./configure --prefix=$_INST_ --disable-examples \
  --disable-unit-tests --enable-shared \
  --size-limit=16384x16384 \
  --enable-onthefly-bitpacking \
  --enable-error-concealment \
  --enable-runtime-cpu-detect \
  --enable-multi-res-encoding \
  --enable-postproc \
  --enable-vp9-postproc \
  --enable-temporal-denoising \
  --enable-vp9-temporal-denoising

#  --enable-better-hw-compatibility \

make -j$(nproc) || exit 1
make install

cd $_SRC_
git clone --depth=1 --branch=v1.3-rc https://github.com/xiph/opus.git
cd opus
./autogen.sh
export CFLAGS=" $CF2 $CF3 "
export CXXFLAGS=" $CF2 $CF3 "
./configure --prefix=$_INST_ --disable-shared
make -j$(nproc) || exit 1
make install

else
  echo "option: *CACHE*"
  export CFLAGS=" $CF2 $CF3 "
  export CXXFLAGS=" $CF2 $CF3 "

  ls -al $_INST_/include/
  cd $_SRC_
  rm -Rf c-toxcore/
fi

cd $_SRC_
git clone https://github.com/Zoxcore/c-toxcore
cd c-toxcore
git checkout "release"

sed -i -e 'sm#define DISABLE_H264_ENCODER_FEATURE.*m#define DISABLE_H264_ENCODER_FEATURE 1m' toxav/rtp.c
cat toxav/rtp.c |grep 'define DISABLE_H264_ENCODER_FEATURE'

./autogen.sh
make clean
export CFLAGS=" $CF2 -D_GNU_SOURCE -I$_INST_/include/ -O3 -g -fstack-protector-all "
export LDFLAGS=-L$_INST_/lib

./configure \
--prefix=$_INST_ \
--disable-soname-versions --disable-testing --disable-shared
make -j$(nproc) || exit 1
make install


echo "dependencies done"
