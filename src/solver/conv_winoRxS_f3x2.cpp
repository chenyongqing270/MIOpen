/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
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

#include <miopen/solver.hpp>

#include <miopen/env.hpp>
#include <miopen/kernel.hpp>
#include <miopen/kernel_build_params.hpp>
#include <miopen/stringutils.hpp>
#include <miopen/conv/compiled_in_parameters.hpp>
#include <miopen/conv/data_invoke_params.hpp>

#include <boost/any.hpp>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_AMD_WINOGRAD_RXS_F3X2)

/// \return v rounded up (towards +inf) to the nearest multiple of m.
/// Defined for positive values only.
static inline int Ceiling(const int v, const int m)
{
    assert(m > 0 && v >= 0);
    if(v % m != 0)
    {
        return (v / m + 1) * m;
    }
    return v;
}

/// \return Value equivalent to ceil(x/y).
/// Defined for positive values only.
static inline int CeilDiv(const int x, const int y)
{
    assert(y > 0);
    return Ceiling(x, y) / y;
}

/// \return Value equivalent to floor(x/y).
/// Defined for positive values only.
static inline int FloorDiv(const int x, const int y)
{
    assert(x >= 0 && y > 0);
    return x / y;
}

/// \todo Consider re-using code from RxS.
static inline bool IsShaderContraintsMet(const int R,
                                         const int S,
                                         const int R_stride,
                                         const int S_stride,
                                         const int C,
                                         const int K,
                                         const int H,
                                         const int W,
                                         const int OH,
                                         const int OW,
                                         const int N,
                                         const miopen::ConvolutionContext& params,
                                         const bool fp16,
                                         const unsigned filter_tile_size)
{
    const auto TILE   = static_cast<int>(filter_tile_size);
    const int TILE_X2 = TILE * 2;
    // Calculate padded filter size first.
    // If stride = 1: if S <= 3 it is padded to 3,
    // otherwise S is padded to smallest 6*n for some integer n
    // If stride = 2: S is always padded to smallest 6*n for some integer n
    int padded_S = 0;
    if(S_stride == 1)
    {
        if(S <= TILE)
        {
            padded_S = TILE;
        }
        else
        {
            padded_S = Ceiling(S, TILE_X2);
        }
    }
    else
    {
        padded_S = Ceiling(S, TILE_X2);
    }
    // If stride = 1: R is always padded to smallest 3*m for some integer m
    // If stride = 2: if R % 6 ==1 then R is padded to smallest 3*m for some
    // integer m, otherwise R is padded to smallest 6*m for some integer m
    int padded_R = 0;
    if(R_stride == 1)
    {
        padded_R = Ceiling(R, TILE);
    }
    else
    {
        if(R % TILE_X2 == 1)
        {
            padded_R = Ceiling(R, TILE);
        }
        else
        {
            padded_R = Ceiling(R, TILE_X2);
        }
    }
    // Check C restrictions:
    // For FP16, all C restrictions shall be multipled by 2.
    // This implicitly introduces restriction that C must be even.
    if(fp16 && C % 2 != 0)
    {
        return false;
    }
    // If stride == 1 and S <= 3 then C needs to be even, otherwise not
    if(S_stride == 1 && S <= TILE && C % (fp16 ? 4 : 2) != 0)
    {
        return false;
    }
    const bool is_dilated_stride_2 = (params.direction.IsBackwardData() && S_stride != 1);
    if(fp16)
    {
        if(is_dilated_stride_2)
        {
            if(C % 4 != 0)
                return false;
            // In dilation mode with stride== 2 the following should be satisfied:
            // C * (ceil(R/6) + floor((R+4)/6)) * ceil(S/6) >= 18*2 (fp16)
            const auto k = CeilDiv(R, TILE_X2) + FloorDiv((R + TILE + 1), TILE_X2);
            const auto l = CeilDiv(S, TILE_X2);
            if(C * k * l < 18 * 2)
                return false;
        }
        if(padded_R * padded_S * C < TILE * TILE * 18 * 2)
            return false;
    }
    else
    {
        // 9_0_14 readme: Additional limitations in the dilated case are R> 1 and  C %2==0
        if(is_dilated_stride_2)
        {
            if(!(R > 1))
                return false;
            if(!(C % 2 == 0))
                return false;
        }
        // If the padded_R x padded_S filter size from above is 3*k x 3*l
        // or (special case for dilated with stride 2) 3*k x 6*l, then
        // it should be that k*l*C  >=18
        assert(padded_R % TILE == 0 && padded_S % (is_dilated_stride_2 ? TILE_X2 : TILE) == 0);
        const int k = padded_R / TILE;
        const int l = padded_S / (is_dilated_stride_2 ? TILE_X2 : TILE);
        if(k * l * C < 18)
            return false;
    }
    // Padding for bwd data shall not be negative.
    /// \todo Either remove WrW related code or re-use function from RxS
    if(params.direction.IsBackwardData() || params.direction.IsBackwardWrW())
    {
        if(!(0 <= params.GetBackwardPadW() && params.GetBackwardPadW() < std::pow(2, 16)))
            return false;
        if(!(0 <= params.GetBackwardPadH() && params.GetBackwardPadH() < std::pow(2, 16)))
            return false;
    }
    const auto grid_workgroup_count_x = params.GetStream().GetMaxComputeUnits();
    assert(params.weights_layout.length() == 0);
    // clang-format off
    // Check implementation limits.
    return N < std::pow(2, 16)
        && C < std::pow(2, 16)
        && K < std::pow(2, 16)
        && H < std::pow(2, 16)
        && W < std::pow(2, 16)
        && OH < std::pow(2, 16)
        && OW < std::pow(2, 16)
        && params.pad_w < std::pow(2, 16)
        && params.pad_h < std::pow(2, 16)
        && S < std::pow(2, 16)
        && R < std::pow(2, 16)
        && grid_workgroup_count_x < std::pow(2, 16)
        && (C * H * W) <= std::pow(2, 28)
        && (OH * OW) <= std::pow(2, 23)
        && (K * OH * OW) <= std::pow(2, 28)
        && (K * R * S) <= std::pow(2, 28)
        && (C * R * S) <= std::pow(2, 28);
    // clang-format on
}

namespace miopen {
namespace solver {

bool ConvBinWinogradRxSf3x2::IsApplicable(const ConvolutionContext& params) const
{
    if(!params.Is2d())
        return false;
    if(!params.IsFp32())
        return false;
    if(miopen::IsDisabled(MIOPEN_DEBUG_AMD_WINOGRAD_RXS_F3X2{}))
        return false;
    if(params.direction.IsBackwardWrW())
        return false;
    if(!params.use_asm_kernels)
        return false;
    if(!params.rmv.IsV2orV3())
        return false;

    const auto name = params.GetStream().GetDeviceName();
    if(!(StartsWith(name, "gfx9")))
        return false;

    // clang-format off
    if (! (params.kernel_stride_w == 1
        && params.kernel_stride_w == params.kernel_stride_h
        && params.kernel_dilation_w == 1
        && params.kernel_dilation_h == 1
        && params.bias == 0
        && params.group_counts == 1
        && params.in_layout == "NCHW"))
        return false;
    // clang-format on

    return IsShaderContraintsMet(params.kernel_size_h, // RxS
                                 params.kernel_size_w,
                                 params.kernel_stride_h,
                                 params.kernel_stride_w,
                                 params.n_inputs,  // C
                                 params.n_outputs, // K
                                 params.in_height, // HxW
                                 params.in_width,
                                 params.out_height, // OHxOW
                                 params.out_width,
                                 params.batch_sz, // N
                                 params,
                                 false,
                                 2);
}

ConvSolution ConvBinWinogradRxSf3x2::GetSolution(const ConvolutionContext& params) const
{
    ConvSolution result;
    const auto n_groups = params.GetStream().GetMaxComputeUnits();
    KernelInfo kernel;

    kernel.g_wk.push_back(512 * n_groups);
    kernel.g_wk.push_back(1);
    kernel.g_wk.push_back(1);

    kernel.l_wk.push_back(512);
    kernel.l_wk.push_back(1);
    kernel.l_wk.push_back(1);

    KernelBuildParameters options{
        {"ROCM_METADATA_VERSION", params.rmv.UseV3() ? 5 : 4},
    };
    kernel.comp_options = options.GenerateFor(kbp::GcnAsm{});

    kernel.kernel_name = "miopenSp3AsmConvRxSf3x2";
    kernel.kernel_file = "Conv_Winograd_v16_5_0_stride1.s";

    result.construction_params.push_back(kernel);
    const auto is_forward = params.direction.IsForward();

    constexpr int F_REVERSE_R = 1 << 0;
    constexpr int F_REVERSE_S = 1 << 1;
    constexpr int F_FLIP_K_C  = 1 << 2;
    // These are not used yet. Nevertheless let's keep as a shader documentation.
    // constexpr int F_FLIP_DATA_N_C = 1 << 3; // Unsupported in f3x2.
    // constexpr int F_FLIP_OUT_N_K = 1 << 4; // Unsupported in f3x2.
    // constexpr int L_F_ADDR_INDIRECT  = 1 << 6;
    // constexpr int L_F_BIAS  = 1 << 7;
    // constexpr int L_F_LEAKY_RELU  = 1 << 8;
    constexpr int L_F_NKC_STRIDES = 1 << 9;

    int flags         = is_forward ? 0 : F_REVERSE_R + F_REVERSE_S + F_FLIP_K_C;
    int reserved      = 0;
    int* reserved_ptr = nullptr;
    int N, C, H, W, K, n_groups_, out_H, out_W, R, S, pad_H, pad_W;
    GetCompiledInParameters(
        params, &N, &C, &H, &W, &K, &n_groups_, &out_H, &out_W, &R, &S, &pad_H, &pad_W);
    MIOPEN_LOG_I2(" N=" << N << " C=" << C << " H=" << H << " W=" << W << " K=" << K << " n_groups="
                        << n_groups_
                        << " flags="
                        << flags
                        << " R="
                        << R
                        << " S="
                        << S
                        << " pad_H="
                        << pad_H
                        << " pad_W="
                        << pad_W
                        << " out_H="
                        << out_H
                        << " out_W="
                        << out_W);

    flags += L_F_NKC_STRIDES;
    /// \todo Consider using BufferInfo to compute strides
    constexpr int SIZEOF_DATA = 4;
    int d_C_stride            = H * W * SIZEOF_DATA;
    int d_N_stride            = C * d_C_stride;
    int f_C_stride            = R * S * SIZEOF_DATA * (is_forward ? 1 : K);
    int f_K_stride            = R * S * SIZEOF_DATA * (is_forward ? C : 1);
    int o_K_stride            = out_H * out_W * SIZEOF_DATA;
    int o_N_stride            = K * o_K_stride;

    MIOPEN_LOG_I2("...flags=" << flags << " d_N_stride=" << d_N_stride << " d_C_stride="
                              << d_C_stride
                              << " f_K_stride="
                              << f_K_stride
                              << " f_C_stride="
                              << f_C_stride
                              << " o_N_stride="
                              << o_N_stride
                              << " o_K_stride="
                              << o_K_stride);

    result.invoker_factory = [=](const std::vector<Kernel>& kernels) {
        return [=](const Handle& handle, const boost::any& ctx) {
            const auto k        = handle.Run(kernels[0]);
            const auto& fwd_ctx = ctx.CastTo<conv::DataInvokeParams>();
            const auto& tensors = fwd_ctx.tensors;

            k(N,
              C,
              H,
              W,
              K,
              n_groups_,
              flags,
              reserved,
              tensors.in,
              tensors.w,
              tensors.out,
              reserved_ptr,
              R,
              S,
              pad_H,
              pad_W,
              out_H,
              out_W,
              reserved_ptr,
              reserved,
              d_N_stride,
              d_C_stride,
              f_K_stride,
              f_C_stride,
              o_N_stride,
              o_K_stride);
        };
    };

    return result;
}

} // namespace solver
} // namespace miopen
