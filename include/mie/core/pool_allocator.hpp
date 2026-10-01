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
            free_list_head_=reinterpret_cast<Freenode*>(storage_);
        }
        T* allocate(){
            if(free_list_head_==nullptr){
                return nullptr;
            }
            Freenode* temp=free_list_head_;
            free_list_head_=free_list_head_->next;
            return reinterpret_cast<T*>(temp);
        }
        void deallocate(T* ptr){
            Freenode* node = reinterpret_cast<Freenode*>(ptr);
            node->next=free_list_head_;
            free_list_head_=node;
        }

    private:
        struct Freenode{
            Freenode* next;
        };

        alignas(T) std::byte storage_[sizeof(T)*Capacity];
        Freenode* free_list_head_;
    
    };


}
