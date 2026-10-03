#pragma once
#include <cstddef>
#include <cstdint>

namespace mie{
    template<typename T,size_t Capacity>
    class PoolAllocator{
    public:
        PoolAllocator(){
            for(size_t i=0;i<Capacity;i++){
                Freenode* node=reinterpret_cast<Freenode*>(storage_ +i*sizeof(T));
                if(i+1 < Capacity){
                    node->next=reinterpret_cast<Freenode*>(storage_ + (i+1)*sizeof(T));
                }else{
                    node->next=nullptr;
                }
            }
            free_list_head_.store(reinterpret_cast<Freenode*>(storage_),std::memory_order_relaxed);
        }
        T* allocate(){
            Freenode* old_head = free_list_head_.load(std::memory_order_acquire);
            while(old_head!=nullptr){
                Freenode* new_head=old_head->next;
                if(free_list_head_.compare_exchange_weak(
                    old_head,new_head,std::memory_order_acquire,std::memory_order_acquire
                )){
                    return reinterpret_cast<T*>(old_head);
                }
            }
            return nullptr;
        }
        void deallocate(T* ptr){
            Freenode* node=reinterpret_cast<Freenode*>(ptr);
            Freenode* old_head=free_list_head_.load(std::memory_order_relaxed);
            do{
                node->next=old_head;
            }while(!free_list_head_.compare_exchange_weak(
                old_head,node,std::memory_order_release,std::memory_order_relaxed
            ));
        }

    private:
        struct Freenode{
            Freenode* next;
        };

        alignas(T) std::byte storage_[sizeof(T)*Capacity];
        std::atomic<Freenode*> free_list_head_;
    
    };


}
