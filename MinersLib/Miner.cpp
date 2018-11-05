/*
 This file is part of cpp-ethereum.

 cpp-ethereum is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 cpp-ethereum is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
 */
/** @file Miner.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2015
 * @author Polyminer1 <https://github.com/polyminer1>
 * @date 2018
 */


#include "precomp.h"
#include "MinersLib/Miner.h"
#include "corelib/PascalWork.h"
#include "MinersLib/CLMinerBase.h"
#include "MinersLib/Global.h"

Miner::Miner(std::string const& _name, FarmFace& _farm, unsigned globalWorkMult, unsigned localWorkSize, U32 gpuIndex) :
    Worker(_name),
    m_isInitializing(false),
    m_globalIndex(gpuIndex),
    m_farm(_farm),
    m_globalWorkMult(globalWorkMult),
    m_localWorkSize(localWorkSize),
    m_workReadyEvent(false, false)
{
    m_gpuInfoCache = &GpuManager::Gpus[m_globalIndex];
    m_isInitializationDone = false;
}

U64 Miner::GetHashRatePerSec() 
{
    U64 rate = 0;
    if (m_hashCountTime)
    {
        S64 dt = TimeGetMilliSec() - m_hashCountTime;
        
        if (dt > 0)
            rate = (U64)round(m_hashCount /(float)(dt/1000.0f));            
        else
        {
            //handle time change !
        }
        AtomicAdd(m_resetHashRateTime, 1);
    }

    return rate;
}

void Miner::SetWork(PascalWorkSptr _work)
{
    {
        Guard l(m_workMutex);
        m_workTemplate = _work;
    }
    m_workReadyEvent.SetDone();
}

void Miner::SetWorkpackageDirty()
{
    AtomicSet(m_workpackageDirty, 1);
}

void Miner::InitFromFarm(U32 relativeIndex) 
{ 
    m_relativeIndex = relativeIndex; 
}

void Miner::StartWorking()
{
    Worker::StartWorking();
}

void Miner::WorkLoop()
{
    try 
    {
        while (true)
        {
            if (!WorkLoopStep())
                break;
        }
    }
    catch (cl::Error const& _e)
    {
        //CL_INVALID_COMMAND_QUEUE -36
        RHMINER_PRINT_EXCEPTION_EX("OpenCL Error", _e.what());
        throw _e;
    }
    catch (...)
    {
        PrintOut("Unknown Exception in %s\n", __FUNCTION__);
    }
}


void Miner::TryResetHashCount()
{
    U64 reset = AtomicSet(m_resetHashRateTime, 0);
    if (reset)
    {
        m_hashCount = 0;
        m_hashCountTime = TimeGetMilliSec();
    }
}

void Miner::Pause()
{
    m_workReadyEvent.Reset();
}

void Miner::Kill()
{
    m_workReadyEvent.SetDone();
    Worker::Kill();
}

void Miner::AddHashCount(U64 hashes)
{ 
    U64 reset = AtomicSet(m_resetHashRateTime, 0);
    if (reset)
    {
        m_hashCount = hashes;
        m_hashCountTime = TimeGetMilliSec();
    }
    else
    {
        m_hashCount += hashes;
    }
}

void Miner::GetTemp(U32& temp, U32& fan)
{
    temp = 0;
    fan = 0;
#ifndef RH_COMPILE_CPU_ONLY
    if (m_nvmlh) 
    {
		wrap_nvml_get_tempC(m_nvmlh, m_nvmlh->cuda_nvml_device_id[m_globalIndex], &temp);
		wrap_nvml_get_fanpcnt(m_nvmlh, m_nvmlh->cuda_nvml_device_id[m_globalIndex], &fan);
	}
	if (m_adlh) 
    {
		wrap_adl_get_tempC(m_adlh, m_globalIndex, &temp);
		wrap_adl_get_fanpcnt(m_adlh, m_globalIndex, &fan);
	}
#endif //RH_COMPILE_CPU_ONLY
}


string SolutionStats::ToString(U64 lst)
{
    string str;
    std::vector<unsigned> gpuIdx = GpuManager::GetEnabledGPUIndices();
    auto ResToStr = [&](unsigned* arr, char ai)
    {
        string str;
        U32 total = 0;
        U32 gpuCount = 0;
        for (unsigned i = 0; i < gpuIdx.size(); i++)
            total += arr[gpuIdx[i]];

        str += std::to_string(total);

        if (gpuIdx.size() > 1 && total)
        {
            str += " (";
            for(int i=0; i < gpuIdx.size(); i++)
            {
                str += FormatString("%d", arr[gpuIdx[i]]);
                if (i + 1 !=  gpuIdx.size())
                    str += " ";
            }
            str += ")";
        }
        return str;
    };
    str = "Shares: Accepted " + ResToStr(accepts, 'a');
    str += "  Rejected " + ResToStr(rejects, 'r');
    str += "  Failed " + ResToStr(failures, 'f');
    str += FormatString(" Up for %s", SecondsToStr((U64)Elapsed()));

    return str;
}


string WorkingProgress::TemperatureToString()
{
    string stemp;
    //GPU0 t=63C fan=31%, GPU1 t=64C fan=24%, GPU2 t=63C fan=24%, GPU3 t=63C fan=24%, GPU4 t=63C fan=25%, GPU5 t=63C fan=24%
    U32 gpuCount = 0;
    for(int i=0; i < temperature.size(); i++)
        if (GpuManager::Gpus[gpuGlobalIndex[i]].gpuType != GpuType_CPU)
            gpuCount++;

    if (gpuCount)
    {
        stemp = "Temp: ";
        for(int i=0; i < temperature.size(); i++)
        {
            if (GpuManager::Gpus[gpuGlobalIndex[i]].gpuType != GpuType_CPU)
                stemp += FormatString("%s %dC %d%%  ", GpuManager::Gpus[gpuGlobalIndex[i]].gpuName.c_str(), temperature[i], fan[i]);
        }
    }
    return stemp;
}


string WorkingProgress::ToString()
{
    float mh = (float)totalHashRate;
    string str;
    if (minersHasheRate.size() == 1)
    {
        str = FormatString("Total: %s %s.", GpuManager::Gpus[gpuGlobalIndex[0]].gpuName.c_str(), HashrateToString(mh));
    }
    else
    {
        str = FormatString("Total: %s ", HashrateToString(mh));

        if (minersHasheRate.size() > 1)
        {
            str += "(";
            for (unsigned i = 0; i < minersHasheRate.size(); i++)
            {
                mh = pround(minersHasheRate[i], 2);
                str += FormatString("%s %s", GpuManager::Gpus[gpuGlobalIndex[i]].gpuName.c_str(), HashrateToString(mh));
                if (i + 1 != minersHasheRate.size())
                    str += " ";
            }

            str += "). ";
        }
    }

    return str;
}

