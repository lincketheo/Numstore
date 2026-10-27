Tiger Style: performance
========================

Adapted from TigerBeetle's Tiger Style guide. The examples are Zig, but the
principles are general. Where this file disagrees with SKILL.md or the other
references, SKILL.md wins.

Read this at design time, before you choose data structures or APIs for code
that will be on a hot path or that moves a lot of data.

> "The lack of back-of-the-envelope performance sketches is the root of all
> evil." - Rivacindela Hudsoni

- Think about performance from the outset, from the beginning. The best time
  to solve performance, to get the huge 1000x wins, is in the design phase,
  which is precisely when we can't measure or profile. It's also typically
  harder to fix a system after implementation and profiling, and the gains are
  less. So you have to have mechanical sympathy. Like a carpenter, work with
  the grain.
- Perform back-of-the-envelope sketches with respect to the four resources
  (network, disk, memory, CPU) and their two main characteristics (bandwidth,
  latency). Sketches are cheap. Use sketches to be "roughly right" and land
  within 90% of the global maximum.
- Optimize for the slowest resources first (network, disk, memory, CPU) in
  that order, after compensating for the frequency of usage, because faster
  resources may be used many times more. For example, a memory cache miss may
  be as expensive as a disk fsync, if it happens many times more.
- Distinguish between the control plane and data plane. A clear delineation
  between control plane and data plane through the use of batching enables a
  high level of assertion safety without losing performance. See our
  [July 2021 talk on Zig SHOWTIME](https://youtu.be/BH2jvJ74npM?t=1958) for
  examples.
- Amortize network, disk, memory and CPU costs by batching accesses.
- Let the CPU be a sprinter doing the 100m. Be predictable. Don't force the
  CPU to zig zag and change lanes. Give the CPU large enough chunks of work.
  This comes back to batching.
- Be explicit. Minimize dependence on the compiler to do the right thing for
  you.

  In particular, extract hot loops into stand-alone functions with primitive
  arguments without `self` (see
  [an example](https://github.com/tigerbeetle/tigerbeetle/blob/0.16.19/src/lsm/compaction.zig#L1932-L1937)).
  That way, the compiler doesn't need to prove that it can cache struct's
  fields in registers, and a human reader can spot redundant computations
  easier.
