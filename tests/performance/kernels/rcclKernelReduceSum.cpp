/*
Copyright (c) 2017-Present Advanced Micro Devices, Inc. 
All rights reserved.
*/

#include <hip/hip_runtime.h>
#include <hip/hip_runtime_api.h>
#include "rcclKernels.h"
#include "performance.h"
#include "common.h"

constexpr size_t iter = 128;

template<typename VectorType, typename DataType>
inline void launchReduceSum(size_t length, int dstDevice, int srcDevice) {

    constexpr unsigned numElements = sizeof(VectorType) / sizeof(DataType);

    VectorType *dSrc1, *dSrc2, *dDst;
    std::vector<DataType> hSrc1(length);
    std::vector<DataType> hSrc2(length);
    std::vector<DataType> hDst(length);

    size_t size = sizeof(DataType) * length;

    HIPCHECK(hipSetDevice(dstDevice));
    HIPCHECK(hipDeviceEnablePeerAccess(srcDevice, 0));
    HIPCHECK(hipMalloc(&dDst, size));
    HIPCHECK(hipMemcpy(dDst, hDst.data(), size, hipMemcpyHostToDevice));

    HIPCHECK(hipSetDevice(srcDevice));
    HIPCHECK(hipDeviceEnablePeerAccess(dstDevice, 0));
    HIPCHECK(hipMalloc(&dSrc1, size));
    HIPCHECK(hipMalloc(&dSrc2, size));
    HIPCHECK(hipMemcpy(dSrc1, hSrc1.data(), size, hipMemcpyHostToDevice));
    HIPCHECK(hipMemcpy(dSrc2, hSrc2.data(), size, hipMemcpyHostToDevice));

    HIPCHECK(hipSetDevice(srcDevice));

    hipLaunchKernelGGL((rcclKernelReduceSum), dim3(1,1,1), dim3(WI,1,1), 0, 0, dDst, dSrc1, dSrc2, length/numElements, length%numElements);

    perf_marker mark;

    for(size_t i=0;i<iter;i++) {
        hipLaunchKernelGGL((rcclKernelReduceSum), dim3(1,1,1), dim3(WI,1,1), 0, 0, dDst, dSrc1, dSrc2, length/numElements, length%numElements);
    }

    HIPCHECK(hipDeviceSynchronize());

    mark.done();
    mark.bw(size*iter);

    HIPCHECK(hipFree(dSrc1));
    HIPCHECK(hipFree(dSrc2));
    HIPCHECK(hipFree(dDst));
}

int main(int argc, char* argv[]){
    if(argc != 5) {
        std::cerr<<"Usage: ./a.out <number of elements> <rcclDataType_t> <dst gpu> <src gpu>"<<std::endl;
        std::cerr<<"\n\
Example: ./a.out 123456 0 1 2       \n\
Does "<<__FILE__<<" of size 123456 bytes \n\
of rcclChar/rcclInt8 from GPU 2 to GPU 1"<<std::endl;
        return 0;
    }

    size_t count = atoi(argv[1]);
    int dataType = atoi(argv[2]);
    if(dataType > 10 || dataType < 0) {
        std::cerr<<"Bad Datatype requested. Use from 0 to 10"<<std::endl;
        return 0;
    }

    int dstDevice = atoi(argv[3]);
    int srcDevice = atoi(argv[4]);

    switch(dataType) {
        case 0:
            launchReduceSum<rccl_char16_t, signed char>(count, dstDevice, srcDevice);
            return 0;
        case 1:
            launchReduceSum<rccl_uchar16_t, unsigned char>(count, dstDevice, srcDevice);
            return 0;
        case 2:
            launchReduceSum<rccl_short8_t, signed short>(count, dstDevice, srcDevice);
            return 0;
        case 3:
            launchReduceSum<rccl_ushort8_t, unsigned short>(count, dstDevice, srcDevice);
            return 0;
        case 4:
            launchReduceSum<rccl_int4_t, signed int>(count, dstDevice, srcDevice);
            return 0;
        case 5:
            launchReduceSum<rccl_uint4_t, unsigned int>(count, dstDevice, srcDevice);
            return 0;
        case 6:
            launchReduceSum<rccl_long2_t, unsigned long>(count, dstDevice, srcDevice);
            return 0;
        case 7:
            launchReduceSum<rccl_ulong2_t, signed long>(count, dstDevice, srcDevice);
            return 0;
        case 8:
            launchReduceSum<rccl_half8_t, rccl_half_t>(count, dstDevice, srcDevice);
            return 0;
        case 9:
            launchReduceSum<rccl_float4_t, float>(count, dstDevice, srcDevice);
            return 0;
        case 10:
            launchReduceSum<rccl_double2_t, double>(count, dstDevice, srcDevice);
            return 0;

        default:
            return 0;
    }
}
