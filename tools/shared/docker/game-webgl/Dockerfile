# TRX building toolchain for the browser (WebGL 2 and WebAssembly).
#
# This is a multi-stage Docker image. It is designed to keep the final image
# size low. Each stage builds an external dependency with Emscripten. The final
# stage takes the artifacts (libraries, includes etc.) from previous stages and
# installs all the tools necessary to build TRX. SDL2, zlib and OpenGL ES come
# with Emscripten itself.

FROM emscripten/emsdk:6.0.13 AS base

# don't prompt during potential installation/update of tzinfo
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Warsaw

RUN apt-get update \
    && apt-get install -y \
        ccache \
        git \
        pkg-config \
        python3-pip \
        wget \
    && python3 -m pip install --break-system-packages \
        meson \
        ninja

# Every library runs alongside the game's threads, so it is built for them,
# and with WebAssembly SIMD, which every browser that runs the game supports.
ENV CFLAGS="-pthread -msimd128 -O2"
ENV CXXFLAGS="-pthread -msimd128 -O2"
ENV LDFLAGS="-pthread"
ENV PKG_CONFIG_LIBDIR=/ext/lib/pkgconfig/
ENV EM_PKG_CONFIG_PATH=/ext/lib/pkgconfig/



# pcre2
FROM base AS pcre2
RUN git clone https://github.com/PCRE2Project/pcre2 --branch pcre2-10.45
RUN emcmake cmake -S pcre2 -B pcre2/build \
        -DCMAKE_INSTALL_PREFIX=/ext/ \
        -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF \
        -DPCRE2_BUILD_TESTS=OFF \
        -DPCRE2_SUPPORT_JIT=OFF \
    && cmake --build pcre2/build \
    && cmake --install pcre2/build



# librubberband
FROM base AS rubberband
COPY ./tools/shared/docker/game-webgl/meson_emscripten.txt /tmp/
RUN git clone https://github.com/breakfastquay/rubberband --branch v4.0.0
# rubberband relies on its system headers to bring in size_t, which the
# libc++ Emscripten ships does not do.
RUN cd rubberband \
    && meson setup build \
        --cross-file /tmp/meson_emscripten.txt \
        --prefix=/ext/ \
        --libdir=lib \
        --default-library=static \
        -Dcpp_args="['-pthread', '-include', 'cstddef']" \
        -Dfft=builtin \
        -Dresampler=builtin \
        -Djni=disabled \
        -Dladspa=disabled \
        -Dlv2=disabled \
        -Dvamp=disabled \
        -Dcmdline=disabled \
        -Dtests=disabled \
    && ninja -C build \
    && ninja -C build install
# Emscripten links its C++ runtime only into C++ programs unless asked to,
# and TRX is a C program
RUN sed -i '/^Libs.private:/d' /ext/lib/pkgconfig/rubberband.pc \
    && echo 'Libs.private: -sDEFAULT_TO_CXX -lm' >> /ext/lib/pkgconfig/rubberband.pc



# libav
FROM base AS libav
COPY --from=rubberband /ext/ /ext/
RUN git clone \
    https://github.com/FFmpeg/FFmpeg
RUN cd FFmpeg && git checkout 066432ebcf
COPY ./tools/ffmpeg_flags.txt /tmp/ffmpeg_flags.txt
RUN cd FFmpeg \
    && emconfigure ./configure \
        --prefix=/ext/ \
        --enable-cross-compile \
        --target-os=none \
        --arch=x86_32 \
        --cc=emcc \
        --cxx=em++ \
        --ar=emar \
        --ranlib=emranlib \
        --nm=emnm \
        --enable-static \
        --disable-shared \
        --disable-autodetect \
        --disable-programs \
        --disable-runtime-cpudetect \
        --disable-stripping \
        --enable-pthreads \
        --pkg-config=pkg-config \
        --pkg-config-flags=--static \
        --extra-cflags="$CFLAGS -sUSE_ZLIB=1 -I/ext/include" \
        --extra-cxxflags="$CXXFLAGS" \
        --extra-ldflags="$LDFLAGS -L/ext/lib" \
        $(cat /tmp/ffmpeg_flags.txt) \
    && emmake make -j 4 \
    && emmake make install



## Lua
FROM base AS lua
RUN wget https://www.lua.org/ftp/lua-5.4.8.tar.gz \
    && tar xzf lua-5.4.8.tar.gz \
    && cd lua-5.4.8/src \
    && emcc $CFLAGS -DLUA_COMPAT_5_3 -c $(ls *.c | grep -vx 'luac\?\.c') \
    && emar rcs liblua.a *.o \
    && mkdir -p /ext/lib /ext/include \
    && cp liblua.a /ext/lib/ \
    && cp lua.h luaconf.h lualib.h lauxlib.h lua.hpp /ext/include/

# Provide pkg-config file for Lua
RUN mkdir -p /ext/lib/pkgconfig
COPY tools/shared/docker/lua.pc /ext/lib/pkgconfig/lua.pc



# uthash
FROM base AS uthash
RUN git clone https://github.com/troydhanson/uthash --branch v2.3.0 \
    && mkdir -p /ext/include \
    && cp uthash/src/*.h /ext/include/



# TRX
FROM base

# set the build dir - actual files are mounted with a Docker volume
RUN mkdir /app
WORKDIR /app

## manually built dependencies
COPY --from=pcre2 /ext/ /ext/
COPY --from=libav /ext/ /ext/
COPY --from=lua /ext/ /ext/
COPY --from=uthash /ext/ /ext/

# Emscripten builds its ports, such as SDL2, into its cache on first use. Doing
# that here keeps them in the image rather than in every build.
RUN embuilder build sdl2-mt zlib \
    && chmod -R a+rwX /emsdk/upstream/emscripten/cache

ENV PYTHONPATH=/app/tools/
ENV CCACHE_BASEDIR=/app/
ENV CCACHE_COMPILERCHECK=content
ENV CCACHE_DIR=/app/.cache/ccache/webgl
ENTRYPOINT ["/app/tools/shared/docker/game-webgl/entrypoint.sh"]
