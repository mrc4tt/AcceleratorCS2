# Cross-compile environment for the Windows package: clang targeting x86_64-pc-windows-msvc, linked
# with lld-link against an MSVC CRT + Windows SDK sysroot made by xwin (mounted at /xwin at run time).
FROM debian:13

ARG PREMAKE_VERSION=5.0.0-beta8

RUN apt-get update \
	&& apt-get install -y --no-install-recommends ca-certificates curl make git gcc libc6-dev uuid-dev clang lld llvm \
	&& rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://github.com/premake/premake-core/archive/refs/tags/v${PREMAKE_VERSION}.tar.gz | tar xz -C /tmp \
	&& make -C /tmp/premake-core-${PREMAKE_VERSION} -f Bootstrap.mak linux -j"$(nproc)" \
	&& install /tmp/premake-core-${PREMAKE_VERSION}/bin/release/premake5 /usr/local/bin/premake5 \
	&& rm -rf /tmp/premake-core-${PREMAKE_VERSION}
