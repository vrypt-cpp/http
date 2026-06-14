FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc cmake ninja-build libc6-dev make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release . \
    && cmake --build /build

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/ultrahttp /app/ultrahttp

EXPOSE ${PORT:-8080}

CMD ["./ultrahttp"]
