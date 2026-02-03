ARG BASE_IMAGE=debian:13.3-slim@sha256:77ba0164de17b88dd0bf6cdc8f65569e6e5fa6cd256562998b62553134a00ef0
ARG BGUTIL_YTDLP_POT_PROVIDER_VERSION=1.2.2
ARG DEBIAN_CODENAME=trixie
ARG DEBIAN_SNAPSHOT=20260127T000000Z
ARG DENO_VERSION=2.6.6
ARG FFMPEG_VERSION=7:7.1.3-0+deb13u1
ARG YT_DLP_VERSION=2026.01.29

########################################################################################################################
# Base stage

FROM ${BASE_IMAGE} AS base

ARG DEBIAN_CODENAME
ARG DEBIAN_SNAPSHOT

RUN printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s ${DEBIAN_CODENAME} main\n" "${DEBIAN_SNAPSHOT}" > /etc/apt/sources.list \
  && printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s ${DEBIAN_CODENAME}-security main\n" "${DEBIAN_SNAPSHOT}" >> /etc/apt/sources.list \
  && printf "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s ${DEBIAN_CODENAME}-updates main\n" "${DEBIAN_SNAPSHOT}" >> /etc/apt/sources.list

RUN apt-get update \
  && apt-get install --no-install-recommends -y ca-certificates \
  && rm -rf /var/lib/apt/lists/*

########################################################################################################################
# Build stage

FROM base AS build

ARG BGUTIL_YTDLP_POT_PROVIDER_VERSION
ARG TARGETARCH
ARG YT_DLP_VERSION

# Install curl, gcc, git, libcurl, unzip
RUN apt-get update \
  && apt-get install --no-install-recommends -y \
    build-essential \
    curl \
    git \
    libcurl4-openssl-dev \
    unzip \
  && rm -rf /var/lib/apt/lists/*

# Download yt-dlp
RUN case "${TARGETARCH}" in \
      amd64) ARCH="" ;; \
      arm64) ARCH="_aarch64" ;; \
      *) echo "Unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
  && curl -fsSL "https://github.com/yt-dlp/yt-dlp/releases/download/${YT_DLP_VERSION}/yt-dlp_linux${ARCH}.zip" -o /yt-dlp.zip \
  && unzip /yt-dlp.zip -d /yt-dlp \
  && mv "/yt-dlp/yt-dlp_linux${ARCH}" /yt-dlp/yt-dlp \
  && rm /yt-dlp.zip

# Download bgutil-ytdlp-pot-provider
RUN curl -fsSL https://github.com/Brainicism/bgutil-ytdlp-pot-provider/releases/download/${BGUTIL_YTDLP_POT_PROVIDER_VERSION}/bgutil-ytdlp-pot-provider.zip -o /bgutil-ytdlp-pot-provider.zip

# Build ytcd
COPY ytcd.c /ytcd.c
RUN gcc -O2 -Wall -Wextra -pedantic -std=c11 ytcd.c -o ytcd -lcurl

########################################################################################################################
# Mini deno stage (only required because COPY doesn't support arg substitution, but FROM does)

FROM denoland/deno:bin-${DENO_VERSION} AS deno

########################################################################################################################
# Runtime stage

FROM base AS runtime

ARG DENO_VERSION
ARG FFMPEG_VERSION

# Install ffmpeg/ffprobe, libcurl
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    "ffmpeg=${FFMPEG_VERSION}" \
    libcurl4 \
  && rm -rf /var/lib/apt/lists/*

# Install deno
COPY --from=deno /deno /usr/local/bin/deno

# Install yt-dlp
COPY --from=build /yt-dlp/ /opt/yt-dlp/
RUN ln -s /opt/yt-dlp/yt-dlp /usr/local/bin/yt-dlp

# Install bgutil-ytdlp-pot-provider
COPY --from=build /bgutil-ytdlp-pot-provider.zip /etc/yt-dlp/plugins/bgutil-ytdlp-pot-provider.zip

# Run ytcd
COPY --from=build /ytcd /usr/local/bin/ytcd
ENTRYPOINT ["ytcd"]
