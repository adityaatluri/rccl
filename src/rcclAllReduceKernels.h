/*
Copyright (c) 2017 - Present Advanced Micro Devices, Inc.
All rights reserved.
*/

#pragma once

#include "rcclKernelHelper.h"

__global__ void rcclAllReduceSetBuffers(DeviceControl_t *currTrack, const void *src, void *dst) {
    if(hipThreadIdx_x == 0) {
        std::atomic_store_explicit(&(currTrack->srcBuffer), (void*)src, std::memory_order_seq_cst);
        std::atomic_store_explicit(&(currTrack->dstBuffer), dst, std::memory_order_seq_cst);
    }
    __syncthreads();
    __threadfence_system();
}

/**
Copy data from src + index to dst
*/
template<typename VectorType>
__global__ void rcclAllReduceFirstCopy(DeviceControl_t *currTrack, VectorType *dst, size_t srcOffset) {
    int tx = hipThreadIdx_x;
    VectorType *src = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->srcBuffer), std::memory_order_seq_cst)) + srcOffset;
    copyChunk(dst, src, tx);
    __syncthreads();
}

template<typename DataType, typename VectorType>
__global__ void rcclAllReduceFirstCopyCnt(DeviceControl_t *currTrack, VectorType *dst, VectorType *src, size_t count) {
    int tx = hipThreadIdx_x;
    copyCnt<DataType, VectorType>(dst, src, tx, count);
    __syncthreads();
}


template<typename DataType, typename VectorType, rcclRedOp_t Op>
__global__ void rcclAllReduceOpCopy(DeviceControl_t *currTrack, VectorType *dst, VectorType *src1, size_t srcOffset) {
    int tx = hipThreadIdx_x;

    VectorType *src2 = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->srcBuffer), std::memory_order_seq_cst)) + srcOffset;

    if(Op == rcclSum) {
        reduceChunkSum(dst, src1, src2, tx);
    }
    if(Op == rcclProd) {
        reduceChunkProd(dst, src1, src2, tx);
    }
    if(Op == rcclMax) {
        reduceChunkMax<DataType, VectorType>(dst, src1, src2, tx);
    }
    if(Op == rcclMin) {
        reduceChunkMin<DataType, VectorType>(dst, src1, src2, tx);
    }
    __syncthreads();
}

template<typename DataType, typename VectorType, rcclRedOp_t Op>
__global__ void rcclAllReduceOpCopyCnt(DeviceControl_t *currTrack, VectorType *dst, VectorType *src1, size_t srcOffset, int count) {
    int tx = hipThreadIdx_x;

    VectorType *src2 = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->srcBuffer), std::memory_order_seq_cst)) + srcOffset;

    if(Op == rcclSum) {
        reduceChunkSumCnt<DataType, VectorType>(dst, src1, src2, tx, count, count % (sizeof(VectorType)/sizeof(DataType)));
    }
    if(Op == rcclProd) {
        reduceChunkProdCnt<DataType, VectorType>(dst, src1, src2, tx, count, count % (sizeof(VectorType)/sizeof(DataType)));
    }
    if(Op == rcclMax) {
        reduceChunkMaxCnt<DataType, VectorType>(dst, src1, src2, tx, count, count % (sizeof(VectorType)/sizeof(DataType)));
    }
    if(Op == rcclMin) {
        reduceChunkMinCnt<DataType, VectorType>(dst, src1, src2, tx, count, count % (sizeof(VectorType)/sizeof(DataType)));
    }

}


template<typename DataType, typename VectorType, rcclRedOp_t Op, bool WaitFornextPeerDst>
__global__ void rcclAllReduceOpCopynextPeerDst(DeviceControl_t *currTrack, size_t offset, VectorType *src1, size_t srcOffset, size_t chunkDwordx4) {
    int tx = hipThreadIdx_x;

    VectorType *src2 = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->srcBuffer), std::memory_order_seq_cst)) + srcOffset;

    if(WaitFornextPeerDst) {
        if(tx == 0) {
            while(std::atomic_load_explicit(&(currTrack->nextPeer->dstBuffer), std::memory_order_seq_cst) == 0);
        }
        __syncthreads();
    }

    VectorType *dst = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->nextPeer->dstBuffer), std::memory_order_seq_cst)) + offset;

    if(Op == rcclSum) {
        reduceChunkSum(dst, src1, src2, tx);
    }
    if(Op == rcclProd) {
        reduceChunkProd(dst, src1, src2, tx);
    }
    if(Op == rcclMax) {
        reduceChunkMax<DataType, VectorType>(dst, src1, src2, tx);
    }
    if(Op == rcclMin) {
        reduceChunkMin<DataType, VectorType>(dst, src1, src2, tx);
    }

    __syncthreads();
}

template<typename VectorType>
__global__ void rcclAllReduceCopynextPeerDst(DeviceControl_t *currTrack, size_t offset) {
    int tx = hipThreadIdx_x;

    VectorType *dst = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->nextPeer->dstBuffer), std::memory_order_seq_cst)) + offset;
    VectorType *src = reinterpret_cast<VectorType*>(std::atomic_load_explicit(&(currTrack->dstBuffer), std::memory_order_seq_cst)) + offset;

    copyChunk(dst, src, tx);
    __syncthreads();
}

template<typename DataType, typename VectorType, rcclRedOp_t Op>
__global__ void rcclAllReduceOpCopyTail(DeviceControl_t *track, int numGpus, size_t offset, size_t count) {
    int tx = hipThreadIdx_x;
    DataType *dst = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->dstBuffer), std::memory_order_seq_cst)) + offset;
    DataType *src = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->srcBuffer), std::memory_order_seq_cst)) + offset;
    if(tx == 0) {
        while(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst) == nullptr) {}
    }
    __syncthreads();

    DataType *srcRemote = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst)) + offset;
    track = track->nextPeer;

    if(Op == rcclSum) {
        reduceChunkSumCnt<DataType, VectorType>(dst, src, srcRemote, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
        for(int i=1;i<numGpus-1;i++) {
            if(tx == 0) {
                while(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst) == nullptr) {}
            }
            __syncthreads();
            srcRemote = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst)) + offset;
            reduceChunkSumCnt<DataType, VectorType>(dst, srcRemote, dst, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
            track = track->nextPeer;
        }
        __threadfence_system();
    }

    if(Op == rcclProd) {
        reduceChunkProdCnt<DataType, VectorType>(dst, src, srcRemote, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
        for(int i=1;i<numGpus-1;i++) {
            if(tx == 0) {
                while(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst) == nullptr) {}
            }
            __syncthreads();
            srcRemote = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst)) + offset;
            reduceChunkProdCnt<DataType, VectorType>(dst, srcRemote, dst, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
            track = track->nextPeer;
        }
        __threadfence_system();
    }

    if(Op == rcclMax) {
        reduceChunkMaxCnt<DataType, VectorType>(dst, src, srcRemote, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
        for(int i=1;i<numGpus-1;i++) {
            if(tx == 0) {
                while(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst) == nullptr) {}
            }
            __syncthreads();
            srcRemote = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst)) + offset;
            reduceChunkMaxCnt<DataType, VectorType>(dst, srcRemote, dst, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
            track = track->nextPeer;
        }
        __threadfence_system();
    }

    if(Op == rcclMin) {
        reduceChunkMinCnt<DataType, VectorType>(dst, src, srcRemote, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
        for(int i=1;i<numGpus-1;i++) {
            if(tx == 0) {
                while(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst) == nullptr) {}
            }
            __syncthreads();
            srcRemote = reinterpret_cast<DataType*>(std::atomic_load_explicit(&(track->nextPeer->srcBuffer), std::memory_order_seq_cst)) + offset;
            reduceChunkMinCnt<DataType, VectorType>(dst, srcRemote, dst, tx, count / (sizeof(VectorType)/sizeof(DataType)), count % (sizeof(VectorType)/sizeof(DataType)));
            track = track->nextPeer;
        }
    }
    __syncthreads();
}
