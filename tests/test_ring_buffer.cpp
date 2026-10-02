void test_ring_single_threaded() {
    mie::SpscRing<int, 4> ring;  // small capacity to easily trigger full/wraparound

    // fill it up
    for (int i = 0; i < 5; ++i) {
        bool ok = ring.push(i);
        std::printf("push(%d) = %s\n", i, ok ? "ok" : "FULL");
    }

    // drain it
    for (int i = 0; i < 5; ++i) {
        auto v = ring.pop();
        if (v) std::printf("pop() = %d\n", *v);
        else   std::printf("pop() = EMPTY\n");
    }

    // now push/pop past the wraparound point and confirm it still works
    for (int i = 100; i < 104; ++i) ring.push(i);
    for (int i = 0; i < 4; ++i) {
        auto v = ring.pop();
        std::printf("pop() = %d\n", v ? *v : -1);
    }
}