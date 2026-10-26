#include "render_image_store.hpp"
#include <cstring>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
constexpr uint32_t kMaxWidth = 16384;
constexpr uint32_t kMaxHeight = 16384;
constexpr uint64_t kMaxImageBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxClientBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;
constexpr uint32_t kMaxImages = 4096;
constexpr uint32_t kMaxMappings = 1024;

bool SupportedFormat(uint32_t format) noexcept {
    return format == MMS_IMAGE_RGBA8 || format == MMS_IMAGE_BGRA8;
}
bool SupportedColorSpace(uint32_t colorSpace) noexcept {
    return colorSpace == MMS_IMAGE_COLOR_SRGB;
}
bool SupportedAlpha(uint32_t alphaMode) noexcept {
    return alphaMode == MMS_IMAGE_ALPHA_OPAQUE || alphaMode == MMS_IMAGE_ALPHA_STRAIGHT ||
        alphaMode == MMS_IMAGE_ALPHA_PREMULTIPLIED;
}
// Bytes the caller's span must cover: (height - 1) * rowStride + rowBytes. False
// on 64-bit overflow.
bool SpanBytes(uint32_t height, uint64_t rowStride, uint64_t rowBytes, uint64_t& out) noexcept {
    const uint64_t rows = static_cast<uint64_t>(height - 1);
    if (rowStride && rows > (UINT64_MAX - rowBytes) / rowStride) return false;
    const uint64_t head = rows * rowStride;
    if (head > UINT64_MAX - rowBytes) return false;
    out = head + rowBytes;
    return true;
}
}

struct RenderImageStore::Payload {
    uint32_t width = 0, height = 0, format = 0, colorSpace = 0, alphaMode = 0;
    uint64_t rowStride = 0, byteCount = 0;
    std::vector<unsigned char> pixels;
};

RenderImageStore::~RenderImageStore() = default;

MmsRenderImageApiV1 RenderImageStore::Api(Client& client) {
    // The caller initializes Client::owner before the table is shared. Writing it
    // here would race with worker calls on a later queryService round, so Api only
    // builds the table and leaves all Client state to the caller.
    MmsRenderImageApiV1 api{Header<MmsRenderImageApiV1>(), &client};
    api.getCapabilities = [](void* context, MmsRenderImageCapsV1* out) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Capabilities(client, out);
        });
    };
    api.createCpu = [](void* context, const MmsRenderImageDescV1* desc, MmsBytes pixels, MmsHandle* out) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Create(client, desc, pixels, out);
        });
    };
    api.describe = [](void* context, MmsHandle image, MmsRenderImageInfoV1* out) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Describe(client, image, out);
        });
    };
    api.retain = [](void* context, MmsHandle image, MmsHandle* out) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Retain(client, image, out);
        });
    };
    api.mapRead = [](void* context, MmsHandle image, MmsRenderImageMapV1* out) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->MapRead(client, image, out);
        });
    };
    api.unmap = [](void* context, MmsHandle mapping) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Unmap(client, mapping);
        });
    };
    api.release = [](void* context, MmsHandle image) {
        return Boundary([&] {
            auto& client = *static_cast<Client*>(context);
            return client.owner->Release(client, image);
        });
    };
    return api;
}

void RenderImageStore::Retire(Client& client) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    client.retired = true;
    for (auto it = images_.begin(); it != images_.end();) {
        if (it->second.owner != &client) {
            ++it;
            continue;
        }
        // Keep the payload alive across the erase so a mapping can outlive the
        // image handles. Erasing only drops our reference; erase itself does not
        // allocate.
        const auto payload = it->second.payload;
        it = images_.erase(it);
        Settle(payload, client);
    }
    client.images = 0;
}

bool RenderImageStore::HasLeases(Client& client) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : mappings_)
        if (entry.second.owner == &client) return true;
    return false;
}

MmsResult RenderImageStore::Capabilities(Client& client, MmsRenderImageCapsV1* out) const {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    MmsRenderImageCapsV1 caps{};
    caps.h = Header<MmsRenderImageCapsV1>();
    caps.flags = MMS_IMAGE_CAP_CPU_COPY;
    caps.maxWidth = kMaxWidth;
    caps.maxHeight = kMaxHeight;
    caps.reserved = 0;
    caps.maxImageBytes = kMaxImageBytes;
    caps.maxClientBytes = kMaxClientBytes;
    caps.maxTotalBytes = kMaxTotalBytes;
    caps.maxImages = kMaxImages;
    caps.maxMappings = kMaxMappings;
    *out = caps;
    return MMS_OK;
}

MmsResult RenderImageStore::Create(Client& client, const MmsRenderImageDescV1* desc, MmsBytes pixels, MmsHandle* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    if (!desc || !Valid(desc)) return MMS_E_ARGUMENT;
    const uint32_t width = desc->width, height = desc->height;
    if (!width || !height) return MMS_E_ARGUMENT;
    // Capability limits are checked before the span so a rejected but otherwise
    // reasonable description never depends on caller-provided buffer arithmetic.
    if (!SupportedFormat(desc->format) || !SupportedColorSpace(desc->colorSpace) ||
        !SupportedAlpha(desc->alphaMode)) {
        return MMS_E_UNSUPPORTED;
    }
    if (desc->flags) return MMS_E_ARGUMENT;
    if (width > kMaxWidth || height > kMaxHeight) return MMS_E_MEMORY;
    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4u;
    const uint64_t byteCount = rowBytes * height;
    if (byteCount > kMaxImageBytes) return MMS_E_MEMORY;
    if (desc->rowStride < rowBytes) return MMS_E_ARGUMENT;
    uint64_t required = 0;
    if (!SpanBytes(height, desc->rowStride, rowBytes, required)) return MMS_E_ARGUMENT;
    if (!pixels.data) return MMS_E_ARGUMENT;
    // The span check only proves the count; the last byte must also be
    // addressable. A forged huge stride/span could otherwise wrap the pointer
    // arithmetic below. Single-row images legitimately pass any rowStride
    // because their required extent stays at width * 4.
    const uintptr_t base = reinterpret_cast<uintptr_t>(pixels.data);
    if (required && base > UINTPTR_MAX - static_cast<uintptr_t>(required - 1)) return MMS_E_ARGUMENT;
    if (pixels.bytes < required) return MMS_E_ARGUMENT;
    if (client.images >= kMaxImages) return MMS_E_MEMORY;
    if (byteCount > kMaxClientBytes || client.bytes > kMaxClientBytes - byteCount) return MMS_E_MEMORY;
    if (byteCount > kMaxTotalBytes || totalBytes_ > kMaxTotalBytes - byteCount) return MMS_E_MEMORY;
    if (!next_) return MMS_E_MEMORY;
    const MmsHandle handle = next_++;
    // Copy only the effective pixels of each row; input padding is never read.
    // A failure here publishes nothing and charges nothing.
    auto payload = std::make_shared<Payload>();
    payload->width = width;
    payload->height = height;
    payload->format = desc->format;
    payload->colorSpace = desc->colorSpace;
    payload->alphaMode = desc->alphaMode;
    payload->rowStride = rowBytes;
    payload->byteCount = byteCount;
    payload->pixels.resize(static_cast<size_t>(byteCount));
    const auto* source = static_cast<const unsigned char*>(pixels.data);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(payload->pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rowBytes),
            source + static_cast<size_t>(y) * static_cast<size_t>(desc->rowStride), static_cast<size_t>(rowBytes));
    }
    images_.emplace(handle, Entry{&client, payload});
    ++client.images;
    client.bytes += byteCount;
    totalBytes_ += byteCount;
    *out = handle;
    return MMS_OK;
}

MmsResult RenderImageStore::Describe(Client& client, MmsHandle image, MmsRenderImageInfoV1* out) const {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = images_.find(image);
    if (found == images_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.payload;
    MmsRenderImageInfoV1 info{};
    info.h = Header<MmsRenderImageInfoV1>();
    info.width = payload.width;
    info.height = payload.height;
    info.format = payload.format;
    info.colorSpace = payload.colorSpace;
    info.alphaMode = payload.alphaMode;
    info.flags = 0;
    info.rowStride = payload.rowStride;
    info.byteCount = payload.byteCount;
    info.deviceGeneration = 0;
    *out = info;
    return MMS_OK;
}

MmsResult RenderImageStore::Retain(Client& client, MmsHandle image, MmsHandle* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = images_.find(image);
    if (found == images_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    if (client.images >= kMaxImages) return MMS_E_MEMORY;
    if (!next_) return MMS_E_MEMORY;
    const auto payload = found->second.payload;
    const MmsHandle handle = next_++;
    images_.emplace(handle, Entry{&client, payload});
    ++client.images;
    *out = handle;
    return MMS_OK;
}

MmsResult RenderImageStore::MapRead(Client& client, MmsHandle image, MmsRenderImageMapV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = images_.find(image);
    if (found == images_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    if (client.mappings >= kMaxMappings) return MMS_E_MEMORY;
    if (!next_) return MMS_E_MEMORY;
    const auto payload = found->second.payload;
    const MmsHandle handle = next_++;
    mappings_.emplace(handle, Entry{&client, payload});
    ++client.mappings;
    MmsRenderImageMapV1 map{};
    map.h = Header<MmsRenderImageMapV1>();
    map.mapping = handle;
    map.pixels = payload->pixels.data();
    map.byteCount = payload->byteCount;
    map.rowStride = payload->rowStride;
    *out = map;
    return MMS_OK;
}

MmsResult RenderImageStore::Unmap(Client& client, MmsHandle mapping) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = mappings_.find(mapping);
    if (found == mappings_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto payload = found->second.payload;
    mappings_.erase(found);
    if (client.mappings) --client.mappings;
    Settle(payload, client);
    return MMS_OK;
}

MmsResult RenderImageStore::Release(Client& client, MmsHandle image) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = images_.find(image);
    if (found == images_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto payload = found->second.payload;
    images_.erase(found);
    if (client.images) --client.images;
    Settle(payload, client);
    return MMS_OK;
}

void RenderImageStore::Settle(const std::shared_ptr<const Payload>& payload, Client& client) noexcept {
    // The entity that settled was erased, so a single remaining reference means
    // no image handle and no mapping still uses the pixels.
    if (payload.use_count() != 1) return;
    const uint64_t bytes = payload->byteCount;
    client.bytes = client.bytes >= bytes ? client.bytes - bytes : 0;
    totalBytes_ = totalBytes_ >= bytes ? totalBytes_ - bytes : 0;
}
}
