/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "Compiler/MetaDataApi/IGCMetaDataHelper.h"
#include "common/MDFrameWork.h"

using namespace IGC;
using namespace IGC::IGCMD;

void IGCMetaDataHelper::addFunction(MetaDataUtils& mdUtils, llvm::Function* pFunc, FunctionTypeMD type)
{
    auto finfo = FunctionInfoMetaDataHandle(FunctionInfoMetaData::get());
    finfo->setType(type);
    mdUtils.setFunctionsInfoItem(pFunc, finfo);
    mdUtils.save(pFunc->getContext());
}

void IGCMetaDataHelper::moveFunction(
    MetaDataUtils& mdUtils,
    ModuleMetaData& MD,
    llvm::Function* OldFunc, llvm::Function* NewFunc)
{
    auto oldFuncIter = mdUtils.findFunctionsInfoItem(OldFunc);
    if (oldFuncIter != mdUtils.end_FunctionsInfo())
    {
        mdUtils.setFunctionsInfoItem(NewFunc, oldFuncIter->second);
        mdUtils.eraseFunctionsInfoItem(oldFuncIter);
    }

    auto& FuncMD = MD.FuncMD;
    auto loc = FuncMD.find(OldFunc);
    if (loc != FuncMD.end())
    {
        auto funcInfo = loc->second;
        FuncMD.erase(OldFunc);
        FuncMD[NewFunc] = funcInfo;
    }
}

void IGCMetaDataHelper::removeFunction(
    MetaDataUtils& mdUtils,
    ModuleMetaData& MD,
    llvm::Function* Func)
{
    auto oldFuncIter = mdUtils.findFunctionsInfoItem(Func);
    if (oldFuncIter != mdUtils.end_FunctionsInfo())
    {
        mdUtils.eraseFunctionsInfoItem(oldFuncIter);
    }

    auto& FuncMD = MD.FuncMD;
    auto loc = FuncMD.find(Func);
    if (loc != FuncMD.end())
    {
        auto funcInfo = loc->second;
        FuncMD.erase(Func);
    }
}

uint32_t IGCMetaDataHelper::getThreadGroupSizeHint(MetaDataUtils& mdUtils, llvm::Function* pKernelFunc)
{
    FunctionInfoMetaDataHandle finfo = mdUtils.getFunctionsInfoItem(pKernelFunc);
    uint32_t size = 0;
    if (finfo->getThreadGroupSizeHint()->hasValue())
    {
        size = finfo->getThreadGroupSizeHint()->getXDim() *
            finfo->getThreadGroupSizeHint()->getYDim() *
            finfo->getThreadGroupSizeHint()->getZDim();
    }
    return size;
}

uint32_t IGCMetaDataHelper::getThreadGroupSize(MetaDataUtils& mdUtils, llvm::Function* pKernelFunc)
{
    FunctionInfoMetaDataHandle finfo = mdUtils.getFunctionsInfoItem(pKernelFunc);
    uint32_t size = 0;
    if (finfo->getThreadGroupSize()->hasValue())
    {
        size = finfo->getThreadGroupSize()->getXDim() *
            finfo->getThreadGroupSize()->getYDim() *
            finfo->getThreadGroupSize()->getZDim();
    }
    return size;
}