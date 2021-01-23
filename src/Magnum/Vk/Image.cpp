/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021 Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "Image.h"
#include "ImageCreateInfo.h"

#include <Corrade/Containers/EnumSet.hpp>

#include "Magnum/Vk/Assert.h"
#include "Magnum/Vk/Device.h"
#include "Magnum/Vk/DeviceProperties.h"
#include "Magnum/Vk/Handle.h"
#include "Magnum/Vk/Integration.h"
#include "Magnum/Vk/MemoryAllocateInfo.h"
#include "Magnum/Vk/PixelFormat.h"
#include "Magnum/Vk/Implementation/DeviceState.h"

namespace Magnum { namespace Vk {

ImageCreateInfo::ImageCreateInfo(const VkImageType type, const ImageUsages usages, const PixelFormat format, const Vector3i& size, const Int layers, const Int levels, const Int samples, const ImageLayout initialLayout, const Flags flags): _info{} {
    _info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    _info.flags = VkImageCreateFlags(flags);
    _info.imageType = type;
    _info.format = VkFormat(format);
    _info.extent = VkExtent3D(size);
    _info.mipLevels = levels;
    _info.arrayLayers = layers;
    _info.samples = VkSampleCountFlagBits(samples);
    _info.tiling = VK_IMAGE_TILING_OPTIMAL;
    _info.usage = VkImageUsageFlags(usages);
    /* _info.sharingMode is implicitly VK_SHARING_MODE_EXCLUSIVE;
       _info.queueFamilyIndexCount and _info.pQueueFamilyIndices should be
       filled only for VK_SHARING_MODE_CONCURRENT */
    _info.initialLayout = VkImageLayout(initialLayout);
}

ImageCreateInfo::ImageCreateInfo(const VkImageType type, const ImageUsages usages, const Magnum::PixelFormat format, const Vector3i& size, const Int layers, const Int levels, const Int samples, const ImageLayout initialLayout, const Flags flags): ImageCreateInfo{type, usages, pixelFormat(format), size, layers, levels, samples, initialLayout, flags} {}

ImageCreateInfo::ImageCreateInfo(const VkImageType type, const ImageUsages usages, const Magnum::CompressedPixelFormat format, const Vector3i& size, const Int layers, const Int levels, const Int samples, const ImageLayout initialLayout, const Flags flags): ImageCreateInfo{type, usages, pixelFormat(format), size, layers, levels, samples, initialLayout, flags} {}

ImageCreateInfo::ImageCreateInfo(NoInitT) noexcept {}

ImageCreateInfo::ImageCreateInfo(const VkImageCreateInfo& info):
    /* Can't use {} with GCC 4.8 here because it tries to initialize the first
       member instead of doing a copy */
    _info(info) {}

Debug& operator<<(Debug& debug, const ImageAspect value) {
    debug << "Vk::ImageAspect" << Debug::nospace;

    switch(value) {
        /* LCOV_EXCL_START */
        #define _c(value) case Vk::ImageAspect::value: return debug << "::" << Debug::nospace << #value;
        _c(Color)
        _c(Depth)
        _c(Stencil)
        #undef _c
        /* LCOV_EXCL_STOP */
    }

    /* Flag bits should be in hex, unlike plain values */
    return debug << "(" << Debug::nospace << reinterpret_cast<void*>(UnsignedInt(value)) << Debug::nospace << ")";
}

Debug& operator<<(Debug& debug, const ImageAspects value) {
    return Containers::enumSetDebugOutput(debug, value, "Vk::ImageAspects{}", {
        Vk::ImageAspect::Color,
        Vk::ImageAspect::Depth,
        Vk::ImageAspect::Stencil});
}

/* Vulkan, it would kill you if 0 was a valid default, right?! ffs */
ImageAspects imageAspectsFor(const PixelFormat format) {
    /** @todo expand somehow to catch any invalid values? */
    CORRADE_ASSERT(Int(format), "Vk::imageAspectsFor(): can't get an aspect for" << format, {});

    if(format == PixelFormat::Depth16UnormStencil8UI ||
       format == PixelFormat::Depth24UnormStencil8UI ||
       format == PixelFormat::Depth32FStencil8UI)
        return ImageAspect::Depth|ImageAspect::Stencil;
    if(format == PixelFormat::Depth16Unorm ||
       format == PixelFormat::Depth24Unorm ||
       format == PixelFormat::Depth32F)
        return ImageAspect::Depth;
    if(format == PixelFormat::Stencil8UI)
        return ImageAspect::Stencil;

    /** @todo planar formats */

    return ImageAspect::Color;
}

ImageAspects imageAspectsFor(const Magnum::PixelFormat format) {
    return imageAspectsFor(pixelFormat(format));
}

Image Image::wrap(Device& device, const VkImage handle, const PixelFormat format, const HandleFlags flags) {
    Image out{NoCreate};
    out._device = &device;
    out._handle = handle;
    out._flags = flags;
    out._format = format;
    return out;
}

Image Image::wrap(Device& device, const VkImage handle, const Magnum::PixelFormat format, const HandleFlags flags) {
    return wrap(device, handle, pixelFormat(format), flags);
}

Image Image::wrap(Device& device, const VkImage handle, const Magnum::CompressedPixelFormat format, const HandleFlags flags) {
    return wrap(device, handle, pixelFormat(format), flags);
}

Image::Image(Device& device, const ImageCreateInfo& info, NoAllocateT): _device{&device}, _flags{HandleFlag::DestroyOnDestruction}, _format{PixelFormat(info->format)}, _dedicatedMemory{NoCreate} {
    MAGNUM_VK_INTERNAL_ASSERT_SUCCESS(device->CreateImage(device, info, nullptr, &_handle));
}

Image::Image(Device& device, const ImageCreateInfo& info, const MemoryFlags memoryFlags): Image{device, info, NoAllocate} {
    const MemoryRequirements requirements = memoryRequirements();
    bindDedicatedMemory(Memory{device, MemoryAllocateInfo{
        requirements.size(),
        device.properties().pickMemory(memoryFlags, requirements.memories())
    }});
}

Image::Image(NoCreateT): _device{}, _handle{}, _format{}, _dedicatedMemory{NoCreate} {}

Image::Image(Image&& other) noexcept: _device{other._device}, _handle{other._handle}, _flags{other._flags}, _format{other._format}, _dedicatedMemory{std::move(other._dedicatedMemory)} {
    other._handle = {};
}

Image::~Image() {
    if(_handle && (_flags & HandleFlag::DestroyOnDestruction))
        (**_device).DestroyImage(*_device, _handle, nullptr);
}

Image& Image::operator=(Image&& other) noexcept {
    using std::swap;
    swap(other._device, _device);
    swap(other._handle, _handle);
    swap(other._flags, _flags);
    swap(other._format, _format);
    swap(other._dedicatedMemory, _dedicatedMemory);
    return *this;
}

MemoryRequirements Image::memoryRequirements() const {
    MemoryRequirements requirements;
    VkImageMemoryRequirementsInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    info.image = _handle;
    _device->state().getImageMemoryRequirementsImplementation(*_device, info, requirements);
    return requirements;
}

void Image::bindMemory(Memory& memory, const UnsignedLong offset) {
    VkBindImageMemoryInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    info.image = _handle;
    info.memory = memory;
    info.memoryOffset = offset;
    MAGNUM_VK_INTERNAL_ASSERT_SUCCESS(_device->state().bindImageMemoryImplementation(*_device, 1, &info));
}

void Image::bindDedicatedMemory(Memory&& memory) {
    bindMemory(memory, 0);
    _dedicatedMemory = std::move(memory);
}

bool Image::hasDedicatedMemory() const {
    /* Sigh. Though better than needing to have `const handle()` overloads
       returning `const VkDeviceMemory_T*` */
    return const_cast<Image&>(*this)._dedicatedMemory.handle();
}

Memory& Image::dedicatedMemory() {
    CORRADE_ASSERT(_dedicatedMemory.handle(),
        "Vk::Image::dedicatedMemory(): image doesn't have a dedicated memory", _dedicatedMemory);
    return _dedicatedMemory;
}

VkImage Image::release() {
    const VkImage handle = _handle;
    _handle = {};
    return handle;
}

void Image::getMemoryRequirementsImplementationDefault(Device& device, const VkImageMemoryRequirementsInfo2& info, VkMemoryRequirements2& requirements) {
    return device->GetImageMemoryRequirements(device, info.image, &requirements.memoryRequirements);
}

void Image::getMemoryRequirementsImplementationKHR(Device& device, const VkImageMemoryRequirementsInfo2& info, VkMemoryRequirements2& requirements) {
    return device->GetImageMemoryRequirements2KHR(device, &info, &requirements);
}

void Image::getMemoryRequirementsImplementation11(Device& device, const VkImageMemoryRequirementsInfo2& info, VkMemoryRequirements2& requirements) {
    return device->GetImageMemoryRequirements2(device, &info, &requirements);
}

VkResult Image::bindMemoryImplementationDefault(Device& device, UnsignedInt count, const VkBindImageMemoryInfo* const infos) {
    for(std::size_t i = 0; i != count; ++i)
        if(VkResult result = device->BindImageMemory(device, infos[i].image, infos[i].memory, infos[i].memoryOffset)) return result;
    return VK_SUCCESS;
}

VkResult Image::bindMemoryImplementationKHR(Device& device, UnsignedInt count, const VkBindImageMemoryInfo* const infos) {
    return device->BindImageMemory2KHR(device, count, infos);
}

VkResult Image::bindMemoryImplementation11(Device& device, UnsignedInt count, const VkBindImageMemoryInfo* const infos) {
    return device->BindImageMemory2(device, count, infos);
}

}}
