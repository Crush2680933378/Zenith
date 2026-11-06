# syntax=docker/dockerfile:1
# ---------------------------------------------------------------------------
# Zenith 运行时镜像：多阶段构建，产物镜像只含运行所需（库 + 示例服务）
#   构建：docker build -t zenith .
#   运行：docker run --rm -p 8080:8080 zenith
#   验证：curl http://127.0.0.1:8080/hello
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DZENITH_TLS=openssl -DZENITH_ENABLE_ORM=OFF \
        -DZENITH_BUILD_TESTS=OFF -DZENITH_BUILD_PLUGINS=OFF \
    && cmake --build build -j"$(nproc)" \
    && cmake --install build --prefix /usr/local

# ---------------------------------------------------------------------------
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 zlib1g curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local /usr/local
RUN ldconfig

EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s \
    CMD curl -fsS http://127.0.0.1:8080/hello || exit 1

WORKDIR /app
# 示例服务：路由见 examples/02_http_server（/hello、/api/users/:id、鉴权分组）
CMD ["/usr/local/bin/http_server"]
