void test_pool_allocator() {
    mie::PoolAllocator<mie::AddOrderMessage, 10> pool;

    auto* p1 = pool.allocate();
    auto* p2 = pool.allocate();
    std::printf("p1=%p p2=%p diff=%td (expect %zu)\n",
                (void*)p1, (void*)p2,
                (char*)p2 - (char*)p1, sizeof(mie::AddOrderMessage));

    pool.deallocate(p1);
    auto* p3 = pool.allocate();
    std::printf("p1=%p p3=%p (expect equal - freed slot reused)\n",
                (void*)p1, (void*)p3);

    // drain the pool completely, confirm exhaustion is handled
    std::vector<mie::AddOrderMessage*> all;
    for (int i = 0; i < 20; ++i) {
        auto* p = pool.allocate();
        if (!p) { std::printf("pool exhausted after %d allocations\n", i); break; }
        all.push_back(p);
    }
}