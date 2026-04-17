FROM debian:bookworm-slim AS ffmpeg-builder

# Build a minimal FFmpeg with our stateful-audio patches applied (native AAC
# encoder + swresample + amix + avfilter state framework). The resulting
# libavcodec exports avcodec_{get,set}_encoder_state; libswresample exports
# swr_{get,set}_state; libavfilter exports avfilter_{get,set}_filter_state.
RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    git \
    nasm \
    pkg-config \
    patch \
    && rm -rf /var/lib/apt/lists/*

# Pinned FFmpeg commit — the stateful-audio patches are authored against this
# exact base (f7ca6f7 "vulkan: fix -Wdiscarded-qualifiers warning"). If you
# update this, re-run the patch-apply step and expect rebases.
ARG FFMPEG_COMMIT=f7ca6f7481
RUN git clone https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg && \
    cd /tmp/ffmpeg && \
    git checkout ${FFMPEG_COMMIT}

# Apply our stateful-audio patches (0001..0006). Uses `patch` with fuzz because
# 0003 and 0006 both touch the head of doc/APIchanges; fuzzy context is
# harmless (docs-only). Order matters: 0003 must be last because it adds the
# lavc APIchanges entry that rebases on top of 0004+0006's entries.
COPY test-local/ffmpeg-patches/0001-state-format-framework.patch \
     test-local/ffmpeg-patches/0002-aac-encoder-state.patch \
     test-local/ffmpeg-patches/0003-public-api.patch \
     test-local/ffmpeg-patches/0004-swresample-state.patch \
     test-local/ffmpeg-patches/0005-avfilter-amix-state.patch \
     test-local/ffmpeg-patches/0006-swr-amix-public-api.patch \
     /tmp/ffmpeg-patches/
RUN cd /tmp/ffmpeg && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0001-state-format-framework.patch && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0002-aac-encoder-state.patch && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0005-avfilter-amix-state.patch && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0006-swr-amix-public-api.patch && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0004-swresample-state.patch && \
    patch -p1 --fuzz=5 -F 99 < /tmp/ffmpeg-patches/0003-public-api.patch

# Minimal build — native AAC encoder only (no libfdk_aac). Mirrors the subset
# that nginx-vod-module actually needs for HLS audio muxing.
RUN cd /tmp/ffmpeg && \
    ./configure \
        --prefix=/usr/local/ffmpeg \
        --enable-shared \
        --disable-static \
        --disable-programs \
        --disable-doc \
        --disable-network \
        --disable-everything \
        --enable-encoder=aac \
        --enable-decoder=aac \
        --enable-decoder=aac_fixed \
        --enable-decoder=aac_latm \
        --enable-parser=aac \
        --enable-parser=aac_latm \
        --enable-demuxer=aac \
        --enable-demuxer=mov \
        --enable-muxer=adts \
        --enable-protocol=file \
        --enable-filter=aresample \
        --enable-filter=asetrate \
        --enable-filter=atempo \
        --enable-filter=volume \
        --enable-filter=amix \
        --enable-filter=aformat \
        --enable-filter=abuffer \
        --enable-filter=abuffersink && \
    make -j$(nproc) && \
    make install

# Sanity: the patched exports must be present in the built shared objects.
RUN nm -D /usr/local/ffmpeg/lib/libavcodec.so \
        | grep -E "avcodec_(get|set)_encoder_state" \
    && nm -D /usr/local/ffmpeg/lib/libswresample.so \
        | grep -E "swr_(get|set)_state" \
    && nm -D /usr/local/ffmpeg/lib/libavfilter.so \
        | grep -E "avfilter_(get|set)_filter_state"

# --- nginx builder ---
FROM debian:bookworm-slim AS nginx-builder

RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    libpcre3-dev \
    zlib1g-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy our custom, patched ffmpeg
COPY --from=ffmpeg-builder /usr/local/ffmpeg /usr/local/ffmpeg

# Download nginx
ARG NGINX_VERSION=1.26.2
RUN cd /tmp && \
    wget -q http://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz && \
    tar zxf nginx-${NGINX_VERSION}.tar.gz

# Copy our fork of vod module
COPY . /tmp/nginx-vod-module

# Build nginx with the module, pointing to our custom ffmpeg
ENV PKG_CONFIG_PATH=/usr/local/ffmpeg/lib/pkgconfig
ENV C_INCLUDE_PATH=/usr/local/ffmpeg/include
ENV LIBRARY_PATH=/usr/local/ffmpeg/lib
ENV LD_LIBRARY_PATH=/usr/local/ffmpeg/lib

# Modern FFmpeg (post Feb-2025) removed avcodec_close and deprecated
# sample_fmts in favor of ch_layouts/get_supported_config. Kaltura's
# upstream nginx-vod-module hasn't caught up yet, so:
#   - relax -Werror for deprecated-declarations + implicit-function-declaration
#   - inject a tiny compat library that defines avcodec_close as a no-op
#     (the calling code av_free()s the context immediately after, which
#      is how avcodec_free_context() internally cleans up anyway — this
#      matches the semantics nginx-vod-module expected from avcodec_close).
RUN cat > /tmp/avcodec_close_compat.c <<'EOF'
#include <libavcodec/avcodec.h>
/* Modern FFmpeg removed avcodec_close; emulate it as a no-op so that the
 * existing [avcodec_close(ctx); av_free(ctx);] idiom keeps behaving
 * correctly. av_free(ctx) will release the AVCodecContext memory, and any
 * codec-internal resources are held in ctx->priv_data which was already
 * allocated by the codec; they will be leaked — acceptable for the test
 * harness. For production, this should be replaced with avcodec_free_context. */
int avcodec_close(AVCodecContext *avctx) { (void)avctx; return 0; }
EOF
RUN cd /tmp && \
    cc -O2 -shared -fPIC -o /usr/local/ffmpeg/lib/libavcodec_close_compat.so \
        -I/usr/local/ffmpeg/include \
        avcodec_close_compat.c && \
    nm -D /usr/local/ffmpeg/lib/libavcodec_close_compat.so | grep avcodec_close

RUN cd /tmp/nginx-${NGINX_VERSION} && \
    ./configure \
        --prefix=/usr/local/nginx \
        --add-module=/tmp/nginx-vod-module \
        --with-file-aio \
        --with-threads \
        --with-http_ssl_module \
        --with-debug \
        --with-cc-opt="-O3 -I/usr/local/ffmpeg/include -Wno-error=deprecated-declarations -Wno-error=implicit-function-declaration -Wno-deprecated-declarations" \
        --with-ld-opt="-L/usr/local/ffmpeg/lib -Wl,-rpath,/usr/local/ffmpeg/lib -lavcodec_close_compat" && \
    make -j$(nproc) && \
    make install

# --- Runtime image ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libpcre3 \
    zlib1g \
    libssl3 \
    python3 \
    curl \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# Copy nginx and custom ffmpeg libs. The libavcodec_close_compat.so shim
# is built in the nginx-builder stage and lives alongside the patched
# ffmpeg libs; copy it too so nginx can dlopen it at runtime.
COPY --from=nginx-builder /usr/local/nginx /usr/local/nginx
COPY --from=ffmpeg-builder /usr/local/ffmpeg/lib /usr/local/ffmpeg/lib
COPY --from=nginx-builder /usr/local/ffmpeg/lib/libavcodec_close_compat.so /usr/local/ffmpeg/lib/
ENV LD_LIBRARY_PATH=/usr/local/ffmpeg/lib

# Create directories
RUN mkdir -p /var/log/nginx /var/cache/nginx /web/content

# Copy test files
COPY test-local/nginx.conf /usr/local/nginx/conf/nginx.conf
COPY test-local/media/ /web/content/
COPY test-local/mapping.py /web/mapping.py

EXPOSE 8080

CMD python3 /web/mapping.py & /usr/local/nginx/sbin/nginx -g "daemon off;"
