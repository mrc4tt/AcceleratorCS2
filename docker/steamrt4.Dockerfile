# Build environment for the SteamRT4 (Debian 13 / glibc 2.41) package.
# Based on Valve's official Steam Runtime SDK image, which already ships a C++20-capable clang.
FROM registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk:latest

ARG PREMAKE_VERSION=5.0.0-beta8

RUN curl -fsSL https://github.com/premake/premake-core/archive/refs/tags/v${PREMAKE_VERSION}.tar.gz | tar xz -C /tmp \
	&& make -C /tmp/premake-core-${PREMAKE_VERSION} -f Bootstrap.mak linux -j"$(nproc)" \
	&& install /tmp/premake-core-${PREMAKE_VERSION}/bin/release/premake5 /usr/local/bin/premake5 \
	&& rm -rf /tmp/premake-core-${PREMAKE_VERSION}
