/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "LayerMemoryModel.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>

#include "LayerConfig.hpp"
#include "LayerDispatch.hpp"
#include "LayerLog.hpp"

namespace VkSim
{

namespace
{

constexpr size_t kMaxWriteRecords = 64;

// Report kinds (one report per resource, device and kind until the device's data is fresh again)
constexpr int kReportStale       = 0;
constexpr int kReportNoInstance  = 1;
constexpr int kReportPeerFeature = 2;

uint32_t MipExtent(uint32_t Extent, uint32_t Mip) { return (std::max)(1u, Extent >> Mip); }

Region Resolve(const ResourceState& Res, Region R)
{
    if (!Res.Image)
    {
        if (R.Offset > Res.Size)
            R.Offset = Res.Size;
        if (R.Size == VK_WHOLE_SIZE || R.Offset + R.Size > Res.Size)
            R.Size = Res.Size - R.Offset;
        return R;
    }
    if (R.MipCount == VK_REMAINING_MIP_LEVELS || R.MipBase + R.MipCount > Res.Mips)
        R.MipCount = Res.Mips > R.MipBase ? Res.Mips - R.MipBase : 0;
    if (R.LayerCount == VK_REMAINING_ARRAY_LAYERS || R.LayerBase + R.LayerCount > Res.Layers)
        R.LayerCount = Res.Layers > R.LayerBase ? Res.Layers - R.LayerBase : 0;
    if (!R.FullRect)
    {
        const int32_t W = static_cast<int32_t>(MipExtent(Res.Extent.width, R.MipBase));
        const int32_t H = static_cast<int32_t>(MipExtent(Res.Extent.height, R.MipBase));
        const int32_t D = static_cast<int32_t>(MipExtent(Res.Extent.depth, R.MipBase));
        if (R.X0 <= 0 && R.Y0 <= 0 && R.Z0 <= 0 && R.X1 >= W && R.Y1 >= H && R.Z1 >= D)
            R.FullRect = true;
    }
    return R;
}

bool RangesOverlap(uint64_t A0, uint64_t ACount, uint64_t B0, uint64_t BCount) { return A0 < B0 + BCount && B0 < A0 + ACount; }
bool RangeContains(uint64_t A0, uint64_t ACount, uint64_t B0, uint64_t BCount) { return A0 <= B0 && B0 + BCount <= A0 + ACount; }

bool Overlaps(bool Image, const Region& A, const Region& B)
{
    if (!Image)
        return RangesOverlap(A.Offset, A.Size, B.Offset, B.Size);
    if (!RangesOverlap(A.MipBase, A.MipCount, B.MipBase, B.MipCount) || !RangesOverlap(A.LayerBase, A.LayerCount, B.LayerBase, B.LayerCount))
        return false;
    if (A.FullRect || B.FullRect)
        return true;
    return A.X0 < B.X1 && B.X0 < A.X1 && A.Y0 < B.Y1 && B.Y0 < A.Y1 && A.Z0 < B.Z1 && B.Z0 < A.Z1;
}

bool Contains(bool Image, const Region& A, const Region& B)
{
    if (!Image)
        return RangeContains(A.Offset, A.Size, B.Offset, B.Size);
    if (!RangeContains(A.MipBase, A.MipCount, B.MipBase, B.MipCount) || !RangeContains(A.LayerBase, A.LayerCount, B.LayerBase, B.LayerCount))
        return false;
    if (A.FullRect)
        return true;
    return !B.FullRect && A.X0 <= B.X0 && B.X1 <= A.X1 && A.Y0 <= B.Y0 && B.Y1 <= A.Y1 && A.Z0 <= B.Z0 && B.Z1 <= A.Z1;
}

// Whether the newest data of some part of R is not in Instance; Writers receives the instances that have it
bool IsStale(const ResourceState& Res, const Region& R, uint32_t Instance, uint32_t& Writers)
{
    const uint32_t Bit = 1u << Instance;
    for (size_t i = Res.Writes.size(); i-- > 0;)
    {
        const WriteRecord& W = Res.Writes[i];
        if (!Overlaps(Res.Image, W.R, R))
            continue;
        if ((W.Instances & Bit) != 0)
        {
            if (Contains(Res.Image, W.R, R))
                return false; // R was last written as a whole, including this instance
            continue;
        }
        // This write missed the instance: is its part of R overwritten later, including the instance?
        bool Covered = false;
        for (size_t j = i + 1; j < Res.Writes.size() && !Covered; ++j)
        {
            const WriteRecord& Newer = Res.Writes[j];
            Covered = (Newer.Instances & Bit) != 0 && (Contains(Res.Image, Newer.R, W.R) || Contains(Res.Image, Newer.R, R));
        }
        if (!Covered)
        {
            Writers = W.Instances;
            return true;
        }
    }
    return false;
}

void AddWrite(ResourceState& Res, const Region& R, uint32_t Instances)
{
    // Older writes entirely overwritten by this one no longer matter
    Res.Writes.erase(std::remove_if(Res.Writes.begin(), Res.Writes.end(), [&](const WriteRecord& W) { return Contains(Res.Image, R, W.R); }), Res.Writes.end());
    Res.Writes.push_back(WriteRecord{R, Instances});
    if (Res.Writes.size() > kMaxWriteRecords)
        Res.Writes.erase(Res.Writes.begin());
}

void DescribeRegion(const ResourceState& Res, const Region& R, char* Buf, size_t Size)
{
    if (!Res.Image)
        std::snprintf(Buf, Size, "bytes %llu-%llu", static_cast<unsigned long long>(R.Offset), static_cast<unsigned long long>(R.Offset + R.Size));
    else if (R.FullRect)
        std::snprintf(Buf, Size, "mip %u (+%u), layer %u (+%u)", R.MipBase, R.MipCount - 1, R.LayerBase, R.LayerCount - 1);
    else
        std::snprintf(Buf, Size, "mip %u, layer %u (+%u), [%d,%d]-[%d,%d]", R.MipBase, R.LayerBase, R.LayerCount - 1, R.X0, R.Y0, R.X1, R.Y1);
}

VkPeerMemoryFeatureFlags RequiredPeerFeature(ACCESS_TYPE Type)
{
    switch (Type)
    {
        case ACCESS_COPY_READ: return VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT;
        case ACCESS_COPY_WRITE: return VK_PEER_MEMORY_FEATURE_COPY_DST_BIT;
        case ACCESS_GENERIC_READ: return VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT;
        default: return VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
    }
}

const char* AccessName(ACCESS_TYPE Type)
{
    switch (Type)
    {
        case ACCESS_COPY_READ: return "copies from";
        case ACCESS_COPY_WRITE: return "copies to";
        case ACCESS_GENERIC_READ: return "reads";
        default: return "writes";
    }
}

bool ShouldReport(MemoryModel& M, uint64_t Handle, uint32_t Device, int Kind)
{
    return M.Reported.insert(std::make_tuple(Handle, Device, Kind)).second;
}

void ExecuteOp(DeviceData& D, const Op& O, uint32_t Devices)
{
    MemoryModel& M = *D.Model;
    // Reads first: a copy reads its source before writing its destination
    for (int Pass = 0; Pass < 2; ++Pass)
    {
        for (const Access& A : O.Accesses)
        {
            const bool IsWrite = A.Type == ACCESS_COPY_WRITE || A.Type == ACCESS_GENERIC_WRITE;
            if (IsWrite != (Pass == 1))
                continue;
            auto ResIt = M.Resources.find(A.Handle);
            if (ResIt == M.Resources.end() || !ResIt->second.Tracked)
                continue;
            ResourceState& Res      = ResIt->second;
            const Region   R        = Resolve(Res, A.R);
            auto           MemIt    = M.Memory.find(reinterpret_cast<uint64_t>(Res.Memory));
            const uint32_t Existing = MemIt != M.Memory.end() ? MemIt->second.Instances : AllNodesMask(D.NodeCount);
            uint32_t       Written  = 0;
            char           Where[96];
            for (uint32_t Dev = 0; Dev < D.NodeCount; ++Dev)
            {
                if ((Devices & (1u << Dev)) == 0)
                    continue;
                const uint32_t Instance = Res.DeviceInstance[Dev];
                if ((Existing & (1u << Instance)) == 0)
                {
                    if (GetConfig().Validate && ShouldReport(M, A.Handle, Dev, kReportNoInstance))
                        ReportValidationError("%s: device %u %s %s 0x%llx through the memory instance of device %u, which does not exist (the memory was allocated for device mask 0x%X)",
                                              O.Api, Dev, AccessName(A.Type), Res.Image ? "image" : "buffer", static_cast<unsigned long long>(A.Handle), Instance, Existing);
                    continue;
                }
                if (Instance != Dev && GetConfig().Validate && (GetConfig().PeerMemoryFeatures & RequiredPeerFeature(A.Type)) == 0 &&
                    ShouldReport(M, A.Handle, Dev, kReportPeerFeature))
                {
                    ReportValidationError("%s: device %u %s %s 0x%llx in the memory of device %u (peer memory), but the group does not support that kind of peer access (peer memory features 0x%X)",
                                          O.Api, Dev, AccessName(A.Type), Res.Image ? "image" : "buffer", static_cast<unsigned long long>(A.Handle), Instance, GetConfig().PeerMemoryFeatures);
                }
                if (IsWrite)
                {
                    Written |= 1u << Instance;
                }
                else
                {
                    uint32_t Writers = 0;
                    if (GetConfig().Validate && IsStale(Res, R, Instance, Writers) && ShouldReport(M, A.Handle, Dev, kReportStale))
                    {
                        DescribeRegion(Res, R, Where, sizeof(Where));
                        ReportValidationError("%s: device %u %s %s 0x%llx (%s) whose newest data is only in the memory instance(s) 0x%X. Each device of a group has its own "
                                              "instance of device-local memory: write the data on device %u too, or copy it there through peer memory",
                                              O.Api, Dev, AccessName(A.Type), Res.Image ? "image" : "buffer", static_cast<unsigned long long>(A.Handle), Where, Writers, Dev);
                    }
                }
            }
            if (IsWrite && Written != 0)
            {
                AddWrite(Res, R, Written);
                for (uint32_t Dev = 0; Dev < D.NodeCount; ++Dev)
                    if ((Written & (1u << Res.DeviceInstance[Dev])) != 0)
                        M.Reported.erase(std::make_tuple(A.Handle, Dev, kReportStale));
            }
        }
    }
}

void Execute(DeviceData& D, VkCommandBuffer CmdBuf, uint32_t SubmitMask, int Depth)
{
    MemoryModel& M  = *D.Model;
    auto         It = M.Commands.find(CmdBuf);
    if (It == M.Commands.end() || Depth > 4)
        return;
    const std::vector<Op> Ops = It->second.Ops; // secondaries may be recorded into the same map
    for (const Op& O : Ops)
    {
        const uint32_t Devices = O.DeviceMask & SubmitMask;
        if (Devices == 0)
            continue;
        for (VkCommandBuffer Secondary : O.Secondaries)
            Execute(D, Secondary, Devices, Depth + 1);
        if (!O.Accesses.empty())
            ExecuteOp(D, O, Devices);
    }
}

} // namespace

void ModelOnAllocate(DeviceData& D, VkDeviceMemory Memory, uint32_t MemoryType, uint32_t DeviceMask)
{
    MemoryAllocation A;
    const uint32_t   Heap = MemoryType < D.TypeHeap.size() ? D.TypeHeap[MemoryType] : 0;
    A.MultiInstance       = Heap < D.DeviceLocalHeap.size() && D.DeviceLocalHeap[Heap];
    // Without subset allocation the device mask is ignored: every device gets an instance
    A.Instances = A.MultiInstance && GetConfig().SubsetAllocation ? DeviceMask : AllNodesMask(D.NodeCount);
    D.Model->Memory[reinterpret_cast<uint64_t>(Memory)] = A;
}

void ModelOnBufferCreated(DeviceData& D, VkBuffer Buffer, const VkBufferCreateInfo& Info)
{
    ResourceState Res;
    Res.Image   = false;
    Res.Size    = Info.size;
    Res.Storage = (Info.usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                 VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) != 0;
    D.Model->Resources[reinterpret_cast<uint64_t>(Buffer)] = Res;
}

void ModelOnImageCreated(DeviceData& D, VkImage Image, const VkImageCreateInfo& Info)
{
    ResourceState Res;
    Res.Image   = true;
    Res.Extent  = Info.extent;
    Res.Mips    = Info.mipLevels;
    Res.Layers  = Info.arrayLayers;
    Res.Storage = (Info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    D.Model->Resources[reinterpret_cast<uint64_t>(Image)] = Res;
}

void ModelOnDestroyed(DeviceData& D, uint64_t Resource)
{
    D.Model->Resources.erase(Resource);
}

void ModelOnBind(DeviceData& D, uint64_t Resource, VkDeviceMemory Memory, const uint32_t* pDeviceIndices, uint32_t NumIndices, bool SplitInstance)
{
    MemoryModel& M     = *D.Model;
    auto         ResIt = M.Resources.find(Resource);
    if (ResIt == M.Resources.end())
        return;
    ResourceState& Res = ResIt->second;
    auto           Mem = M.Memory.find(reinterpret_cast<uint64_t>(Memory));
    Res.Memory         = Memory;
    Res.Tracked        = D.NodeCount > 1 && Mem != M.Memory.end() && Mem->second.MultiInstance && !Res.Storage && !SplitInstance;
    for (uint32_t Dev = 0; Dev < 32; ++Dev)
        Res.DeviceInstance[Dev] = (pDeviceIndices != nullptr && Dev < NumIndices) ? pDeviceIndices[Dev] : Dev;
    Res.Writes.clear();
}

Region WholeResource(const ResourceState& Res)
{
    Region R;
    R.Offset     = 0;
    R.Size       = Res.Size;
    R.MipCount   = Res.Mips;
    R.LayerCount = Res.Layers;
    return R;
}

Region BufferRegion(VkDeviceSize Offset, VkDeviceSize Size)
{
    Region R;
    R.Offset = Offset;
    R.Size   = Size;
    return R;
}

Region ImageRegion(const VkImageSubresourceLayers& Sub, VkOffset3D Offset, VkExtent3D Extent)
{
    Region R;
    R.MipBase    = Sub.mipLevel;
    R.MipCount   = 1;
    R.LayerBase  = Sub.baseArrayLayer;
    R.LayerCount = Sub.layerCount;
    R.FullRect   = false;
    R.X0         = Offset.x;
    R.Y0         = Offset.y;
    R.Z0         = Offset.z;
    R.X1         = Offset.x + static_cast<int32_t>(Extent.width);
    R.Y1         = Offset.y + static_cast<int32_t>(Extent.height);
    R.Z1         = Offset.z + static_cast<int32_t>(Extent.depth);
    return R;
}

Region ImageRange(const VkImageSubresourceRange& Range)
{
    Region R;
    R.MipBase    = Range.baseMipLevel;
    R.MipCount   = Range.levelCount;
    R.LayerBase  = Range.baseArrayLayer;
    R.LayerCount = Range.layerCount;
    return R;
}

CommandRecord* ModelRecord(DeviceData& D, VkCommandBuffer CmdBuf)
{
    auto It = D.Model->Commands.find(CmdBuf);
    return It != D.Model->Commands.end() ? &It->second : nullptr;
}

void ModelAddOp(DeviceData& D, VkCommandBuffer CmdBuf, Op&& NewOp)
{
    CommandRecord* pRec = ModelRecord(D, CmdBuf);
    if (pRec == nullptr)
        return;
    NewOp.DeviceMask = pRec->DeviceMask;
    pRec->Ops.push_back(std::move(NewOp));
}

void ModelAddAttachments(DeviceData& D, VkCommandBuffer CmdBuf, const char* Api, const std::vector<std::pair<uint64_t, AttachmentOps>>& Views, const VkRect2D& Area)
{
    Op O;
    O.Api = Api;
    for (const auto& [View, Ops] : Views)
    {
        auto ViewIt = D.Model->Views.find(View);
        if (ViewIt == D.Model->Views.end())
            continue;
        Region R   = ImageRange(ViewIt->second.Range);
        R.MipCount = 1;
        R.FullRect = false;
        R.X0       = Area.offset.x;
        R.Y0       = Area.offset.y;
        R.Z0       = 0;
        R.X1       = Area.offset.x + static_cast<int32_t>(Area.extent.width);
        R.Y1       = Area.offset.y + static_cast<int32_t>(Area.extent.height);
        R.Z1       = INT32_MAX;
        if (Ops.Load == VK_ATTACHMENT_LOAD_OP_LOAD || Ops.StencilLoad == VK_ATTACHMENT_LOAD_OP_LOAD)
            O.Accesses.push_back(Access{ViewIt->second.Image, true, R, ACCESS_GENERIC_READ});
        O.Accesses.push_back(Access{ViewIt->second.Image, true, R, ACCESS_GENERIC_WRITE});
    }
    if (!O.Accesses.empty())
        ModelAddOp(D, CmdBuf, std::move(O));
}

void ModelSubmit(DeviceData& D, VkCommandBuffer CmdBuf, uint32_t SubmitMask)
{
    if (D.Model)
        Execute(D, CmdBuf, SubmitMask, 0);
}

} // namespace VkSim
