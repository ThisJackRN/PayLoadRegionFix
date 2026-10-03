# Same base image libmocha's own Dockerfile uses.
FROM ghcr.io/wiiu-env/devkitppc:20260225

# Build libmocha from source and install it where the Makefile expects it
# ($DEVKITPRO/wut/usr), mirroring libmocha's Dockerfile.
RUN git clone --depth 1 https://github.com/wiiu-env/libmocha /tmp/libmocha && \
    make -C /tmp/libmocha && \
    mkdir -p $DEVKITPRO/wut/usr && \
    cp -r /tmp/libmocha/lib /tmp/libmocha/include $DEVKITPRO/wut/usr/ && \
    rm -rf /tmp/libmocha

WORKDIR /project
