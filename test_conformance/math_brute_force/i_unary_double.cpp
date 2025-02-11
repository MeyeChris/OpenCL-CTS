//
// Copyright (c) 2017 The Khronos Group Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "common.h"
#include "function_list.h"
#include "test_functions.h"
#include "utility.h"

#include <cinttypes>
#include <cstring>

namespace {

int BuildKernel(const char *name, int vectorSize, cl_kernel *k, cl_program *p,
                bool relaxedMode)
{
    auto kernel_name = GetKernelName(vectorSize);
    auto source = GetUnaryKernel(kernel_name, name, ParameterType::Int,
                                 ParameterType::Double, vectorSize);
    std::array<const char *, 1> sources{ source.c_str() };
    return MakeKernel(sources.data(), sources.size(), kernel_name.c_str(), k, p,
                      relaxedMode);
}

using Kernels = std::array<clKernelWrapper, VECTOR_SIZE_COUNT>;

struct BuildKernelInfo2
{
    Kernels &kernels;
    Programs &programs;
    const char *nameInCode;
    bool relaxedMode; // Whether to build with -cl-fast-relaxed-math.
};

cl_int BuildKernelFn(cl_uint job_id, cl_uint thread_id UNUSED, void *p)
{
    BuildKernelInfo2 *info = (BuildKernelInfo2 *)p;
    cl_uint vectorSize = gMinVectorSizeIndex + job_id;
    return BuildKernel(info->nameInCode, vectorSize,
                       &(info->kernels[vectorSize]),
                       &(info->programs[vectorSize]), info->relaxedMode);
}

} // anonymous namespace

int TestFunc_Int_Double(const Func *f, MTdata d, bool relaxedMode)
{
    int error;
    Programs programs;
    Kernels kernels;
    int ftz = f->ftz || gForceFTZ;
    uint64_t step = getTestStep(sizeof(cl_double), BUFFER_SIZE);
    int scale =
        (int)((1ULL << 32) / (16 * BUFFER_SIZE / sizeof(cl_double)) + 1);

    logFunctionInfo(f->name, sizeof(cl_double), relaxedMode);

    // This test is not using ThreadPool so we need to disable FTZ here
    // for reference computations
    FPU_mode_type oldMode;
    DisableFTZ(&oldMode);

    Force64BitFPUPrecision();

    // Init the kernels
    {
        BuildKernelInfo2 build_info{ kernels, programs, f->nameInCode,
                                     relaxedMode };
        if ((error = ThreadPool_Do(BuildKernelFn,
                                   gMaxVectorSizeIndex - gMinVectorSizeIndex,
                                   &build_info)))
            return error;
    }

    for (uint64_t i = 0; i < (1ULL << 32); i += step)
    {
        // Init input array
        double *p = (double *)gIn;
        if (gWimpyMode)
        {
            for (size_t j = 0; j < BUFFER_SIZE / sizeof(cl_double); j++)
                p[j] = DoubleFromUInt32((uint32_t)i + j * scale);
        }
        else
        {
            for (size_t j = 0; j < BUFFER_SIZE / sizeof(cl_double); j++)
                p[j] = DoubleFromUInt32((uint32_t)i + j);
        }

        if ((error = clEnqueueWriteBuffer(gQueue, gInBuffer, CL_FALSE, 0,
                                          BUFFER_SIZE, gIn, 0, NULL, NULL)))
        {
            vlog_error("\n*** Error %d in clEnqueueWriteBuffer ***\n", error);
            return error;
        }

        // write garbage into output arrays
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            uint32_t pattern = 0xffffdead;
            memset_pattern4(gOut[j], &pattern, BUFFER_SIZE);
            if ((error =
                     clEnqueueWriteBuffer(gQueue, gOutBuffer[j], CL_FALSE, 0,
                                          BUFFER_SIZE, gOut[j], 0, NULL, NULL)))
            {
                vlog_error("\n*** Error %d in clEnqueueWriteBuffer2(%d) ***\n",
                           error, j);
                goto exit;
            }
        }

        // Run the kernels
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            size_t vectorSize = sizeValues[j] * sizeof(cl_double);
            size_t localCount = (BUFFER_SIZE + vectorSize - 1)
                / vectorSize; // BUFFER_SIZE / vectorSize  rounded up
            if ((error = clSetKernelArg(kernels[j], 0, sizeof(gOutBuffer[j]),
                                        &gOutBuffer[j])))
            {
                LogBuildError(programs[j]);
                goto exit;
            }
            if ((error = clSetKernelArg(kernels[j], 1, sizeof(gInBuffer),
                                        &gInBuffer)))
            {
                LogBuildError(programs[j]);
                goto exit;
            }

            if ((error =
                     clEnqueueNDRangeKernel(gQueue, kernels[j], 1, NULL,
                                            &localCount, NULL, 0, NULL, NULL)))
            {
                vlog_error("FAILED -- could not execute kernel\n");
                goto exit;
            }
        }

        // Get that moving
        if ((error = clFlush(gQueue))) vlog("clFlush failed\n");

        // Calculate the correctly rounded reference result
        int *r = (int *)gOut_Ref;
        double *s = (double *)gIn;
        for (size_t j = 0; j < BUFFER_SIZE / sizeof(cl_double); j++)
            r[j] = f->dfunc.i_f(s[j]);

        // Read the data back
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            if ((error =
                     clEnqueueReadBuffer(gQueue, gOutBuffer[j], CL_TRUE, 0,
                                         BUFFER_SIZE, gOut[j], 0, NULL, NULL)))
            {
                vlog_error("ReadArray failed %d\n", error);
                goto exit;
            }
        }

        if (gSkipCorrectnessTesting) break;

        // Verify data
        uint32_t *t = (uint32_t *)gOut_Ref;
        for (size_t j = 0; j < BUFFER_SIZE / sizeof(cl_double); j++)
        {
            for (auto k = gMinVectorSizeIndex; k < gMaxVectorSizeIndex; k++)
            {
                uint32_t *q = (uint32_t *)(gOut[k]);
                // If we aren't getting the correctly rounded result
                if (t[j] != q[j])
                {
                    if ((ftz || relaxedMode) && IsDoubleSubnormal(s[j]))
                    {
                        unsigned int correct0 = f->dfunc.i_f(0.0);
                        unsigned int correct1 = f->dfunc.i_f(-0.0);
                        if (q[j] == correct0 || q[j] == correct1) continue;
                    }

                    uint32_t err = t[j] - q[j];
                    if (q[j] > t[j]) err = q[j] - t[j];
                    vlog_error(
                        "\nERROR: %sD%s: %d ulp error at %.13la: *%d vs. %d\n",
                        f->name, sizeNames[k], err, ((double *)gIn)[j], t[j],
                        q[j]);
                    error = -1;
                    goto exit;
                }
            }
        }

        if (0 == (i & 0x0fffffff))
        {
            if (gVerboseBruteForce)
            {
                vlog("base:%14" PRIu64 " step:%10" PRIu64
                     "  bufferSize:%10d \n",
                     i, step, BUFFER_SIZE);
            }
            else
            {
                vlog(".");
            }

            fflush(stdout);
        }
    }

    if (!gSkipCorrectnessTesting)
    {
        if (gWimpyMode)
            vlog("Wimp pass");
        else
            vlog("passed");
    }

    vlog("\n");

exit:
    RestoreFPState(&oldMode);
    return error;
}
