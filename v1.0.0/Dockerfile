# ──────────────────────────────────────────────────────────────────────────────
# FIFA World Cup 2026 Live Ticker ACAP — Dockerfile
#
# Builds using the official Axis ACAP Native SDK image.
# Target: Axis C1720 / C1710  (aarch64 / ARTPEC-8)
#
# Usage:
#   docker build --build-arg ARCH=aarch64 --tag fifa_wc:1.0.0 .
#   docker cp $(docker create fifa_wc:1.0.0):/opt/app ./build
#   # Output: ./build/fifa_wc_1_0_0_aarch64.eap
#
# Troubleshooting build errors:
#   • "acap-native-sdk image not found"  → confirm Docker can pull from axisecp/
#   • "cJSON download failed"            → check host network / proxy settings
#   • Linker errors on -laxparameter     → wrong SDK version; change VERSION below
#   • "architecture mismatch"            → only aarch64 is supported by C1720
# ──────────────────────────────────────────────────────────────────────────────

ARG ARCH=aarch64
ARG VERSION=12.9.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Download cJSON v1.7.18 (not bundled in Axis SDK sysroot)
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends curl ca-certificates && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h \
         -o /tmp/cJSON.h && \
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c \
         -o /tmp/cJSON.c

# Copy application source (main.c, Makefile, manifest.json, html/, mock/)
COPY ./app /opt/app/

# Place cJSON alongside source files
RUN cp /tmp/cJSON.h /opt/app/cJSON.h && \
    cp /tmp/cJSON.c /opt/app/cJSON.c

WORKDIR /opt/app

# Source SDK cross-compile environment and build ACAP package
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./
