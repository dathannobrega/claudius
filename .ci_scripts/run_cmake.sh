#!/usr/bin/env bash

EXTRA_CMAKE_ARGS=""
CMAKE_BUILD_TYPE="Release"

if [ "$BUILD_DEDICATED_SERVER" = "ON" ]; then
  EXTRA_CMAKE_ARGS="$EXTRA_CMAKE_ARGS -DBUILD_DEDICATED_SERVER=ON"
  if [ "$BUILD_TARGET" = "linux" ]; then
    CMAKE_BUILD_TYPE="RelWithDebInfo"
  fi
fi

case "$BUILD_TARGET" in
"vita")
	docker exec vitasdk /bin/bash -c "git config --global --add safe.directory /build/git && mkdir build && cd build && cmake -DAV1_VIDEO_SUPPORT=ON -DCMAKE_BUILD_TYPE=Release -DTARGET_PLATFORM=vita $EXTRA_CMAKE_ARGS .."
	;;
"switch")
	docker exec switchdev /bin/bash -c "git config --global --add safe.directory /build/git && /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -DAV1_VIDEO_SUPPORT=ON -DCMAKE_BUILD_TYPE=Release -DTARGET_PLATFORM=switch $EXTRA_CMAKE_ARGS -B build -S ."
	;;
"mac")
	mkdir build && cd build && cmake -DAV1_VIDEO_SUPPORT=ON -DENABLE_MULTIPLAYER=ON -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" $EXTRA_CMAKE_ARGS ..
	;;
"flatpak")
	;;
"appimage")
	mkdir build && cd build && cmake -DAV1_VIDEO_SUPPORT=ON -DENABLE_MULTIPLAYER=ON -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE -DCMAKE_INSTALL_PREFIX=/usr $EXTRA_CMAKE_ARGS ..
	;;
"linux")
	mkdir build && cd build && cmake -DAV1_VIDEO_SUPPORT=ON -DENABLE_MULTIPLAYER=ON -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE $EXTRA_CMAKE_ARGS ..
	;;
"emscripten")
	export EMSDK=${PWD}/emsdk
	mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE -DTARGET_PLATFORM=emscripten $EXTRA_CMAKE_ARGS ..
	;;
*)
	mkdir build && cd build && cmake -DAV1_VIDEO_SUPPORT=ON -DENABLE_MULTIPLAYER=ON $EXTRA_CMAKE_ARGS ..
	;;
esac
