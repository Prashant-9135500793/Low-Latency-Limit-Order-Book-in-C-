# The Limit Order Book

> **Version 2 note:** The price-level data structure and price-time invariant remain the same. The current matching layer also supports market, IOC, FOK, post-only, execution reports, and risk checks; see [`order_semantics.md`](order_semantics.md).

## Vocabulary

- **Bid**: the price a buyer is willing to pay. The **best bid** is
  the *highest* resting buy price.
- **Ask** (offer): the price a seller is willing to accept. The
  **best ask** is the *lowest* resting sell price.
- **Spread**: `best_ask - best_bid`. In a non-crossed book this is
  always `> 0` — if it were `<= 0` a trade should already have
  happened at that price.
- **Price level**: all resting orders sharing one exact price on one
  side. Represented here by `PriceLevel { std::list<Order> orders; }`.
- **Depth**: the number of *distinct price levels* on a side (not the
  number of orders — a level with 40 orders and a level with 1 order
  each count as depth 1).

## Price-time priority

When more than one resting order could satisfy an incoming order, two
rules decide which one gets filled first:

1. **Price priority**: the better price always wins. For BUY orders,
   "better" means higher (a buyer willing to pay more gets filled
   first); for SELL orders, "better" means lower (a seller willing to
   accept less gets filled first). This is why the BUY side is kept
   sorted descending (`std::map<price, PriceLevel, std::greater<>>`)
   and the SELL side ascending (`std::greater<>`/`std::less<>` — see
   `order_book.hpp`): `.begin()` on each map is always the best price
   for that side.
2. **Time priority**: among orders at the *same* price, whoever
   arrived first is filled first. This is FIFO within a `PriceLevel`,
   enforced by always appending new orders to the back of
   `PriceLevel::orders` and always matching from the front.

Together: "best price first, and ties broken by arrival order" is
price-time priority — the standard matching discipline used by most
exchanges' continuous limit order books.

## Matching walkthrough

Given the resting book:

```
SELL 100 @ 101
SELL 200 @ 102
```

and an incoming `BUY 150 @ 102`:

1. Opposite side is SELL; best ask is 101. `102 >= 101` → crosses.
2. Match against the oldest order at 101 (qty 100). Trade: `100 @ 101`.
   Incoming order now has `150 - 100 = 50` remaining.
3. Best ask is now 102 (the 101 order is gone). `102 >= 102` → crosses.
4. Match against the order at 102 (qty 200), but only for the
   remaining 50. Trade: `50 @ 102`. Incoming order now has `0`
   remaining — done, nothing to insert.
5. The order at 102 had 200, used 50, so 150 remains resting.

Final book: `SELL 150 @ 102`. This exact scenario is asserted in
`tests/test_matching.cpp::test_scenario_from_brief` and reproduced
live by `benchmark_order_book`.

**Execution price** is always the *resting* order's price (the order
that was already in the book), not the incoming order's price — this
matches standard price-improvement semantics: an aggressive buyer
willing to pay 102 who crosses a 101 offer pays 101, not 102.

## Partial fills

An order can be partially filled across zero, one, or many resting
orders in a single call to `MatchingEngine::submitOrder`:
`OrderBook::reduceFront` reduces a resting order's quantity in place;
when it reaches zero the order is fully erased (from both the FIFO
list and the id index), otherwise it stays at the front of the level
with reduced quantity, ready to absorb the *next* incoming order that
crosses it.

## Cancellation and its complexity

`OrderBook::cancelOrder(id)` does **not** scan the book. It maintains
`std::unordered_map<order_id, OrderLocation>` where `OrderLocation` is
`{ side, price, std::list<Order>::iterator }`. Because `std::map`
iterators (to the price level) and `std::list` iterators (to the order
within its level) both remain valid across insertions/erasures of
*other* elements, this map lets cancellation jump straight to the
order:

- hash lookup: **O(1) average**
- `list::erase` at that iterator: **O(1)**
- if that was the last order at the level, erase the level from the
  price map: **O(log P)**, P = number of distinct price levels

So cancellation is **O(1) average, O(log P) worst case** — not O(N) in
the number of resting orders, which is what a naive "scan every level"
implementation would cost.

## Why `std::map` for price levels?

The project brief is explicit that correctness should come before
micro-optimization for the first implementation. A `std::map` (red-
black tree) gives O(log P) insert/erase/best-of for free, with no
custom memory management, and is easy to reason about and test. If you
wanted to go faster, the natural next step (not implemented here) is a
flat array or intrusive skip-list indexed directly by integer tick,
giving O(1) best-of at the cost of more complex code and, for a wide
price range, more memory — a classic space/complexity-vs-latency
trade-off that's worth understanding but was deliberately out of scope
for this project's "compact and understandable" goal.

## Invariants checked

`OrderBook::checkInvariants()` (used throughout the test suite)
verifies:

- the book is never crossed (`spread() <= 0` never holds once matching
  has run to completion for a batch of orders)
- every resting order has `quantity > 0`
- every order's stored price matches the map key of the level it's in
- BUY levels are strictly descending, SELL levels strictly ascending
- each level's cached `total_quantity` matches the sum of its orders
- the id index (`locations_`) has exactly one entry per resting order
