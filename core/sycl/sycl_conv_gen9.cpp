// Copyright 2009-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "sycl_conv_gen9.h"
#include "sycl_common.h"

namespace oidn {

  template<typename T, TensorLayout tensorLayout, TensorLayout weightLayout, PostOp postOp>
  struct SYCLConvGen9Kernel
  {
    static constexpr int blockOH = 2;               // block output height
    static constexpr int blockOW = 8;               // block output width
    static constexpr int blockIW = blockOW + 3 - 1; // block input width

    static constexpr int blockC = TensorAccessor3D<T, tensorLayout>::blockC; // block input/output channels

    TensorAccessor3D<T, tensorLayout> src;
    TensorAccessor4D<T, weightLayout> weight;
    TensorAccessor1D<T> bias;
    TensorAccessor3D<T, tensorLayout> dst;

    OIDN_INLINE void operator ()(const WorkGroupItem<3>& it) const SYCL_ESIMD_FUNCTION
    {
      const int oc = it.getLocalId<0>()  * blockC;
      const int oh = it.getGlobalId<1>() * blockOH;
      const int ow = it.getGlobalId<2>() * blockOW;

      // Output rows
      simd<T, blockOW * blockC> outRows[blockOH] = {}; // = 0

      // Iterate over input channel blocks
      for (int ic = 0; ic < src.C; ic += blockC)
      {
        const int ih = oh - 1;
        const int iw = ow - 1;

        // Load input rows into a ring buffer
        simd<T, blockIW * blockC> inRows[blockOH];

        #pragma unroll
        for (int boh = 0; boh < blockOH - 1; ++boh)
          loadRow(inRows[boh], ic, ih + boh, iw);

        // Iterate over kernel height
        #pragma unroll
        for (int kh = 0; kh < 3; ++kh)
        {
          // Load next input row into ring buffer
          loadRow(inRows[(kh + blockOH - 1) % blockOH], ic, ih + (kh + blockOH - 1), iw);

          // Get pointer to weights for kernel row
          const T* weightPtr = &weight(oc, ic, kh, 0);

          // Iterate over kernel width
          #pragma unroll
          for (int kw = 0; kw < 3; ++kw)
          {
            // Load weight matrix for kernel tap
            simd<T, blockC * blockC> weightMat;
            loadLargeBlock<T, blockC * blockC>(weightPtr, weightMat);
            weightPtr += blockC * blockC;

            // Multiply + accumulate rows
            #pragma unroll
            for (int i = 0; i < blockC; ++i)
            {
              #pragma unroll
              for (int boh = 0; boh < blockOH; ++boh)
              {
                #pragma unroll
                for (int bow = 0; bow < blockOW; ++bow)
                  outRows[boh].template select<blockC, 1>(bow * blockC) +=
                    inRows[(kh + boh) % blockOH].template replicate_w<blockC, 1>((kw + bow) * blockC + i) *
                    weightMat.template select<blockC, 1>(i * blockC);
              }
            }
          }
        }
      }

      // Load bias vector
      const auto biasVec = loadBlock<T, blockC>(&bias(oc));

      #pragma unroll
      for (int boh = 0; boh < blockOH; ++boh)
      {
        // Add bias
        outRows[boh] += biasVec.template replicate<blockOW>();

        // Apply ReLU
        outRows[boh] = max(outRows[boh], simd<T, blockOW * blockC>(0));
      }
      
      // Store output rows
      if constexpr (postOp == PostOp::None)
      {
        #pragma unroll
        for (int boh = 0; boh < blockOH; ++boh)
        {
          if (oh + boh >= dst.H)
            break;

          // Store output row
          storeRow(outRows[boh], oc, oh + boh, ow);
        }
      }
      else if constexpr (postOp == PostOp::Pool)
      {
        #pragma unroll
        for (int boh = 0; boh < blockOH; boh += 2)
        {
          if (oh + boh >= src.H) // src.H = output height without pooling
            break;

          // Pool output rows
          auto poolRow2x1 = max(outRows[boh], outRows[boh + 1]);
          auto poolRow2x2 = max(poolRow2x1.template replicate_vs_w<blockOW / 2, blockC * 2, blockC>(0),
                                poolRow2x1.template replicate_vs_w<blockOW / 2, blockC * 2, blockC>(blockC));
          
          // Store pooled row
          storeRow(poolRow2x2, oc, (oh + boh) / 2, ow / 2);
        }
      }
      else if constexpr (postOp == PostOp::Upsample)
      {
        #pragma unroll
        for (int boh = 0; boh < blockOH; ++boh)
        {
          if (oh + boh >= src.H) // src.H = output height without upsampling
            break;

          // Upsample output row
          simd<T, blockOW * blockC * 2> upRow1x2;

          #pragma unroll
          for (int bow = 0; bow < blockOW; ++bow)
            upRow1x2.template select<blockC * 2, 1>(bow * blockC * 2) = outRows[boh].template replicate_w<2, blockC>(bow * blockC);

          // Store upsampled rows
          storeRow<2>(upRow1x2, oc, (oh + boh) * 2,     ow * 2);
          storeRow<2>(upRow1x2, oc, (oh + boh) * 2 + 1, ow * 2);
        }
      }
    }

    // Loads a row from the src tensor
    template<int N>
    OIDN_INLINE void loadRow(simd<T, N>& row, int ic, int ih, int iw) const
    {
      static_assert(N % blockC == 0, "non-integer width");
      constexpr int W = N / blockC;

      if (ih < 0 || ih >= src.H)
      {
        row = 0;
        return;
      }

      const T* srcPtr = &src(ic, ih, iw);

      if (iw >= 0 && iw + W <= src.W)
      {
        // Fast path: load the entire row
        loadLargeBlock(srcPtr, row);
      }
      else
      {
        // Slow path: load the in-bounds columns of the row
        const simd<int, W> wVec(0, 1); // 0, 1, 2, ...
        simd_mask<W> predVec = (wVec >= -iw) & (wVec < src.W - iw);

        #pragma unroll
        for (int w = 0; w < W; ++w)
        {
          row.template select<blockC, 1>(w * blockC) = loadBlock<T, blockC>(srcPtr, predVec.template select<1, 1>(w));
          srcPtr += blockC;
        }
      }
    }

    // Stores a row to the dst tensor
    // Columns can be stored in chunks of K to improve performance
    template<int K = 1, int N>
    OIDN_INLINE void storeRow(simd<T, N>& row, int oc, int oh, int ow) const
    {
      static_assert(N % blockC == 0, "non-integer width");
      constexpr int W = N / blockC;
      static_assert(W % K == 0, "non-integer chunks");

      //if (oh >= dst.H)
      //  return;

      T* dstPtr = &dst(oc, oh, ow);

      if (ow + W <= dst.W)
      {
        // Fast path: store the entire row
        storeLargeBlock(dstPtr, row);
      }
      else
      {
        // Slow path: store the in-bounds columns of the row
        constexpr int numChunks = W / K;
        constexpr int chunkSize = blockC * K;
        const simd<int, numChunks> wVec(0, K); // 0, 1*K, 2*K, ...
        simd_mask<numChunks> predVec = wVec < dst.W - ow;

        #pragma unroll
        for (int i = 0; i < numChunks; ++i)
        {
          storeBlock(dstPtr, row.template select<chunkSize, 1>(i * chunkSize).read(), predVec.template select<1, 1>(i));
          dstPtr += chunkSize;
        }
      }
    }
  };

  SYCLConvGen9::SYCLConvGen9(const Ref<SYCLEngine>& engine, const ConvDesc& desc)
    : Conv(desc),
      engine(engine)
  {
    if (srcDesc.layout != TensorLayout::Chw16c || srcDesc.dataType != DataType::Float16)
      throw std::invalid_argument("unsupported convolution source layout/data type");
    if (weightDesc.layout != TensorLayout::OIhw16i16o || weightDesc.dataType != DataType::Float16)
      throw std::invalid_argument("unsupported convolution weight layout/data type");
    if (biasDesc.layout != TensorLayout::x || biasDesc.dataType != DataType::Float16)
      throw std::invalid_argument("unsupported convolution bias layout/data type");
  }

  void SYCLConvGen9::submit()
  {
    if (!src || !weight || !bias || !dst)
      throw std::logic_error("convolution argument not set");

    switch (postOp)
    {
    case PostOp::None:
      runImpl<PostOp::None>();
      break;

    case PostOp::Pool:
      runImpl<PostOp::Pool>();
      break;
    
    case PostOp::Upsample:
      runImpl<PostOp::Upsample>();
      break;
    }
  }

  template<PostOp kernelPostOp>
  void SYCLConvGen9::runImpl()
  {
    using Kernel = SYCLConvGen9Kernel<half, TensorLayout::Chw16c, TensorLayout::OIhw16i16o, kernelPostOp>;

    Kernel kernel;
    kernel.src    = *src;
    kernel.weight = *weight;
    kernel.bias   = *bias;
    kernel.dst    = *dst;

    WorkDim<3> globalSize = {dst->getCB(),
                             ceil_div(src->getH(), Kernel::blockOH),
                             ceil_div(src->getW(), Kernel::blockOW)};

    // Optimize for EU fusion
    if (globalSize[0] % 2 != 0 && globalSize[1] % 2 != 0 && globalSize[2] % 2 != 0)
      globalSize[2]++;

    WorkDim<3> localSize = {globalSize[0], 1, 1};
    int totalSize = globalSize[0];

    while (totalSize * 2 <= 16)
    {
      const int i = (localSize[1] * Kernel::blockOH < localSize[2] * Kernel::blockOW) ? 1 : 2;
      if (globalSize[i] % (localSize[i]*2) == 0)
      {
        localSize[i] *= 2;
        totalSize *= 2;
      }
      else if (globalSize[3-i] % (localSize[3-i]*2) == 0)
      {
        localSize[3-i] *= 2;
        totalSize *= 2;
      }
      else
        break;
    }

    engine->submitESIMDKernel(globalSize / localSize, localSize, kernel);
  }

} // namespace oidn