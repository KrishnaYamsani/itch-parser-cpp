#pragma once
#include <atomic>
#include <cstddef>
#include <optional>

namespace mie{
    template<typename T,size_t Capacity>
    class SpscRing{
        static_assert((Capacity & (Capacity-1))==0,"Capacity must be power of 2");
    
    private:
        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};
        T buffer_[Capacity];
    
    public:
        
        bool push(T item){
            size_t head=head_.load(std::memory_order_relaxed);
            size_t next_head=((head+1)&(Capacity-1));

            if(next_head==tail_.load(std::memory_order_acquire)){
                return false;
            }

            buffer_[head]=item;
            head_.store(next_head,std::memory_order_release);
            return true;
        }

        std::optional<T> pop(){
            size_t tail=tail_.load(std::memory_order_relaxed);
            
            if(tail==head_.load(std::memory_order_acquire)){
                return std::nullopt;
            }

            T item=buffer_[tail];
            tail_.store((tail+1)&(Capacity-1),std::memory_order_release);
            return item;
        }
        
    };
}