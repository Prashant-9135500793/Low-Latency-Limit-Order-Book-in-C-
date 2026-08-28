# Data

## `AAPL.csv`

**Real, historical, publicly published daily price data for Apple Inc.
(AAPL)** — 506 real trading days, 2015-02-17 through 2017-02-16. Columns:
`Date, Open, High, Low, Close, Volume, Adjusted, ...` (the last few
columns are moving-average/Bollinger-band fields from the original
source, unused by this project).

- **Source:** `plotly/datasets` (`finance-charts-apple.csv`), a
  long-standing public dataset used in Plotly's own official charting
  examples: <https://github.com/plotly/datasets>. Fetched directly from
  `raw.githubusercontent.com` — not generated, not fabricated, not
  hand-edited.
- **What's real:** every `Date`, `Open`, `High`, `Low`, `Close`, and
  `Volume` value is an actual recorded historical trading value for
  AAPL on that date.
- **What's necessarily synthesized, and why:** freely available public
  data of this kind is published at **daily-bar** granularity (one
  Open/High/Low/Close/Volume record per trading day) — genuine
  individual-order-level historical data (the kind exchanges publish
  as paid feeds, e.g. LOBSTER/ITCH) is not freely available. So
  `feed_handler --real-data` (see `docs/real_data.md`) generates
  several individual limit orders *per real day*, with each order's
  price constrained to fall **within that day's actual real
  [Low, High] range** and order sizing derived from that day's actual
  real volume. The dates, the price bounds, and the volume are 100%
  real; only the individual order-level breakdown within a real day is
  synthesized, because that granularity of real data isn't publicly
  available for free. This is explained in full, with the exact
  formula used, in `docs/real_data.md`.

## License / redistribution note

`plotly/datasets` is a public, widely reused example-data repository;
this project redistributes one small CSV file (`data/AAPL.csv`, ~30KB)
from it for demonstration purposes only, exactly as fetched.
