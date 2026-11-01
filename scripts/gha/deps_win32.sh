#!/bin/bash

. scripts/lib.sh

rm -rf SDL2_VC SDL2.zip "SDL2-$SDL_VERSION"
curl -fL --retry 5 --retry-delay 3 "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-VC.zip" -o SDL2.zip || exit 1
unzip -q SDL2.zip || exit 1
mv "SDL2-$SDL_VERSION" SDL2_VC || exit 1
test -f SDL2_VC/include/SDL.h || { echo "SDL2 headers missing after download"; exit 1; }

if [ "$GH_CPU_ARCH" = "i386" ]; then
	rustup target add i686-pc-windows-msvc

	# YY-Thunks obj for Windows XP support
	curl -L "https://github.com/Chuyu-Team/YY-Thunks/releases/download/$YY_THUNKS_VERSION/YY-Thunks-Objs.zip" -o yy-thunks.zip
	unzip -q -j -o yy-thunks.zip 'objs/x86/YY_Thunks_for_WinXP.obj' -d 3rdparty/yy-thunks
	rm yy-thunks.zip
fi

curl -L https://github.com/FWGS/potential-meme/releases/download/prebuilts/mingw-w64-x86_64-pkgconf-1.2.3.0-1-any.pkg.tar.zst -o pkgconf.tar.zst
7z x pkgconf.tar.zst
7z x pkgconf.tar
rm pkgconf.tar*
mv mingw64 pkgconf

FFMPEG_ARCHIVE=$(get_ffmpeg_archive)
curl -L "https://github.com/FWGS/FFmpeg-Builds/releases/download/latest/$FFMPEG_ARCHIVE.zip" -o ffmpeg.zip
if [ -f ffmpeg.zip ]; then
	unzip -x ffmpeg.zip
	mv "$FFMPEG_ARCHIVE" ffmpeg
fi
