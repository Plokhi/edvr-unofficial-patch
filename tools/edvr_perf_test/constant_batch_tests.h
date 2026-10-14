#pragma once
#include "../../src/d3d11/flat_constant_batch.h"
#include <array>
#include <cstdint>

namespace constant_batch_tests {
// Match the full 160-byte foreground payload, not just its first vector.
struct Payload { uint32_t words[40]; };
template<class Check> void run(Check check) {
    using Batch=edvr::FlatConstantBatch<Payload,128>;
    check(Batch::stride==256 && Batch::bytes==32768 && Batch::count==16,"foreground window shape");
    std::array<unsigned char,Batch::bytes+16> data{};
    for(unsigned n : {0u,1u,2u,127u,128u}) {
        data.fill(0xcd);
        unsigned made=0;
        const auto make=[&](unsigned i){++made;Payload p{};
            for(unsigned k=0;k<40;++k)p.words[k]=0x80000000u+i*40+k;return p;};
        check(Batch::pack(data.data(),n,make) && made==n,"every active record packed once");
        for(unsigned i=0;i<n;++i) {
            Payload actual{};std::memcpy(&actual,data.data()+Batch::first(i)*16,sizeof(actual));
            bool equal=true,padding=true;
            for(unsigned k=0;k<40;++k)equal &= actual.words[k]==0x80000000u+i*40+k;
            for(unsigned k=sizeof(Payload);k<Batch::stride;++k)padding &= data[i*Batch::stride+k]==0;
            check(equal && padding && Batch::first(i)%16==0,"window reads exact record and zero padding, including last draw");
        }
        bool tail=true;for(size_t k=size_t(n)*Batch::stride;k<data.size();++k)tail &= data[k]==0xcd;
        check(tail,"unused windows and allocation guard untouched");
    }
    unsigned made=0;auto make=[&](unsigned){++made;return Payload{};};
    data.fill(0xcd);
    check(!Batch::pack(data.data(),129,make) && !Batch::pack(nullptr,1,make) && !made,
          "bad count or missing mapping refuses before writing");
    bool untouched=true;for(auto byte:data)untouched &= byte==0xcd;
    check(untouched,"refused upload leaves allocation intact");
}
} // namespace constant_batch_tests
