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
#include <sstream>
#include <miopen/kernel_info.hpp>
#include <miopen/gcn_asm_utils.hpp>
#include <miopen/solver.hpp>
#include <miopen/env.hpp>
#include <miopen/kernel_build_params.hpp>
#include <miopen/conv/tensors.hpp>
#include <boost/any.hpp>
#include <miopen/conv/invokers/flexgemm.hpp>

// clang-format off
static void get_solution(const miopen::ConvolutionContext& ctx, miopen::solver::ConvSolution& sol, const miopen::flexgemm::param_ufconv_t& p, const std::string& file_name, uint32_t relu)
{
    static const char* knames_ufco[]=
    {
        "suffco7x4_om"     ,
        "suffco7x4_om_relu",
        "suffco7x4_dm"     ,
        "suffco7x4_dm_relu",
        "suffco7x4_qm"     ,
        "suffco7x4_qm_relu",
        "suffco8x5_om"     ,
        "suffco8x5_om_relu",
        "suffco8x5_dm"     ,
        "suffco8x5_dm_relu",
        "suffco8x5_qm"     ,
        "suffco8x5_qm_relu",
        "suffco8x6_om"     ,
        "suffco8x6_om_relu",
        "suffco8x6_dm"     ,
        "suffco8x6_dm_relu",
        "suffco8x6_qm"     ,
        "suffco8x6_qm_relu",
        "suffco7x7_om"     ,
        "suffco7x7_om_relu",
        "suffco7x7_dm"     ,
        "suffco7x7_dm_relu",
        "suffco7x7_qm"     ,
        "suffco7x7_qm_relu",
        "sufbco7x4_om"     ,
        "sufbco7x4_dm"     ,
        "sufbco7x4_qm"     ,
        "sufbco8x5_om"     ,
        "sufbco8x5_dm"     ,
        "sufbco8x5_qm"     ,
        "sufbco8x6_om"     ,
        "sufbco8x6_dm"     ,
        "sufbco8x6_qm"     ,
        "sufbco7x7_om"     ,
        "sufbco7x7_dm"     ,
        "sufbco7x7_qm"     
    };
    static const uint32_t blk_ufco[]={128,256,256,256};
    uint32_t id=p.id&0xffff;
    uint32_t mode=p.id>>16;
    uint32_t shx=(id>0)&&(id<3)?8:7;
    uint32_t shy=4+id;
    uint32_t gdy=(p.n+(1<<shy)-1)>>shy;
    uint32_t gdx=p.ntidx>>shx;
    uint32_t routine=id*(3+p.dir)+p.dir*24+((mode<<(1^p.dir))|relu);
    const std::vector<size_t> block{blk_ufco[id],1,1};
    const std::vector<size_t> grid{gdy*blk_ufco[id],gdx,p.ng};
    std::ostringstream options;
    GenerateClangDefsym(options, "ROCM_METADATA_VERSION", ctx.rmv.UseV3()?5:4);
    miopen::solver::KernelInfo kinfo={ options.str(), block, grid, file_name, knames_ufco[routine] };
    sol.construction_params.push_back(kinfo);
}
static void get_solution(const miopen::ConvolutionContext& ctx, miopen::solver::ConvSolution& sol, const miopen::flexgemm::param_conv_t& p, const std::string& file_name, uint32_t relu)
{
    static const char* knames_co[]=
    {
        "sfco"        ,
        "sfco_relu"   ,
        "sfco7x4"     ,
        "sfco7x4_relu",
        "sfco8x5"     ,
        "sfco8x5_relu",
        "sfco8x6"     ,
        "sfco8x6_relu",
        "sfco7x7"     ,
        "sfco7x7_relu",
        "sbco7x4"     ,
        "sbco8x5"     ,
        "sbco8x6"     ,
        "sbco7x7"
    };
    static const uint32_t blk_co[]={256,128,256,256,256};
    std::ostringstream options;
    GenerateClangDefsym(options, "ROCM_METADATA_VERSION", ctx.rmv.UseV3()?5:4);
    if(p.spad!=0){
        uint32_t gdx=(p.pnx*p.pny*p.ng*p.inc+255)>>8;
        const std::vector<size_t> block{256,1,1};
        const std::vector<size_t> grid{gdx<<8,p.bs,1};
        miopen::solver::KernelInfo kinfo={ options.str(), block, grid, file_name, "padding2d" };
        sol.construction_params.push_back(kinfo);
    }

    if(p.dir!=0){
        uint32_t gdx=(((p.n+3)&~3)+63)>>6;
        const std::vector<size_t> block{64,1,1};
        const std::vector<size_t> grid{gdx<<6,p.inc,p.ng};
        miopen::solver::KernelInfo kinfo={ options.str(), block, grid, file_name, "perm2d_flip" };
        sol.construction_params.push_back(kinfo);
    }

    {
        uint32_t srelo=((p.k+15)&~15)+32;
        uint32_t ntid=p.ntidx>srelo?p.ntidx:srelo;
        uint32_t gdx=(ntid+63)>>6;
        const std::vector<size_t> block{64,1,1};
        const std::vector<size_t> grid{gdx<<6,1,1};
        miopen::solver::KernelInfo kinfo={ options.str(), block, grid, file_name, "genidx2d" };
        sol.construction_params.push_back(kinfo);
    }

    {
        uint32_t routine=p.dir==0?((p.id<<1)|relu):(10+p.id);
        uint32_t shx=((p.dir==0?0x78878:0x7887)>>(p.id<<2))&0xf;
        uint32_t shy=((p.dir==0?0x76545:0x7654)>>(p.id<<2))&0xf;
        uint32_t ngx=p.ntidx>>shx;
        uint32_t ngy=(p.n+(1<<shy)-1)>>shy;
        uint32_t gdx=p.pad!=0?gdx:gdy;
        uint32_t gdy=p.pad!=0?gdy:gdx;
        const std::vector<size_t> block{blk_co[p.id-p.dir],1,1};
        const std::vector<size_t> grid{gdx*blk_co[p.id-p.dir],gdy,p.ng};
        miopen::solver::KernelInfo kinfo={ options.str(), block, grid, file_name, knames_co[routine] };
        sol.construction_params.push_back(kinfo);
    }
}

namespace miopen {
namespace solver {
bool ConvFlexgemm::IsApplicable(const ConvolutionContext& ctx) const
{
    const auto name=ctx.GetStream().GetDeviceName();
    if((name!="gfx900"&&name!="gfx906")||ctx.direction.IsBackwardWrW()) return false;
    if((ctx.kernel_size_w|ctx.kernel_size_h|ctx.kernel_size_d)!=1){
        if(!ctx.Is2d()) return false;
        if(!ctx.direction.IsForward()){
            if((ctx.kernel_stride_w|ctx.kernel_stride_h|ctx.kernel_dilation_w|ctx.kernel_dilation_h)!=1)
                return false;
        }
    }
    return (ctx.IsFp32()&&(ctx.in_layout=="NCHW")&&(ctx.bias==0));
}
size_t ConvFlexgemm::GetWorkspaceSize(const ConvolutionContext& ctx) const
{
    if((ctx.kernel_size_w|ctx.kernel_size_h|ctx.kernel_size_d|
        ctx.kernel_stride_w|ctx.kernel_stride_h|ctx.kernel_stride_d|
        ctx.kernel_dilation_w|ctx.kernel_dilation_h|ctx.kernel_dilation_d)==1)
        return 0;
    return flexgemm::get_auxbuf_size(ctx);
}
ConvSolution ConvFlexgemm::GetSolution(const ConvolutionContext& ctx) const
{
    ConvSolution sol{};
    const std::string file_name="flexgemm_"+ctx.GetStream().GetDeviceName()+".s";
    uint32_t ksize=ctx.kernel_size_w|ctx.kernel_size_h|ctx.kernel_size_d;
    uint32_t stride=ctx.kernel_stride_w|ctx.kernel_stride_h|ctx.kernel_stride_d;
    uint32_t dilation=ctx.kernel_dilation_w|ctx.kernel_dilation_h|ctx.kernel_dilation_d;

    if((ksize|stride|dilation)==1){
        flexgemm::param_ufconv_t params{};
        flexgemm::build_params_ufconv(params, ctx);
        sol.workspce_sz=0;
        get_solution(ctx, sol, params, file_name, 0);
        sol.invoker_factory=conv::MakeFlexgemmInvokerFactory(params, 1.f);
    } else {
        flexgemm::param_conv_t params{};
        flexgemm::build_params_conv(params, ctx);
        sol.workspce_sz=params.spad+params.sperm+params.sidx;
        get_solution(ctx, sol, params, file_name, 0);
        sol.invoker_factory=conv::MakeFlexgemmInvokerFactory(params, 1.f);
    }
    return sol;
}
} //namespace solver
} //namespace miopen
// clang-format on