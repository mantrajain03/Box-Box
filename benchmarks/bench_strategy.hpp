#ifndef BOXBOX_BENCH_STRATEGY_HPP
#define BOXBOX_BENCH_STRATEGY_HPP

/// Benchmarks for the optimizer, the strategy lab and the structures the other
/// modules own that the plan (sec. 4.7) also asks to be measured: the event
/// priority queue, the bounded top-K heap and the monotonic deque.
/// Returns true when every correctness check passed. Timings are never checked.
bool runStrategyBenchmarks();

#endif  // BOXBOX_BENCH_STRATEGY_HPP
