/*
Copyright (c) 2017 - Present Advanced Micro Devices, Inc.
All rights reserved.
*/

#include "rcclTracker.h"
#include "rcclAllReduceRuntime.h"
#include "rcclBroadCastRuntime.h"

#include <vector>

const std::vector<size_t> sizeVec = {
sizeof(signed char),
sizeof(unsigned char),
sizeof(signed short),
sizeof(unsigned short),
sizeof(signed int),
sizeof(unsigned int),
sizeof(signed long),
sizeof(unsigned long),
sizeof(__fp16),
sizeof(float),
sizeof(double)
};

std::vector<DevTrackerPool_t*> Pools;

struct RcclUniqueId {
    DevTrackerPool_t *pool;
    RcclUniqueId() {
        pool = new DevTrackerPool_t;
    }
    ~RcclUniqueId() {
        delete pool;
    }
};

const char* rcclGetErrorString(rcclResult_t result) {
    switch(result) {
        case rcclSuccess : return "rcclSuccess";
        case rcclUnhandledHipError : return "rcclUnhandledHipError";
        case rcclSystemError: return "rcclSystemError";
        case rcclInternalError: return "rcclInternalError";
        case rcclInvalidDevicePointer: return "rcclInvalidDevicePointer";
        case rcclInvalidRank: return "rcclInvalidRank";
        case rcclUnsupportedDeviceCount: return "rcclUnsupportedDeviceCount";
        case rcclDeviceNotFound: return "rcclDeviceNotFound";
        case rcclInvalidDeviceIndex: return "rcclInvalidDeviceIndex";
        case rcclLibWrapperNotSet: return "rcclLibWrapperNotSet";
        case rcclHipMallocFailed: return "rcclHipMallocFailed";
        case rcclRankMismatch: return "rcclRankMismatch";
        case rcclInvalidArguments: return "rcclInvalidArguments";
        case rcclInvalidType: return "rcclInvalidType";
        case rcclInvalidOperation: return "rcclInvalidOperation";
        default: return "rcclErrorNotFound";
    }
}

rcclResult_t rcclGetUniqueId(rcclUniqueId *uniqueId) {
    if(uniqueId == nullptr) {
        return rcclInvalidArguments;
    }
    auto tmpId = new RcclUniqueId;
    *uniqueId = tmpId;
    return rcclSuccess;
}

rcclResult_t rcclCommInitRank(rcclComm_t *comm, int ndev, rcclUniqueId commId, int rank) {
    if(comm == nullptr) {
        return rcclInvalidArguments;
    }
    if(rank >= ndev) {
        return rcclInvalidRank;
    }
    if(commId == nullptr) {
        return rcclInvalidArguments;
    }

    auto pool = commId->pool;
    int dev;
    HIPCHECK(hipGetDevice(&dev));
    RcclComm_t *rcomm = pool->AddDevice(dev, rank, ndev);
    rcomm->pool = pool;
    *comm = rcomm;
    return rcclSuccess;
}

rcclResult_t rcclCommInitAll(rcclComm_t *comm, int ndev, int *devlist) {
    if(comm == nullptr || devlist == nullptr || ndev < 1) {
        return rcclInvalidArguments;
    }

    int userDevice;
    HIPCHECK(hipGetDevice(&userDevice));

    int deviceCount;
    HIPCHECK(hipGetDeviceCount(&deviceCount));
    if(ndev > deviceCount) {
        return rcclUnsupportedDeviceCount;
    }
    
    for(int i=0;i<ndev;i++) {
        HIPCHECK(hipSetDevice(devlist[i]));
        for(int j=0;j<ndev;j++) {
            if(devlist[i] != devlist[j]) {
                if(hipDeviceEnablePeerAccess(devlist[j], 0) != hipErrorPeerAccessAlreadyEnabled) {
                    HIPCHECK(hipSetDevice(userDevice));
                    return rcclDeviceNotFound;
                }
            }
        }
    }

    RcclComm_t *rcomm;
    DevTrackerPool_t *pool = new DevTrackerPool_t(devlist, ndev);

    DeviceControl_t *track;

    for(int i=0;i<ndev;i++) {
        rcomm = new RcclComm_t;
        track = pool->getPoolByDevID(devlist[i]);
        rcomm->pool = pool;
        rcomm->Track = track;
        rcomm->device = devlist[i];
        rcomm->rank = i;
        rcomm->numDevices = ndev;
        comm[i] = rcomm;
        HIPCHECK(hipSetDevice(devlist[i]));
        HIPCHECK(hipEventCreateWithFlags(&rcomm->event, hipEventReleaseToSystem));
    }

    HIPCHECK(hipSetDevice(userDevice));

    return rcclSuccess;
}

rcclResult_t rcclCommCuDevice(rcclComm_t comm, int *dev) {
    RcclComm_t *rcomm = comm;
    *dev = rcomm->device;
    return rcclSuccess;
}

rcclResult_t rcclCommUserRank(rcclComm_t comm, int *rank) {
    RcclComm_t *rcomm = comm;
    *rank = rcomm->rank;
    return rcclSuccess;
}

rcclResult_t rcclCommCount(rcclComm_t comm, int *count) {
    RcclComm_t *rcomm = comm;
    *count = rcomm->numDevices;
    return rcclSuccess;
}

rcclResult_t rcclCommDestroy(rcclComm_t comm) {
    RcclComm_t *rcomm = comm;
    rcomm->pool->activeDevices--;
    if(rcomm->pool->activeDevices == 0) {
        delete rcomm->pool;
    }
    delete rcomm;
    return rcclSuccess;
}

rcclResult_t rcclBcast(void *buff, int count, rcclDataType_t datatype, int root, rcclComm_t comm, hipStream_t stream) {
    #if RCCL_DEBUG == 1
    std::cerr<<"rcclBcast Count: "<<count<<" DataType: "<<datatype<<std::endl;
    #endif
    RcclComm_t *Comm = comm;
    DeviceControl_t *currTrack = Comm->Track;
    std::atomic_store_explicit(&(currTrack->srcBuffer), buff, std::memory_order_seq_cst);
    std::atomic_store_explicit(&(currTrack->prevPeer->dstBuffer), buff, std::memory_order_seq_cst);

    bool isRoot = Comm->Track->hipCurrentDeviceId == root;
    if(Comm->Track->hipCurrentDeviceId != Comm->pool->getPoolByDevID(root)->prevPeer->hipCurrentDeviceId) {
    hipLaunchKernelGGL(CheckPtrs, dim3(1,1,1), dim3(1,1,1), 0, stream, currTrack);

    switch(datatype) {
        case rcclChar:
        case rcclUchar:
        {
            if (isRoot) {
                rcclInternalBcastRoot<char, rccl_char16_t>(currTrack, count, stream);
            } else {
                rcclInternalBcast<char, rccl_char16_t>(currTrack, count, stream);
            }
            break;
        }
        case rcclShort:
        case rcclUshort:
        case rcclHalf:
        {
            if (isRoot) {
                rcclInternalBcastRoot<short, rccl_short8_t>(currTrack, count, stream);
            } else {
                rcclInternalBcast<short, rccl_short8_t>(currTrack, count, stream);
            }
            break;
        }
        case rcclInt:
        case rcclUint:
        case rcclFloat:
        {
            if (isRoot) {
                rcclInternalBcastRoot<int, rccl_int4_t>(currTrack, count, stream);
            } else {
                rcclInternalBcast<int, rccl_int4_t>(currTrack, count, stream);
            }

            break;
        }
        case rcclLong:
        case rcclUlong:
        case rcclDouble:
        {
            if (isRoot) {
                rcclInternalBcastRoot<long, rccl_long2_t>(currTrack, count, stream);
            } else {
                rcclInternalBcast<long, rccl_long2_t>(currTrack, count, stream);
            }

            break;
        }
        default: {
            break;
        }
    }
    }
    return rcclSuccess;
}

rcclResult_t rcclAllReduce(const void *sendbuff, void *recvbuff, size_t count, rcclDataType_t datatype, rcclRedOp_t op, rcclComm_t comm, hipStream_t stream) {
    RcclComm_t *Comm = comm;

    int numGpus = Comm->numDevices;
    int rank = Comm->rank;

    if(numGpus == 1) {
        hipMemcpyAsync(recvbuff, sendbuff, count * sizeVec[int(datatype)], hipMemcpyDeviceToDevice);
        return rcclSuccess;
   }

    DeviceControl_t *currTrack = Comm->Track;

    hipLaunchKernelGGL(rcclAllReduceSetBuffers, dim3(1,1,1), dim3(1,1,1), 0, stream, currTrack, sendbuff, recvbuff);

    if(op == rcclSum) {
    switch(datatype) {
        case rcclChar: {
            rcclInternalAllReduce<signed char, rccl_char16_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUchar: {
            rcclInternalAllReduce<unsigned char, rccl_uchar16_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclShort: {
            rcclInternalAllReduce<signed short, rccl_short8_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUshort: {
            rcclInternalAllReduce<unsigned short, rccl_ushort8_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclInt: {
            rcclInternalAllReduce<signed int, rccl_int4_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUint: {
            rcclInternalAllReduce<unsigned int, rccl_uint4_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclLong: {
            rcclInternalAllReduce<signed long, rccl_long2_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUlong: {
            rcclInternalAllReduce<unsigned long, rccl_ulong2_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclHalf: {
            rcclInternalAllReduce<__fp16, rccl_half8_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclFloat: {
            rcclInternalAllReduce<float, rccl_float4_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclDouble: {
            rcclInternalAllReduce<double, rccl_double2_t, rcclSum>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }

        default: {
            return rcclInvalidType;
        }
    }
    }

    if(op == rcclProd) {
    switch(datatype) {
        case rcclChar: {
            rcclInternalAllReduce<signed char, rccl_char16_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUchar: {
            rcclInternalAllReduce<unsigned char, rccl_uchar16_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclShort: {
            rcclInternalAllReduce<signed short, rccl_short8_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUshort: {
            rcclInternalAllReduce<unsigned short, rccl_ushort8_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclInt: {
            rcclInternalAllReduce<signed int, rccl_int4_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUint: {
            rcclInternalAllReduce<unsigned int, rccl_uint4_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclLong: {
            rcclInternalAllReduce<signed long, rccl_long2_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUlong: {
            rcclInternalAllReduce<unsigned long, rccl_ulong2_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclHalf: {
            rcclInternalAllReduce<__fp16, rccl_half8_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclFloat: {
            rcclInternalAllReduce<float, rccl_float4_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclDouble: {
            rcclInternalAllReduce<double, rccl_double2_t, rcclProd>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        default: {
            return rcclInvalidType;
        }
    }
    }

    if(op == rcclMax) {
    switch(datatype) {
        case rcclChar: {
            rcclInternalAllReduce<signed char, rccl_char16_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUchar: {
            rcclInternalAllReduce<unsigned char, rccl_uchar16_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclShort: {
            rcclInternalAllReduce<signed short, rccl_short8_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUshort: {
            rcclInternalAllReduce<unsigned short, rccl_ushort8_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclInt: {
            rcclInternalAllReduce<signed int, rccl_int4_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUint: {
            rcclInternalAllReduce<unsigned int, rccl_uint4_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclLong: {
            rcclInternalAllReduce<signed long, rccl_long2_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUlong: {
            rcclInternalAllReduce<unsigned long, rccl_ulong2_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclHalf: {
            rcclInternalAllReduce<__fp16, rccl_half8_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclFloat: {
            rcclInternalAllReduce<float, rccl_float4_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclDouble: {
            rcclInternalAllReduce<double, rccl_double2_t, rcclMax>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }

        default: {
            return rcclInvalidType;
        }
    }
    }

    if(op == rcclMin) {
    switch(datatype) {
        case rcclChar: {
            rcclInternalAllReduce<signed char, rccl_char16_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUchar: {
            rcclInternalAllReduce<unsigned char, rccl_uchar16_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclShort: {
            rcclInternalAllReduce<signed short, rccl_short8_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUshort: {
            rcclInternalAllReduce<unsigned short, rccl_ushort8_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclInt: {
            rcclInternalAllReduce<signed int, rccl_int4_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUint: {
            rcclInternalAllReduce<unsigned int, rccl_uint4_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclLong: {
            rcclInternalAllReduce<signed long, rccl_long2_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclUlong: {
            rcclInternalAllReduce<unsigned long, rccl_ulong2_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclHalf: {
            rcclInternalAllReduce<__fp16, rccl_half8_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclFloat: {
            rcclInternalAllReduce<float, rccl_float4_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }
        case rcclDouble: {
            rcclInternalAllReduce<double, rccl_double2_t, rcclMin>(currTrack, rank, numGpus, count, stream, Comm->event);
            return rcclSuccess;
        }

        default: {
            return rcclInvalidType;
        }
    }
    }


    return rcclSuccess;
}


