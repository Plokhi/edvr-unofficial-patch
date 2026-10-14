#pragma once
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace edvr {
// D3D11.1 windows use 16-byte constants, aligned to 16 constants (256 bytes).
// One DISCARD upload supplies every draw in a run without intervening transfers.
template<class T, unsigned Capacity> struct FlatConstantBatch {
    static_assert(std::is_trivially_copyable<T>::value, "constant payload must be bytes");
    static constexpr unsigned stride = (sizeof(T)+255u)&~255u;
    static constexpr unsigned bytes = stride*Capacity;
    static constexpr unsigned count = stride/16;
    static constexpr unsigned first(unsigned index) { return index*count; }
    static_assert(Capacity && bytes<=65536, "batch must fit an ordinary constant buffer");
    template<class Make> static bool pack(void* output,unsigned n,Make make) {
        if(!output || n>Capacity)return false;
        auto* data=static_cast<unsigned char*>(output);
        std::memset(data,0,size_t(n)*stride);
        for(unsigned i=0;i<n;++i) {
            const T value=make(i);
            std::memcpy(data+size_t(i)*stride,&value,sizeof(T));
        }
        return true;
    }
};
} // namespace edvr
