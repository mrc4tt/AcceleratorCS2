#!/bin/sh
# Compile and package inside a build image from docker/. Called by build-linux.sh / build-windows.sh,
# and by cs2-build-system's build-all.sh (the release builds on re).
#
# Usage: scripts/container-build.sh linux|windows
#
# Run from the repo root with HL2SDKCS2 and MMSOURCE_DEV set; windows also needs the xwin sysroot at
# /xwin. The package ends up in build/package/AcceleratorCS2.
set -eu

# Objects from another toolchain must not be reused.
rm -rf build bin

case "${1:-}" in
linux)
	premake5 gmake
	make -C build config=release_x64 -j"$(nproc)"
	premake5 package
	;;
windows)
	premake5 --os=windows --cc=clang gmake
	# premake writes GNU-linker/ELF flags for clang; translate them for the msvc target and lld-link.
	find build -name Makefile -exec sed -i \
		-e "s/ -fPIC//g" \
		-e "s/-Wl,--out-implib=/-Wl,\/implib:/g" \
		-e "s/ -L\/usr\/lib64//g" {} +
	# characterset: what Visual Studio defines for the breakpad projects (Unicode) and the plugin (MBCS).
	sed -i "s/^DEFINES += /DEFINES += -DUNICODE -D_UNICODE /" build/breakpad*/Makefile build/libdisasm/Makefile
	sed -i "s/^DEFINES += /DEFINES += -D_MBCS /" build/AcceleratorCS2/Makefile

	SYS="-isystem /xwin/crt/include -isystem /xwin/sdk/include/ucrt -isystem /xwin/sdk/include/um -isystem /xwin/sdk/include/shared -isystem /xwin/sdk/include/winrt"
	# /MD like the MSVC build, CodeView debug info for the .pdb.
	FLAGS="$SYS -D_CRT_USE_BUILTIN_OFFSETOF -fms-runtime-lib=dll -g -gcodeview -Wno-everything"
	make -C build config=release_x64 -j"$(nproc)" -Otarget \
		CC="clang --target=x86_64-pc-windows-msvc -fuse-ld=lld" \
		CXX="clang++ --target=x86_64-pc-windows-msvc -fuse-ld=lld" \
		AR="llvm-ar" \
		CFLAGS="$FLAGS" CXXFLAGS="$FLAGS" \
		LDFLAGS="-L/xwin/crt/lib/x86_64 -L/xwin/sdk/lib/um/x86_64 -L/xwin/sdk/lib/ucrt/x86_64 -lwininet -Wl,/debug -Wl,/opt:ref -Wl,/opt:icf -Wl,/pdb:../../bin/Release/AcceleratorCS2.pdb"
	premake5 --os=windows package
	;;
*)
	echo "usage: $0 linux|windows" >&2
	exit 1
	;;
esac
