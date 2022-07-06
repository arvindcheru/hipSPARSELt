/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include "definitions.h"
#include "handle.h"
#include "hipsparselt_ostream.hpp"
#include "rocsparselt.h"
#include "rocsparselt_spmm_utils.hpp"
#include "utility.hpp"

#include <hip/hip_runtime_api.h>

template <typename Ti, int SG0I, int SG1J, int TT0I, int TT1J>
__global__ void compress_kernel(const Ti*      in,
                                Ti*            out,
                                unsigned char* metadata,
                                int64_t        m,
                                int64_t        n,
                                int64_t        stride1,
                                int64_t        stride2,
                                int64_t        batch_stride,
                                int64_t        c_stride1,
                                int64_t        c_stride2,
                                int64_t        c_batch_stride,
                                int64_t        m_stride1,
                                int64_t        m_stride2,
                                int64_t        m_batch_stride,
                                int            num_batches,
                                int64_t        sizes,
                                int64_t        c_sizes,
                                int64_t        m_sizes)
{
    constexpr int metadata_tiles_y = 8;
    constexpr int tiles_y          = 4;

    constexpr unsigned int MT0I = SG0I * TT0I;
    constexpr unsigned int MT1J = SG1J * TT1J;

    unsigned int serial = hc_get_workitem_id(0);
    unsigned int sg0I   = serial % SG0I;
    unsigned int sg1J   = serial / SG0I;

    unsigned int wg0I    = hc_get_group_id(0); // M / MT0I
    unsigned int wg1J    = hc_get_group_id(1); // N / MT0J
    unsigned int batchId = hc_get_group_id(2);

    if((MT1J * wg1J + sg1J * TT1J) >= n || (MT0I * wg0I + sg0I * TT0I) >= m)
        return;

    //caculate the tagret address (offset) of the dense matrix.
    int64_t stride           = sg0I * stride1 + sg1J * TT1J * stride2;
    int64_t wg_stride        = MT1J * wg1J * stride2 + MT0I * wg0I * stride1;
    int64_t b_stride         = batchId * batch_stride;
    int64_t globalReadOffset = b_stride + wg_stride + stride;

    //caculate the tagret address (offset) of the compresed matrix.
    int64_t c_stride = (sg0I * TT0I * c_stride1)
                       + (sg1J * TT1J * c_stride2 >> 1); // compressed matrix's k is orginla k/2
    int64_t c_wg_stride       = (MT0I * wg0I * c_stride1) + (MT1J * wg1J * c_stride2 >> 1);
    int64_t c_b_stride        = batchId * c_batch_stride;
    int64_t globalWriteOffset = c_b_stride + c_wg_stride + c_stride;

    //caculate the tagret address (offset) of the metadata
    int64_t m_stride
        = (sg0I * m_stride1) + (sg1J * TT1J * m_stride2 >> 3); // metadata's k is orginal k/8
    int64_t m_wg_stride               = (MT0I * wg0I * m_stride1) + (MT1J * wg1J * m_stride2 >> 3);
    int64_t m_b_stride                = batchId * m_batch_stride;
    int64_t globalWriteMetadataOffset = m_b_stride + m_wg_stride + m_stride;

    for(int i = 0; i < TT0I; i++)
    {
        for(int j = 0; j < TT1J; j += metadata_tiles_y)
        {
            auto offset = globalReadOffset + i * stride1 + j * stride2;

            Ti            values[] = {static_cast<Ti>(0.0f),
                           static_cast<Ti>(0.0f),
                           static_cast<Ti>(0.0f),
                           static_cast<Ti>(0.0f)};
            unsigned char md       = 0xEE;

            for(int t = 0; t < metadata_tiles_y / tiles_y; t++)
            {
                int m_idx = 0;
                for(int k = 0; k < tiles_y; k++)
                {
                    int64_t pos = offset + (k + t * tiles_y) * stride2;
                    //TODO pos is always lower than sizes by pre-conditions, maybe can remove this check.
                    if(pos > sizes)
                        break;

                    Ti value = in[pos];
                    if(value != static_cast<Ti>(0.0f))
                    {
                        if(m_idx == 0 && k == 3)
                            m_idx++;
                        auto midx    = m_idx + t * (tiles_y >> 1);
                        values[midx] = value;
                        auto shift   = midx << 1;
                        md           = (md & (~(0x03 << shift))) | ((k & 0x03) << shift);
                        m_idx++;
                        if(m_idx > 1)
                            break;
                    }
                }
            }

            auto c_offset = globalWriteOffset + i * c_stride1 + (j >> 1) * c_stride2;
#pragma unroll
            for(int k = 0; k < tiles_y; k++)
            {
                auto c_pos = c_offset + k * c_stride2;
                out[c_pos] = values[k];
            }
            auto m_offset      = globalWriteMetadataOffset + i * m_stride1 + (j >> 3) * m_stride2;
            metadata[m_offset] = md;
        }
    }
}

template <typename Ti>
rocsparselt_status rocsparselt_smfmac_compress_template(const rocsparselt_handle handle,
                                                        int64_t                  m,
                                                        int64_t                  n,
                                                        int64_t                  stride0,
                                                        int64_t                  stride1,
                                                        int                      batch_stride,
                                                        int64_t                  c_stride0,
                                                        int64_t                  c_stride1,
                                                        int                      c_batch_stride,
                                                        int64_t                  m_stride0,
                                                        int64_t                  m_stride1,
                                                        int64_t                  m_batch_stride,
                                                        int                      num_batches,
                                                        rocsparselt_operation    op,
                                                        rocsparselt_order        order,
                                                        const Ti*                d_in,
                                                        Ti*                      d_out,
                                                        unsigned char*           d_metadata,
                                                        hipStream_t              stream)
{
    constexpr int SG0I = 16;
    constexpr int SG1J = 2;
    constexpr int TT0I = 1;
    constexpr int TT1J = 8; //must be the multiplication of 8.
    constexpr int MT0I = SG0I * TT0I;
    constexpr int MT1J = SG1J * TT1J;

    int block_x = m / MT0I + (m % MT0I > 0 ? 1 : 0);
    int block_y = n / MT1J + (n % MT1J > 0 ? 1 : 0);
    hipLaunchKernelGGL((compress_kernel<Ti, SG0I, SG1J, TT0I, TT1J>), /* compute kernel*/
                       dim3(block_x, block_y, num_batches),
                       dim3(SG0I * SG1J),
                       0 /*dynamic shared*/,
                       stream,
                       d_in,
                       d_out,
                       d_metadata,
                       m,
                       n,
                       stride0,
                       stride1,
                       batch_stride,
                       c_stride0,
                       c_stride1,
                       c_batch_stride,
                       m_stride0,
                       m_stride1,
                       m_batch_stride,
                       num_batches,
                       num_batches * batch_stride,
                       num_batches * c_batch_stride,
                       num_batches * m_batch_stride);
    return rocsparselt_status_success;
}

rocsparselt_status rocsparselt_smfmac_compress_impl(const rocsparselt_handle handle,
                                                    rocsparselt_mat_descr    matrix,
                                                    rocsparselt_operation    op,
                                                    const void*              d_in,
                                                    void*                    d_out,
                                                    hipStream_t              stream)
{

    int64_t ld = matrix->ld;

    int64_t m, n;
    int64_t stride0, stride1, c_stride0, c_stride1, m_stride0, m_stride1;
    int64_t c_batch_stride, m_batch_stride;

    if(op == rocsparselt_operation_transpose)
    {
        m              = matrix->n;
        n              = matrix->m;
        stride0        = ld;
        stride1        = 1;
        c_stride0      = matrix->c_ld;
        c_stride1      = 1;
        m_stride0      = matrix->c_k / 4;
        m_stride1      = 1;
        c_batch_stride = c_stride0 * matrix->n;
        m_batch_stride = m_stride0 * m;
    }
    else
    {
        m              = matrix->m;
        n              = matrix->n;
        stride0        = 1;
        stride1        = ld;
        c_stride0      = 1;
        c_stride1      = matrix->c_ld;
        m_stride0      = matrix->c_k / 4;
        m_stride1      = 1;
        c_batch_stride = c_stride1 * matrix->c_k;
        m_batch_stride = m_stride0 * m;
    }

    rocsparselt_order    order = matrix->order;
    rocsparselt_datatype type  = matrix->type;

    int     num_batches  = 1;
    int64_t batch_stride = 0;
    matrix->attributes[rocsparselt_mat_num_batches].get(&num_batches);
    matrix->attributes[rocsparselt_mat_batch_stride].get(&batch_stride);
    //set number of batches to 1, since we only care the first batch under the boradcast case.
    if(batch_stride == 0)
    {
        num_batches  = 1;
        batch_stride = matrix->n * ld;
    }

    auto           num_cols   = op == rocsparselt_operation_none ? matrix->c_k : matrix->n;
    unsigned char* d_metadata = reinterpret_cast<unsigned char*>(d_out)
                                + rocsparselt_metadata_offset_in_compressed_matrix(
                                    num_cols, matrix->c_ld, num_batches, type);

#define COMPRESS_PARAMS(T)                                                                         \
    handle, m, n, stride0, stride1, batch_stride, c_stride0, c_stride1, c_batch_stride, m_stride0, \
        m_stride1, m_batch_stride, num_batches, op, order, reinterpret_cast<const T*>(d_in),       \
        reinterpret_cast<T*>(d_out), d_metadata, stream

    switch(type)
    {
    case rocsparselt_datatype_f16_r:
        return rocsparselt_smfmac_compress_template<__half>(COMPRESS_PARAMS(__half));
    case rocsparselt_datatype_bf16_r:
        return rocsparselt_smfmac_compress_template<hip_bfloat16>(COMPRESS_PARAMS(hip_bfloat16));
    case rocsparselt_datatype_i8_r:
        return rocsparselt_smfmac_compress_template<int8_t>(COMPRESS_PARAMS(int8_t));
    default:
        return rocsparselt_status_not_implemented;
    }
}

#ifdef __cplusplus
extern "C" {
#endif

rocsparselt_status rocsparselt_smfmac_compressed_size_impl(rocsparselt_mat_descr matrix,
                                                           rocsparselt_operation op,
                                                           size_t*               compressedSize)
{
    int64_t              num_cols;
    int64_t              c_ld;
    rocsparselt_datatype type;
    int                  num_batches = 1;
    int64_t              batch_stride;

    num_cols = op == rocsparselt_operation_none ? matrix->c_k : matrix->n;
    c_ld     = matrix->c_ld;
    type     = matrix->type;
    matrix->attributes[rocsparselt_mat_num_batches].get(&num_batches);
    matrix->attributes[rocsparselt_mat_batch_stride].get(&batch_stride);

    //set the number of batches to 1 since in the broadcast case, we only care about contents in first batch.
    if(batch_stride == 0) //boardcast case.
    {
        num_batches = 1;
    }

    int64_t metadata_offset
        = rocsparselt_metadata_offset_in_compressed_matrix(num_cols, c_ld, num_batches, type);

    *compressedSize = c_ld * num_cols / 4 * num_batches + metadata_offset;
    return rocsparselt_status_success;
}

/********************************************************************************
 * \brief
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_compressed_size(const rocsparselt_handle*      handle,
                                                      const rocsparselt_matmul_plan* plan,
                                                      size_t*                        compressedSize)

{
    // Check if handle is valid
    if(handle == nullptr || plan == nullptr || *handle == nullptr || *plan == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    // Check if pointer is valid
    if(compressedSize == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    {
        // Only support when Matrix A is a structured matrix.
        rocsparselt_mat_descr matrix;

        if((*plan)->matmul_descr->matrix_A->m_type == rocsparselt_matrix_type_structured)
        {
            matrix = (*plan)->matmul_descr->matrix_A;
        }
        else
        {
            return rocsparselt_status_not_implemented;
        }

        log_trace(*handle, "rocsparselt_smfmac_compressed_size");
        return rocsparselt_smfmac_compressed_size_impl(
            matrix, (*plan)->matmul_descr->op_A, compressedSize);
    }
}

/********************************************************************************
 * \brief
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_compressed_size2(const rocsparselt_handle*    handle,
                                                       const rocsparselt_mat_descr* sparseMatDescr,
                                                       size_t*                      compressedSize)

{
    // Check if handle is valid
    if(handle == nullptr || sparseMatDescr == nullptr || *handle == nullptr
       || *sparseMatDescr == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    // Check if pointer is valid
    if(compressedSize == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    {
        // Only support when Matrix A is a structured matrix.
        rocsparselt_mat_descr matrix = *sparseMatDescr;
        if(matrix->m_type != rocsparselt_matrix_type_structured)
        {
            return rocsparselt_status_not_implemented;
        }

        bool predict_compressed_info = false;
        if(matrix->c_ld == -1 && matrix->c_k == -1)
        {
            // do not know the operation type at this moment so assume which is rocsparselt_operation_none;
            // btw, the operation type does not impact the result of rocsparselt_smfmac_compressed_size_impl(),
            // since no matter which kind of operation type they will all get the same result:
            //    compressed size = compressed matrix size(m * n / 2 * sizeof(type) * num_batches)
            //                      + metadata size(m * n / 2 / 4 * num_batches).
            matrix->c_ld            = matrix->m;
            matrix->c_k             = matrix->n / 2;
            predict_compressed_info = true;
        }

        rocsparselt_operation op = matrix->c_ld == matrix->c_k ? rocsparselt_operation_transpose
                                                               : rocsparselt_operation_none;

        if(predict_compressed_info)
        {
            matrix->c_ld = -1;
            matrix->c_k  = -1;
        }

        log_trace(*handle, "rocsparselt_smfmac_compressed_size2");
        return rocsparselt_smfmac_compressed_size_impl(matrix, op, compressedSize);
    }
}

/********************************************************************************
 * \brief
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_compress(const rocsparselt_handle*      handle,
                                               const rocsparselt_matmul_plan* plan,
                                               const void*                    d_dense,
                                               void*                          d_compressed,
                                               hipStream_t                    stream)

{
    // Check if handle is valid
    if(handle == nullptr || plan == nullptr || *handle == nullptr || *plan == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    // Check if pointer is valid
    if(d_dense == nullptr || d_compressed == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    rocsparselt_mat_descr matrix;
    // Check if matrix A is a structured matrix
    if((*plan)->matmul_descr->matrix_A->m_type == rocsparselt_matrix_type_structured)
        matrix = (*plan)->matmul_descr->matrix_A;
    else
        return rocsparselt_status_not_implemented;

    log_trace(*handle, "rocsparselt_smfmac_compress");

    return rocsparselt_smfmac_compress_impl(
        *handle, matrix, (*plan)->matmul_descr->op_A, d_dense, d_compressed, stream);
}

/********************************************************************************
 * \brief
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_compress2(const rocsparselt_handle*    handle,
                                                const rocsparselt_mat_descr* sparseMatDescr,
                                                int                          isSparseA,
                                                rocsparselt_operation        op,
                                                const void*                  d_dense,
                                                void*                        d_compressed,
                                                hipStream_t                  stream)

{
    // Check if handle is valid
    if(handle == nullptr || sparseMatDescr == nullptr || *handle == nullptr
       || *sparseMatDescr == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    if(!isSparseA)
        return rocsparselt_status_not_implemented;

    if(op != rocsparselt_operation_none && op != rocsparselt_operation_transpose)
        return rocsparselt_status_invalid_value;

    // Check if pointer is valid
    if(d_dense == nullptr || d_compressed == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    rocsparselt_mat_descr matrix = *sparseMatDescr;
    // Check if matrix A is a structured matrix
    if(matrix->m_type != rocsparselt_matrix_type_structured)
        return rocsparselt_status_not_implemented;

    log_trace(*handle, "rocsparselt_smfmac_compress2");

    return rocsparselt_smfmac_compress_impl(*handle, matrix, op, d_dense, d_compressed, stream);
}

#ifdef __cplusplus
}
#endif