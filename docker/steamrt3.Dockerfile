# Build environment for the SteamRT3 (sniper, Debian 11 / glibc 2.31) package.
# Based on Valve's official Steam Runtime SDK image. Sniper only ships clang 11, which is too old
# for KHook / C++20, so a current clang comes from apt.llvm.org.
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

ARG LLVM_VERSION=22
ARG PREMAKE_VERSION=5.0.0-beta8

RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
	&& echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
	&& apt-get update \
	&& apt-get install -y --no-install-recommends clang-${LLVM_VERSION} lld-${LLVM_VERSION} \
	&& ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/bin/clang \
	&& ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/bin/clang++ \
	&& rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://github.com/premake/premake-core/archive/refs/tags/v${PREMAKE_VERSION}.tar.gz | tar xz -C /tmp \
	&& make -C /tmp/premake-core-${PREMAKE_VERSION} -f Bootstrap.mak linux -j"$(nproc)" \
	&& install /tmp/premake-core-${PREMAKE_VERSION}/bin/release/premake5 /usr/local/bin/premake5 \
	&& rm -rf /tmp/premake-core-${PREMAKE_VERSION}
