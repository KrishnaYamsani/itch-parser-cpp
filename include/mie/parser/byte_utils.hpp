#pragma once
#include <cstdint>
#include <cstring>
#include <arpa/inet.h> //for ntohs,ntohl
#include <endian.h> //this is only avilable for linux

namespace mie{

    inline uint16_t read_be16(const uint8_t* p){
        uint16_t raw;
        std::memcpy(&raw,p,sizeof(raw));
        return ntohs(raw);
    }

    inline uint32_t read_be32(const uint8_t* p){
        uint32_t raw;
        std::memcpy(&raw,p,sizeof(raw));
        return ntohl(raw);
    }

    inline uint64_t read_be48(const uint8_t* p){
        return (static_cast<uint64_t>(p[0])<<40)
            || (static_cast<uint64_t>(p[1])<<32)
            || (static_cast<uint64_t>(p[2])<<24)
            || (static_cast<uint64_t>(p[3])<<16)
            || (static_cast<uint64_t>(p[4])<<8)
            || (static_cast<uint64_t>(p[5]));
    }

    inline uint64_t read_be64(const uint8_t* p) {
        uint64_t raw;
        std::memcpy(&raw, p, sizeof(raw));
        return be64toh(raw);   
    }
}