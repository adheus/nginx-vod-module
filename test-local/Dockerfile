FROM debian:bookworm-slim AS ffmpeg-builder

# Enable non-free for libfdk-aac
RUN echo "deb http://deb.debian.org/debian bookworm main contrib non-free" > /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian bookworm-updates main contrib non-free" >> /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian-security bookworm-security main contrib non-free" >> /etc/apt/sources.list

# Build a minimal ffmpeg with AAC encoding support
RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    nasm \
    pkg-config \
    libfdk-aac-dev \
    && rm -rf /var/lib/apt/lists/*

ARG FFMPEG_VERSION=5.1.6
RUN cd /tmp && \
    wget -q https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz && \
    tar xf ffmpeg-${FFMPEG_VERSION}.tar.xz && \
    cd ffmpeg-${FFMPEG_VERSION} && \
    ./configure \
        --prefix=/usr/local/ffmpeg \
        --enable-shared \
        --disable-static \
        --disable-programs \
        --disable-doc \
        --disable-network \
        --disable-everything \
        --enable-libfdk-aac \
        --enable-encoder=libfdk_aac \
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
        --enable-filter=abuffersink \
        --enable-nonfree \
        --enable-gpl && \
    make -j$(nproc) && \
    make install

# --- nginx builder ---
FROM debian:bookworm-slim AS nginx-builder

RUN echo "deb http://deb.debian.org/debian bookworm main contrib non-free" > /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian bookworm-updates main contrib non-free" >> /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian-security bookworm-security main contrib non-free" >> /etc/apt/sources.list

RUN apt-get update && apt-get install -y \
    build-essential \
    wget \
    libpcre3-dev \
    zlib1g-dev \
    libssl-dev \
    libfdk-aac-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy our custom ffmpeg
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

RUN cd /tmp/nginx-${NGINX_VERSION} && \
    ./configure \
        --prefix=/usr/local/nginx \
        --add-module=/tmp/nginx-vod-module \
        --with-file-aio \
        --with-threads \
        --with-http_ssl_module \
        --with-debug \
        --with-cc-opt="-O3 -I/usr/local/ffmpeg/include" \
        --with-ld-opt="-L/usr/local/ffmpeg/lib -Wl,-rpath,/usr/local/ffmpeg/lib" && \
    make -j$(nproc) && \
    make install

# --- Runtime image ---
FROM debian:bookworm-slim

RUN echo "deb http://deb.debian.org/debian bookworm main contrib non-free" > /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian bookworm-updates main contrib non-free" >> /etc/apt/sources.list && \
    echo "deb http://deb.debian.org/debian-security bookworm-security main contrib non-free" >> /etc/apt/sources.list

RUN apt-get update && apt-get install -y \
    libpcre3 \
    zlib1g \
    libssl3 \
    libfdk-aac2 \
    python3 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy nginx and custom ffmpeg libs
COPY --from=nginx-builder /usr/local/nginx /usr/local/nginx
COPY --from=ffmpeg-builder /usr/local/ffmpeg/lib /usr/local/ffmpeg/lib
ENV LD_LIBRARY_PATH=/usr/local/ffmpeg/lib

# Create directories
RUN mkdir -p /var/log/nginx /var/cache/nginx /web/content

# Copy test files
COPY test-local/nginx.conf /usr/local/nginx/conf/nginx.conf
COPY test-local/media/ /web/content/
COPY test-local/mapping.py /web/mapping.py

EXPOSE 8080

CMD python3 /web/mapping.py & /usr/local/nginx/sbin/nginx -g "daemon off;"
