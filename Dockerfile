FROM ubuntu:24.04 AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DLOB_ENABLE_LTO=ON \
    && cmake --build /build --parallel 2 \
    && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04 AS runtime
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/feed_handler /usr/local/bin/
COPY --from=build /build/matching_engine_main /usr/local/bin/
COPY --from=build /build/market_viewer /usr/local/bin/
COPY --from=build /build/cleanup_shared_memory /usr/local/bin/
COPY --from=build /build/partitioned_matching_main /usr/local/bin/
COPY --from=build /build/journal_replay_main /usr/local/bin/
COPY data /opt/low-latency-lob/data
WORKDIR /opt/low-latency-lob
CMD ["partitioned_matching_main", "--producers", "4", "--shards", "4", "--orders-per-producer", "25000", "--advanced-orders"]
