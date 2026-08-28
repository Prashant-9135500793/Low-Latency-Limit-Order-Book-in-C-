# Running on Real Market Data

> **Version 2 note:** Real-data mode still drives the same order book. The IPC protocol now also emits one execution report per command, so the viewer should run while the feed is active for large lossless bounded-queue runs.

By default `feed_handler` generates deterministic **synthetic** orders
(a fixed seed drives a reproducible random walk around an arbitrary
100.00 reference price — useful for controlled benchmarking, but not
grounded in any actual market). This project also supports a
**`--real-data` mode** that replays orders derived from genuine
historical AAPL price data instead.

## Quick start

```sh
cd build
./matching_engine_main &
sleep 0.5
./feed_handler --real-data --orders-per-day 40 --seed 7
./market_viewer
```

Measured output from this exact run: 506 real trading days x 40
orders/day = 20,240 orders pushed, 18,413 trades emitted by the engine
and 18,413 observed by the viewer (exact match), final prices around
115.84 / 121.09 (bid/ask, in ticks -- see below), which sits inside
AAPL's genuine historical range for the dataset period ($89.47 to
$136.27 -- see `data/README.md`).

## Where the data comes from

`data/AAPL.csv` is 506 rows of **real, publicly published historical
daily price data** for Apple Inc. (`Date, Open, High, Low, Close,
Volume`), covering 2015-02-17 through 2017-02-16, fetched directly
from the long-standing public `plotly/datasets` GitHub repository
(`finance-charts-apple.csv`). It is not generated, invented, or
hand-edited. Full provenance: `data/README.md`.

## Exactly what is real, and what is necessarily synthesized

Be precise about this, because it matters:

- **Real:** every date, every Open/High/Low/Close price, and every
  Volume figure in `data/AAPL.csv` is an actual recorded historical
  value.
- **Necessarily synthesized, and why:** individual-order-level
  historical data (the kind real exchanges publish only as paid feeds,
  e.g. LOBSTER or ITCH -- see `docs/interview_questions.md` Q6-Q10 for
  the general mmap/IPC concepts, unrelated to this) is not available
  for free. A daily bar tells you the day's actual high, low, and
  total volume, but not the sequence of individual orders that
  produced them. So `RealDataOrderGenerator` (in
  `apps/feed_handler.cpp`) generates several discrete limit orders
  *per real day*, with two hard constraints tying every generated
  order back to that day's real data:
  1. **Every order's price is drawn uniformly from that day's actual,
     real `[Low, High]` range** (converted to integer ticks — see
     `docs/order_book.md` for why integer ticks, not floating point).
     No order can ever be priced outside the real range that actually
     occurred that day.
  2. **Order size is scaled by that day's actual real Volume**: higher
     real-volume days produce a wider (larger-average) order-size
     range than lower-volume days, so the *relative* real volume
     signal carries through even though absolute per-order sizes are
     necessarily scaled down (real daily volume is tens of millions of
     shares -- far larger than a readable individual demo order).

What is **not** claimed: that these are the literal individual orders
that traded on those real days (that data isn't public), or that the
matching engine reproduces the literal historical sequence of trades.
What **is** true: the entire price path this generator can ever
produce, across all 506 days, is bounded end-to-end by genuine
recorded AAPL market history rather than an arbitrary synthetic random
walk.

## Command-line reference

```
./feed_handler --real-data [--data-file PATH] [--orders-per-day N] [--seed N] [--path P]
```

- `--real-data` — switch from the default synthetic generator to the
  real-data generator. Required to enable this mode.
- `--data-file PATH` — path to the CSV (default: `../data/AAPL.csv`,
  which resolves correctly when run from the standard `build/`
  directory). This mode has **no synthetic fallback** if the file is
  missing or fails to parse — it exits with an error rather than
  silently substituting fake data, since the entire point of the mode
  is to run on real data.
- `--orders-per-day N` — how many discrete orders to synthesize per
  real trading day (default 40). Total orders pushed = 506 x N.
- `--seed N` — seed for the random price/side/quantity draws *within*
  each day's real bounds; the real bounds themselves are unaffected by
  the seed.
- `--path P` — shared-memory backing file path, same as the default
  mode.

## Why daily bars, not tick data, in this offline environment

Genuine tick-by-tick or order-level historical market data is
generally only available through paid commercial feeds or restricted
academic datasets requiring registration/licensing (e.g. LOBSTER)
that could not be fetched programmatically in this project's sandboxed
build environment, which only has network access to a small allowlist
of package-registry and GitHub domains — no live exchange APIs. Daily
OHLCV bars from a public GitHub-hosted dataset were the highest-
granularity genuinely real, freely and programmatically fetchable
data available under those constraints. If you have access to your
own tick-level or LOBSTER-format data, it would be a natural extension
to add a second loader in `csv_reader.hpp` that feeds
`RealDataOrderGenerator`-style bounds directly from real per-order
records instead of per-day bars — the rest of the pipeline (shared
memory, SPSC queues, matching engine) is unaffected either way.
