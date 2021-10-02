FROM alpine:3.13.6 as build

RUN apk add build-base gcc abuild binutils binutils-doc gcc-doc cmake cmake-doc extra-cmake-modules extra-cmake-modules-doc

WORKDIR /src

COPY . .

WORKDIR /src/Build

RUN cmake .. -DLINUX_LOCAL_DEV=true \
  && make \
  && cp -r ../Data/Sys/ Binaries/ \
  && touch Binaries/portable.txt