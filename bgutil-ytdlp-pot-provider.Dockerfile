ARG BASE_IMAGE=debian:13.3-slim@sha256:77ba0164de17b88dd0bf6cdc8f65569e6e5fa6cd256562998b62553134a00ef0
ARG BGUTIL_YTDLP_POT_PROVIDER_VERSION=1.2.2
ARG DEBIAN_CODENAME=trixie
ARG DEBIAN_SNAPSHOT=20260127T000000Z
ARG NODE_VERSION=25.5.0

########################################################################################################################
# Base stage

FROM ${BASE_IMAGE} AS base

ARG DEBIAN_CODENAME
ARG DEBIAN_SNAPSHOT

RUN printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s ${DEBIAN_CODENAME} main\n" "${DEBIAN_SNAPSHOT}" > /etc/apt/sources.list \
  && printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s ${DEBIAN_CODENAME}-security main\n" "${DEBIAN_SNAPSHOT}" >> /etc/apt/sources.list \
  && printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s ${DEBIAN_CODENAME}-updates main\n" "${DEBIAN_SNAPSHOT}" >> /etc/apt/sources.list

RUN apt-get update \
  && apt-get install --no-install-recommends -y \
    ca-certificates \
    libatomic1 \
  && rm -rf /var/lib/apt/lists/*

########################################################################################################################
# Build stage

FROM base AS build

ARG BGUTIL_YTDLP_POT_PROVIDER_VERSION
ARG NODE_VERSION
ARG TARGETARCH

# Install curl, git, xz
RUN apt-get update \
  && apt-get install --no-install-recommends -y \
    build-essential \
    curl \
    git \
    libcairo2-dev \
    libpango1.0-dev \
    xz-utils \
  && rm -rf /var/lib/apt/lists/*

# Install node/npm/npx
RUN case "${TARGETARCH}" in \
      amd64) ARCH="x64" ;; \
      arm64) ARCH="arm64" ;; \
      *) echo "Unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
  && curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-${ARCH}.tar.xz" -o /node.tar.xz \
  && mkdir -p /node \
  && tar -xJf /node.tar.xz -C /node --strip-components=1 \
  && rm /node.tar.xz

# Download and build bgutil-ytdlp-pot-provider
#
# Disastrously, the canvas npm package seems to bundle its own dynamic libraries. Maybe that's usually fine, but a few
# don' work on my Raspberry Pi 5 with its 16k page alignment. Rather than try to kick npm into some kind of slower
# "build from source and use system libs" mode, I think the simplest and best solution is to just delete all of these
# files and install working versions into the runtime container via the system package manager.
RUN git clone --branch "${BGUTIL_YTDLP_POT_PROVIDER_VERSION}" --depth 1 --single-branch https://github.com/Brainicism/bgutil-ytdlp-pot-provider.git \
  && cd /bgutil-ytdlp-pot-provider/server \
  && export PATH="/node/bin:${PATH}" \
  && npm ci \
  && npx tsc \
  && npm prune --omit=dev \
  && rm -f node_modules/canvas/build/Release/lib*.so*

########################################################################################################################
# Runtime stage

FROM base AS runtime

WORKDIR /bgutil-ytdlp-pot-provider

# Install canvas runtime dependencies. Even though we probably don't actually need JPG, GIF, or SVG support, which
# are said to be "optional", the node package still needs libgif and librsvg2 for whatever reason.
RUN apt-get update \
  && apt-get install --no-install-recommends -y \
    libcairo2 \
    libgif7 \
    libpango-1.0-0 \
    libpangocairo-1.0-0 \
    librsvg2-2 \
  && rm -rf /var/lib/apt/lists/*

# Install node/npm/npx
COPY --from=build /node/ /usr/local/

# Run bgutil-ytdlp-pot-provider
COPY --from=build /bgutil-ytdlp-pot-provider/server/build/ build/
COPY --from=build /bgutil-ytdlp-pot-provider/server/node_modules/ node_modules/
ENTRYPOINT ["node", "build/main.js"]
