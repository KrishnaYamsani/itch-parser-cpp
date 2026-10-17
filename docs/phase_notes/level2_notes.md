# Topic to research further: "Memory Pool Allocators" / "Fixed-Size Block Allocators"
(also searchable as: "intrusive free list allocator", "slab allocator" for the
more advanced industrial version of this same idea)

## Core idea
- Allocate one big contiguous block ONCE upfront (storage_), sized
  slot_size * capacity. This is the only heap allocation ever.
- Carve it into fixed-size "slots." All slots are the same size
  (sizeof(T) here), decided at compile time via template parameter.
- "Allocating" = handing out one pre-carved slot. O(1), no malloc call,
  no lock, no syscall.
- "Freeing" = marking a slot available again. Also O(1).

## The intrusive free list trick
- A FREE slot's bytes aren't holding a live object, so nobody cares what's
  in them. We reuse those bytes to store a pointer to the NEXT free slot.
- This chains all free slots into a singly linked list using zero extra
  memory — no separate array/structure needed to track "which slots are free."
- "Intrusive" = the list's bookkeeping (the `next` pointers) lives INSIDE
  the objects being tracked, not in a separate structure alongside them.

## Key mechanism: reinterpret_cast as a "lens"
- Same bytes in storage_ get interpreted two different ways depending on
  whether the slot is free or in-use:
    - FREE:   interpreted as `Freenode { Freenode* next; }`
    - IN USE: interpreted as the real type `T`
- reinterpret_cast<X*>(ptr) does NOT touch memory. It only tells the
  compiler "treat this address as pointing to type X from now on."
  No bytes move, no conversion happens — purely a compile-time relabel.
- The danger isn't in the cast itself, it's in what you do after:
  reading/writing fields assumes a struct layout that only you (the
  programmer) are responsible for making sure is actually true at that
  moment. The type system can't check this for reinterpret_cast.
- RULE: only ever read/write `.next` on a slot while it's genuinely free.
  Once handed out via allocate(), its bytes belong to T and .next is
  meaningless (likely overwritten by T's real data).

## Why this is NOT a normal linked list
- Normal linked list: nodes scattered wherever malloc happened to put
  them, each individually heap-allocated.
- Pool's free list: every node's address is storage_ + i*sizeof(T) for
  some i — all nodes live inside ONE contiguous block. Initially linked
  in memory order (cache-friendly), though can become scrambled after
  churn (freed slots get pushed to whatever the current head is).

## Address storage — the "aha" moment
- There is NO separate array of pointers anywhere.
- Each slot's `next` field IS the address of another slot, written
  directly into that slot's own bytes during construction.
- The address is always recomputable via storage_ + i*sizeof(T), but
  after setup we never need to recompute — we just follow existing
  .next pointers already sitting in memory (pure pointer-chasing,
  no arithmetic at allocate()/deallocate() time).

## alignas(T) on storage_
- Raw byte arrays have no guaranteed alignment. T likely needs to start
  at an address that's a multiple of its alignment requirement (e.g. 8
  bytes if it contains a uint64_t/uint64_t-sized field).
- Skipping this can silently "work" on x86 (tolerates misalignment, just
  slower) but breaks outright on stricter architectures. Always match
  storage alignment to the type you're storing.

# Topic to research further: "C++ Memory Model" / "Lock-Free Programming"
(also searchable as: "std::memory_order", "acquire-release semantics",
"happens-before relationship", "cache coherence MESI protocol",
"x86 TSO vs ARM weak memory model")

## Reordering — what it actually changes (and what it doesn't)

- Reordering changes WHEN two independent instructions execute relative to
  each other. It does NOT change WHERE they write — each instruction still
  targets its own correct, fixed memory address, always.
- Example: `slot[5] = x;` followed by `head = 6;` — reordering might let
  `head = 6` execute first in time, but `slot[5] = x` still always writes
  to slot[5]. There's no reordering bug that redirects a write to a
  different address — that would be a separate bug (e.g. off-by-one),
  unrelated to memory ordering.
- WHY reordering is legal at all: compilers/CPUs only promise to preserve
  program order as observed by a SINGLE thread. If a thread never reads
  back its own write in a way that would detect the reorder, swapping
  the order is invisible and "safe" from that thread's own point of view.
- It becomes DANGEROUS only when a second, independent thread is watching
  one of the two memory locations as a "signal" about the other
  (e.g. consumer watches `head` as a signal that `slot[5]` is ready).
  The compiler/CPU has no idea this relationship exists unless told via
  atomics + explicit memory order.

## Acquire/Release vs Mutex (lock) — fundamentally different mechanisms

| | Mutex (lock) | Atomic acquire/release |
|---|---|---|
| Controls | WHO may run (mutual exclusion) | WHAT memory becomes visible, and in what order |
| Can a thread block/wait? | Yes — possibly via OS scheduler | Never — no waiting at all |
| Worst case cost | Unbounded (scheduler/OS dependent) | Bounded — a few CPU cycles |
| Typical instruction | atomic read-modify-write (e.g. cmpxchg) + possible futex syscall | plain mov (x86) + compiler-level barrier |
| What "lock-free" means | N/A | No thread ever waits for another to finish an operation |

- KEY INSIGHT: acquire/release is NOT a lock. There is no mutual exclusion,
  no "only one thread allowed in here." It's purely about ordering
  guarantees on memory visibility between two specific operations.
- Our push()/pop() never block — if there's nothing to pop, pop() just
  returns std::nullopt immediately rather than waiting. That's what makes
  it genuinely lock-free, not just "uses fewer locks."

## The happens-before relationship (how synchronization really works)

> If thread B's ACQUIRE-load reads the exact value that thread A's
> RELEASE-store wrote, then everything thread A did BEFORE that release
> is guaranteed visible to thread B AFTER that acquire.

- This is CONDITIONAL on actually observing that specific value. If the
  consumer's acquire-load reads an older/stale value, no synchronization
  has happened yet — and that's a valid, expected outcome (means "nothing
  new yet," not a bug).
- The underlying hardware mechanism that propagates values between cores
  at all is cache coherence (MESI protocol) — this runs continuously
  regardless of atomics. What acquire/release adds on top is the ORDERING
  guarantee: that by the time one write becomes visible, an earlier
  write (from the same thread) is guaranteed to be visible too.

## Why acquire/release is nearly free on x86 specifically

- x86 uses a memory model called TSO (Total Store Order), which already
  guarantees, in hardware, for ordinary loads/stores:
    - loads never reorder relative to other loads
    - stores never reorder relative to other stores
    - a store never reorders to before an earlier load
  The ONE reordering x86 allows: a later load can sneak ahead of an
  earlier store (store buffer effect).
- acquire/release don't need to forbid that one permitted reordering —
  only seq_cst (sequential consistency, the strongest/most expensive
  ordering) forbids it, which is why seq_cst needs an extra hardware
  fence/locked instruction on x86, while acquire/release do not.
- Practical result: on x86, memory_order_acquire/release compile down to
  the SAME plain `mov` instruction as a normal load/store. The only
  change is a COMPILER-level instruction ("don't reorder code across
  this line") — no extra CPU-level fence instruction needed.
- On weaker architectures (ARM), this is NOT free: acquire compiles to a
  real barrier instruction (ldar — load-acquire), release to another
  (stlr — store-release). Still far cheaper than a mutex (no waiting,
  no OS involvement) but not literally "the same as a plain load."
- This is exactly why we write memory_order_acquire/release explicitly
  in source rather than relying on x86-specific behavior — it gives
  correctness guarantees that hold on ANY architecture, while letting
  each compiler/platform generate the cheapest instructions that still
  satisfy those guarantees on that specific hardware.

## Applying this to SpscRing::push() / pop()

- push(): read head_ with RELAXED (only the producer ever writes head_,
  so there's nothing to synchronize when reading our own last value back).
  Read tail_ with ACQUIRE (must see consumer's latest progress). Write
  data into buffer_ as an ordinary (non-atomic) write. Publish via
  head_.store(..., RELEASE) — this is what forces the data write to be
  visible before the head update takes effect from the consumer's view.
- pop() mirrors this: read head_ with ACQUIRE, read the data, publish
  consumption via tail_.store(..., RELEASE).
- alignas(64) on head_ and tail_: pads each to its own CPU cache line
  (64 bytes), preventing false sharing — without this, every producer
  write to head_ would force the consumer's core to reload the WHOLE
  cache line even though it only cares about tail_, silently destroying
  the performance benefit of having two separate threads at all.

## Wiring the pipeline: pool allocator + ring buffer + real parsed messages

### The problem with "parse in consumer"
- The pool allocator is templated on ONE fixed concrete type T — it can't
  hand out "generic raw bytes" of varying size/layout per message type.
- If parsing were deferred to the consumer, the producer would need to push
  raw undifferentiated bytes, and the consumer would have to duplicate the
  entire type-dispatch + field-extraction logic that already exists in the
  producer's parse loop — same work, done twice, in two places.
- Correct approach: PARSE ONCE, in the producer (as close to the raw bytes
  as possible), into one self-describing, fixed-size "event" type. Every
  thread downstream operates on that structured event, never on raw bytes
  again. This is standard practice in real low-latency systems.

### Tagged union pattern
- A `union` holds exactly ONE active member at a time, using the SAME
  underlying memory for all its members (so its size = size of its
  largest member, not the sum of all of them).
- The compiler does NOT track which member is "active" for you — reading
  the wrong member silently reinterprets the bytes as the wrong type,
  with zero complaint. Same danger class as reinterpret_cast.
- Fix: pair the union with an explicit "tag" field (an enum) that YOU
  set and check manually, telling you which member is actually valid:

    enum class MsgKind : uint8_t { Add, Delete, Executed, ... };
    struct ParsedMessage {
        MsgKind kind;     // <- the tag: which union member is valid
        union { AddOrderMessage add; DeleteOrderMessage del; ... };
    };

- This is called a "tagged union" / "discriminated union" specifically
  because of this tag-plus-union pairing.
- `std::variant` is the modern, type-safe C++ alternative — it tracks the
  active member automatically and prevents misreads at compile/runtime.
  Noted as a possible later upgrade (Effective Modern C++ style
  improvement) once the raw-union mental model is solid.

### enum class gotcha
- `enum class` (scoped enum) does NOT implicitly convert to/from its
  underlying integer type, unlike a plain `enum`. This is deliberate —
  it prevents accidental mixing of unrelated enums/ints.
  `p->kind = raw_byte;` will NOT compile — must explicitly map each
  raw byte case to its matching enum value inside each branch:
  `p->kind = MsgKind::Add;` etc.

### One pool instead of six
- Since ParsedMessage is ONE fixed-size struct (size = largest union
  member + the tag), you only need ONE PoolAllocator<ParsedMessage, N>
  for the whole pipeline — not a separate pool per message type.

### Ring buffer carries POINTERS, not full structs
- SpscRing<ParsedMessage*, Capacity> stores just 8-byte pointers, not
  full (up to ~48-byte) structs by value — avoids copying message data
  on every push/pop. The actual message lives in pool-allocated memory;
  the ring just carries a reference to it.
- ring.pop() on a pointer-typed ring returns std::optional<ParsedMessage*>.
  Must dereference TWICE to reach fields: *v unwraps the optional (gives
  the ParsedMessage*), then ->kind reaches into the struct. Forgetting
  the first dereference (writing v->kind directly) won't compile, since
  std::optional has no .kind member.

### Producer/consumer shutdown race (critical correctness bug class)
- A spinning consumer loop that checks "pop failed + producer_done flag
  is true => exit" has a race: the producer could push its LAST item and
  set producer_done=true in the gap between the consumer's failed pop
  and its check of the flag — silently dropping that final message.
- Fix: after observing producer_done==true, attempt ONE more pop before
  concluding "truly empty, safe to exit." This closes the gap.
- Verification discipline: always compare producer's total parsed count
  against consumer's total processed count after .join() — they must
  match EXACTLY, every run. A silent mismatch is exactly the kind of bug
  that passes most runs and fails rarely, making it dangerous if unchecked.

### Thread lifecycle reminders
- std::thread must be .join()'d (or .detach()'d) before it's destroyed —
  otherwise undefined behavior if the program exits with a live thread
  object. join() blocks the caller until that thread's lambda returns.
- Timing (steady_clock start/stop) must bracket the ACTUAL work, i.e.
  start before launching threads, stop AFTER both .join() calls return —
  otherwise you're not measuring the real pipeline duration.