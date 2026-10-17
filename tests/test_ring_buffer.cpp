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

void test_ring_multithreaded(){
    mie::SpscRing<int,1024> ring;
    constexpr int NUM_ITEMS=1'000'000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&](){
        for(int i=0;i<NUM_ITEMS;i++){
            while(!ring.push(i)){} //ring is full so retry again busy waiting is expected rather than blocking
        }
        producer_done.store(true,std::memory_order_release);
    });

    std::thread consumer([&](){
        int expected=0;
        while(expected<NUM_ITEMS){
            auto v=ring.pop();
            if(!v){
                continue; //nothing ready yet spin and retry
            }
            if(*v != expected){
                std::printf("MISMATCH: expected %d, got %d\n", expected, *v);
                std::exit(1);
            }
            expected++;
        }
    });

    producer.join();
    consumer.join();
    std::printf("Multithreaded test passed: %d items, in order, no corruption\n", NUM_ITEMS);
}