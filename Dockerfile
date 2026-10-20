# syntax=docker/dockerfile:1

# Build and test for the target Docker platform (linux/amd64 or linux/arm64).
FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make clean \
    && make all shared quickstart \
    && make check \
    && make PREFIX=/opt/cdin install

# SDK image: applications can compile and link against the installed library.
FROM debian:bookworm-slim AS sdk

ARG VERSION=dev
ENV DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.title="cdin-kernel SDK" \
      org.opencontainers.image.description="C11 development image with the cdin-kernel headers and libraries" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.licenses="GPL-3.0-or-later"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/cdin/ /usr/local/
COPY LICENSE /usr/share/licenses/cdin-kernel/LICENSE
RUN ldconfig

WORKDIR /workspace
CMD ["bash"]
