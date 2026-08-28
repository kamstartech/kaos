/*
 * Copyright (C) 2026 The HybridOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * NullComposerHal -- perseus LXC container display-shim
 *
 * Background:
 *   The Halium-style Android container on Xiaomi Mi Mix 3 boots Android's
 *   HAL/userspace (including surfaceflinger) inside Ubuntu Touch to provide
 *   binder services such as camera buffer queues.  The physical display must
 *   remain owned by Lomiri, but surfaceflinger powers on the real panel in
 *   SurfaceFlinger::initializeDisplays() as soon as it connects to the real
 *   composer HAL (frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
 *   around lines 1012-1013 and 5465-5473).
 *
 *   This stub replaces the real vendor composer HAL (HIDL 2.3,
 *   android.hardware.graphics.composer@2.3::IComposer/default) inside the
 *   container.  It reports one fake physical display and accepts the full
 *   HWC2 command surface, but every operation is a no-op.  It never opens
 *   /dev/dri/card0, /dev/dri/renderD128, /dev/kgsl-3d0, or any other DRM
 *   device, and never becomes DRM master.
 *
 *   The implementation is built on top of the existing AOSP composer-hal
 *   utility templates in hardware/interfaces/graphics/composer/2.3/utils/hal/.
 *   Those templates already implement IComposer/IComposerClient, the command
 *   queue parser, and buffer resource tracking; this file only provides the
 *   trivial backend (ComposerHal subclass) that the templates drive.
 */

#include "NullComposerHal.h"

#include <chrono>
#include <thread>
#include <unistd.h>

#include <log/log.h>

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_3 {
namespace hal {

namespace V0 = android::hardware::graphics::common::V1_0;
namespace V1 = android::hardware::graphics::common::V1_1;
namespace V2 = android::hardware::graphics::common::V1_2;

namespace {

constexpr int32_t kInvalidFence = -1;

void closeFence(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

}  // namespace

NullComposerHal::NullComposerHal() = default;

NullComposerHal::~NullComposerHal() {
    std::lock_guard<std::mutex> lock(mVsyncMutex);
    stopVsyncThreadLocked();
}

bool NullComposerHal::hasCapability(hwc2_capability_t /* capability */) {
    return false;
}

std::string NullComposerHal::dumpDebugInfo() {
    return "NullComposerHal (no DRM/display hardware)";
}

void NullComposerHal::registerEventCallback(EventCallback* callback) {
    {
        std::lock_guard<std::mutex> lock(mVsyncMutex);
        mCallback = callback;
    }

    // Report the single fake panel as connected, as required by the HWC2
    // contract before registerEventCallback returns.
    if (callback) {
        callback->onHotplug(kDisplayId, IComposerCallback::Connection::CONNECTED);
    }
}

void NullComposerHal::unregisterEventCallback() {
    std::lock_guard<std::mutex> lock(mVsyncMutex);
    mCallback = nullptr;
    mVsyncEnabled = false;
    stopVsyncThreadLocked();
}

uint32_t NullComposerHal::getMaxVirtualDisplayCount() {
    return 0;
}

Error NullComposerHal::destroyVirtualDisplay(Display /* display */) {
    return Error::NONE;
}

Error NullComposerHal::createLayer(Display display, Layer* outLayer) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (!outLayer) {
        return Error::BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(mLayerMutex);
    Layer layer = mNextLayer++;
    mLayers.insert(layer);
    *outLayer = layer;
    return Error::NONE;
}

Error NullComposerHal::destroyLayer(Display display, Layer layer) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    std::lock_guard<std::mutex> lock(mLayerMutex);
    if (mLayers.erase(layer) == 0) {
        return Error::BAD_LAYER;
    }
    return Error::NONE;
}

Error NullComposerHal::getActiveConfig(Display display, Config* outConfig) {
    if (!isValidDisplay(display) || !outConfig) {
        return Error::BAD_DISPLAY;
    }
    *outConfig = kConfigId;
    return Error::NONE;
}

Error NullComposerHal::getDisplayAttribute(Display display, Config config,
                                           IComposerClient::Attribute attribute,
                                           int32_t* outValue) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (config != kConfigId) {
        return Error::BAD_CONFIG;
    }
    if (!outValue) {
        return Error::BAD_PARAMETER;
    }

    switch (attribute) {
        case IComposerClient::Attribute::WIDTH:
            *outValue = kWidth;
            return Error::NONE;
        case IComposerClient::Attribute::HEIGHT:
            *outValue = kHeight;
            return Error::NONE;
        case IComposerClient::Attribute::VSYNC_PERIOD:
            *outValue = kVsyncPeriodNs;
            return Error::NONE;
        case IComposerClient::Attribute::DPI_X:
            *outValue = kDpiX;
            return Error::NONE;
        case IComposerClient::Attribute::DPI_Y:
            *outValue = kDpiY;
            return Error::NONE;
        default:
            return Error::BAD_PARAMETER;
    }
}

Error NullComposerHal::getDisplayConfigs(Display display, hidl_vec<Config>* outConfigs) {
    if (!isValidDisplay(display) || !outConfigs) {
        return Error::BAD_DISPLAY;
    }
    *outConfigs = hidl_vec<Config>{kConfigId};
    return Error::NONE;
}

Error NullComposerHal::getDisplayName(Display display, hidl_string* outName) {
    if (!isValidDisplay(display) || !outName) {
        return Error::BAD_DISPLAY;
    }
    *outName = "Perseus Null Panel";
    return Error::NONE;
}

Error NullComposerHal::getDisplayType(Display display, IComposerClient::DisplayType* outType) {
    if (!isValidDisplay(display) || !outType) {
        return Error::BAD_DISPLAY;
    }
    *outType = IComposerClient::DisplayType::PHYSICAL;
    return Error::NONE;
}

Error NullComposerHal::getDozeSupport(Display display, bool* outSupport) {
    if (!isValidDisplay(display) || !outSupport) {
        return Error::BAD_DISPLAY;
    }
    *outSupport = false;
    return Error::NONE;
}

Error NullComposerHal::getHdrCapabilities(Display display,
                                          hidl_vec<V0::Hdr>* outTypes,
                                          float* outMaxLuminance,
                                          float* outMaxAverageLuminance,
                                          float* outMinLuminance) {
    if (!isValidDisplay(display) || !outTypes || !outMaxLuminance || !outMaxAverageLuminance ||
        !outMinLuminance) {
        return Error::BAD_DISPLAY;
    }
    *outTypes = hidl_vec<V0::Hdr>{};
    *outMaxLuminance = 0.0f;
    *outMaxAverageLuminance = 0.0f;
    *outMinLuminance = 0.0f;
    return Error::NONE;
}

Error NullComposerHal::setActiveConfig(Display display, Config config) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (config != kConfigId) {
        return Error::BAD_CONFIG;
    }
    return Error::NONE;
}

Error NullComposerHal::setVsyncEnabled(Display display, IComposerClient::Vsync enabled) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }

    std::lock_guard<std::mutex> lock(mVsyncMutex);
    if (enabled == IComposerClient::Vsync::ENABLE) {
        mVsyncEnabled = true;
        startVsyncThreadLocked();
    } else if (enabled == IComposerClient::Vsync::DISABLE) {
        mVsyncEnabled = false;
    }
    mVsyncCv.notify_all();
    return Error::NONE;
}

Error NullComposerHal::setColorTransform(Display /* display */, const float* /* matrix */,
                                         int32_t /* hint */) {
    return Error::NONE;
}

Error NullComposerHal::setClientTarget(Display /* display */, buffer_handle_t /* target */,
                                       int32_t acquireFence, int32_t /* dataspace */,
                                       const std::vector<hwc_rect_t>& /* damage */) {
    closeFence(acquireFence);
    return Error::NONE;
}

Error NullComposerHal::setOutputBuffer(Display /* display */, buffer_handle_t /* buffer */,
                                       int32_t releaseFence) {
    closeFence(releaseFence);
    return Error::NONE;
}

Error NullComposerHal::validateDisplay(Display display, std::vector<Layer>* outChangedLayers,
                                       std::vector<IComposerClient::Composition>* outCompositionTypes,
                                       uint32_t* outDisplayRequestMask,
                                       std::vector<Layer>* outRequestedLayers,
                                       std::vector<uint32_t>* outRequestMasks) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (outChangedLayers) outChangedLayers->clear();
    if (outCompositionTypes) outCompositionTypes->clear();
    if (outDisplayRequestMask) *outDisplayRequestMask = 0;
    if (outRequestedLayers) outRequestedLayers->clear();
    if (outRequestMasks) outRequestMasks->clear();
    return Error::NONE;
}

Error NullComposerHal::acceptDisplayChanges(Display display) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    return Error::NONE;
}

Error NullComposerHal::presentDisplay(Display display, int32_t* outPresentFence,
                                      std::vector<Layer>* outLayers,
                                      std::vector<int32_t>* outReleaseFences) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (outPresentFence) *outPresentFence = kInvalidFence;
    if (outLayers) outLayers->clear();
    if (outReleaseFences) outReleaseFences->clear();
    return Error::NONE;
}

Error NullComposerHal::setLayerCursorPosition(Display display, Layer /* layer */, int32_t /* x */,
                                              int32_t /* y */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerBuffer(Display display, Layer /* layer */,
                                      buffer_handle_t /* buffer */, int32_t acquireFence) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    closeFence(acquireFence);
    return Error::NONE;
}

Error NullComposerHal::setLayerSurfaceDamage(Display display, Layer /* layer */,
                                             const std::vector<hwc_rect_t>& /* damage */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerBlendMode(Display display, Layer /* layer */, int32_t /* mode */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerColor(Display display, Layer /* layer */,
                                     IComposerClient::Color /* color */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerCompositionType(Display display, Layer /* layer */,
                                               int32_t /* type */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerDataspace(Display display, Layer /* layer */,
                                         int32_t /* dataspace */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerDisplayFrame(Display display, Layer /* layer */,
                                            const hwc_rect_t& /* frame */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerPlaneAlpha(Display display, Layer /* layer */, float /* alpha */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerSidebandStream(Display display, Layer /* layer */,
                                              buffer_handle_t /* stream */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerSourceCrop(Display display, Layer /* layer */,
                                          const hwc_frect_t& /* crop */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerTransform(Display display, Layer /* layer */,
                                         int32_t /* transform */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerVisibleRegion(Display display, Layer /* layer */,
                                             const std::vector<hwc_rect_t>& /* visible */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerZOrder(Display display, Layer /* layer */, uint32_t /* z */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getPerFrameMetadataKeys(
        Display display,
        std::vector<V2_2::IComposerClient::PerFrameMetadataKey>* outKeys) {
    if (!isValidDisplay(display) || !outKeys) {
        return Error::BAD_DISPLAY;
    }
    outKeys->clear();
    return Error::NONE;
}

Error NullComposerHal::setLayerPerFrameMetadata(
        Display display, Layer /* layer */,
        const std::vector<V2_2::IComposerClient::PerFrameMetadata>& /* metadata */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getReadbackBufferAttributes(Display display, V1::PixelFormat* outFormat,
                                                   V1::Dataspace* outDataspace) {
    if (!isValidDisplay(display) || !outFormat || !outDataspace) {
        return Error::BAD_DISPLAY;
    }
    *outFormat = V1::PixelFormat::RGBA_8888;
    *outDataspace = V1::Dataspace::UNKNOWN;
    return Error::UNSUPPORTED;
}

Error NullComposerHal::setReadbackBuffer(Display display, const native_handle_t* /* bufferHandle */,
                                         base::unique_fd /* fenceFd */) {
    return isValidDisplay(display) ? Error::UNSUPPORTED : Error::BAD_DISPLAY;
}

Error NullComposerHal::getReadbackBufferFence(Display display, base::unique_fd* outFenceFd) {
    if (!isValidDisplay(display) || !outFenceFd) {
        return Error::BAD_DISPLAY;
    }
    *outFenceFd = base::unique_fd();
    return Error::UNSUPPORTED;
}

Error NullComposerHal::createVirtualDisplay_2_2(uint32_t /* width */, uint32_t /* height */,
                                                V1::PixelFormat* /* format */,
                                                Display* /* outDisplay */) {
    return Error::UNSUPPORTED;
}

Error NullComposerHal::getClientTargetSupport_2_2(Display display, uint32_t width, uint32_t height,
                                                  V1::PixelFormat format, V1::Dataspace dataspace) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (width == static_cast<uint32_t>(kWidth) && height == static_cast<uint32_t>(kHeight) &&
        format == V1::PixelFormat::RGBA_8888 && dataspace == V1::Dataspace::UNKNOWN) {
        return Error::NONE;
    }
    return Error::UNSUPPORTED;
}

Error NullComposerHal::setPowerMode_2_2(Display display, IComposerClient::PowerMode /* mode */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::setLayerFloatColor(Display display, Layer /* layer */,
                                          IComposerClient::FloatColor /* color */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getColorModes_2_2(Display display, hidl_vec<V1::ColorMode>* outModes) {
    if (!isValidDisplay(display) || !outModes) {
        return Error::BAD_DISPLAY;
    }
    *outModes = hidl_vec<V1::ColorMode>{V1::ColorMode::NATIVE};
    return Error::NONE;
}

Error NullComposerHal::getRenderIntents(Display display, V1::ColorMode mode,
                                        std::vector<V1::RenderIntent>* outIntents) {
    if (!isValidDisplay(display) || !outIntents) {
        return Error::BAD_DISPLAY;
    }
    if (mode == V1::ColorMode::NATIVE) {
        *outIntents = {V1::RenderIntent::COLORIMETRIC};
        return Error::NONE;
    }
    return Error::UNSUPPORTED;
}

Error NullComposerHal::setColorMode_2_2(Display display, V1::ColorMode mode,
                                        V1::RenderIntent /* intent */) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    return (mode == V1::ColorMode::NATIVE) ? Error::NONE : Error::UNSUPPORTED;
}

std::array<float, 16> NullComposerHal::getDataspaceSaturationMatrix(V1::Dataspace /* dataspace */) {
    // Identity matrix in column-major order.
    return std::array<float, 16>{
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
}

Error NullComposerHal::getPerFrameMetadataKeys_2_3(
        Display display,
        std::vector<IComposerClient::PerFrameMetadataKey>* outKeys) {
    if (!isValidDisplay(display) || !outKeys) {
        return Error::BAD_DISPLAY;
    }
    outKeys->clear();
    return Error::NONE;
}

Error NullComposerHal::setColorMode_2_3(Display display, V2::ColorMode mode,
                                        V1::RenderIntent /* intent */) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    return (mode == V2::ColorMode::NATIVE) ? Error::NONE : Error::UNSUPPORTED;
}

Error NullComposerHal::getRenderIntents_2_3(Display display, V2::ColorMode mode,
                                            std::vector<V1::RenderIntent>* outIntents) {
    if (!isValidDisplay(display) || !outIntents) {
        return Error::BAD_DISPLAY;
    }
    if (mode == V2::ColorMode::NATIVE) {
        *outIntents = {V1::RenderIntent::COLORIMETRIC};
        return Error::NONE;
    }
    return Error::UNSUPPORTED;
}

Error NullComposerHal::getColorModes_2_3(Display display, hidl_vec<V2::ColorMode>* outModes) {
    if (!isValidDisplay(display) || !outModes) {
        return Error::BAD_DISPLAY;
    }
    *outModes = hidl_vec<V2::ColorMode>{V2::ColorMode::NATIVE};
    return Error::NONE;
}

Error NullComposerHal::getClientTargetSupport_2_3(Display display, uint32_t width, uint32_t height,
                                                  V2::PixelFormat format, V2::Dataspace dataspace) {
    if (!isValidDisplay(display)) {
        return Error::BAD_DISPLAY;
    }
    if (width == static_cast<uint32_t>(kWidth) && height == static_cast<uint32_t>(kHeight) &&
        format == V2::PixelFormat::RGBA_8888 && dataspace == V2::Dataspace::UNKNOWN) {
        return Error::NONE;
    }
    return Error::UNSUPPORTED;
}

Error NullComposerHal::getReadbackBufferAttributes_2_3(Display display, V2::PixelFormat* outFormat,
                                                       V2::Dataspace* outDataspace) {
    if (!isValidDisplay(display) || !outFormat || !outDataspace) {
        return Error::BAD_DISPLAY;
    }
    *outFormat = V2::PixelFormat::RGBA_8888;
    *outDataspace = V2::Dataspace::UNKNOWN;
    return Error::UNSUPPORTED;
}

Error NullComposerHal::getHdrCapabilities_2_3(Display display, hidl_vec<V2::Hdr>* outTypes,
                                              float* outMaxLuminance,
                                              float* outMaxAverageLuminance,
                                              float* outMinLuminance) {
    if (!isValidDisplay(display) || !outTypes || !outMaxLuminance || !outMaxAverageLuminance ||
        !outMinLuminance) {
        return Error::BAD_DISPLAY;
    }
    *outTypes = hidl_vec<V2::Hdr>{};
    *outMaxLuminance = 0.0f;
    *outMaxAverageLuminance = 0.0f;
    *outMinLuminance = 0.0f;
    return Error::NONE;
}

Error NullComposerHal::setLayerPerFrameMetadata_2_3(
        Display display, Layer /* layer */,
        const std::vector<IComposerClient::PerFrameMetadata>& /* metadata */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getDisplayIdentificationData(Display display, uint8_t* outPort,
                                                    std::vector<uint8_t>* outData) {
    if (!isValidDisplay(display) || !outPort || !outData) {
        return Error::BAD_DISPLAY;
    }
    *outPort = 0;
    outData->clear();
    return Error::UNSUPPORTED;
}

Error NullComposerHal::setLayerColorTransform(Display display, Layer /* layer */,
                                              const float* /* matrix */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getDisplayedContentSamplingAttributes(
        uint64_t display, V2::PixelFormat& /* format */, V2::Dataspace& /* dataspace */,
        hidl_bitfield<IComposerClient::FormatColorComponent>& /* componentMask */) {
    return (display == static_cast<uint64_t>(kDisplayId)) ? Error::UNSUPPORTED : Error::BAD_DISPLAY;
}

Error NullComposerHal::setDisplayedContentSamplingEnabled(
        uint64_t display, IComposerClient::DisplayedContentSampling /* enable */,
        hidl_bitfield<IComposerClient::FormatColorComponent> /* componentMask */,
        uint64_t /* maxFrames */) {
    return (display == static_cast<uint64_t>(kDisplayId)) ? Error::UNSUPPORTED : Error::BAD_DISPLAY;
}

Error NullComposerHal::getDisplayedContentSample(uint64_t display, uint64_t /* maxFrames */,
                                                 uint64_t /* timestamp */,
                                                 uint64_t& frameCount,
                                                 hidl_vec<uint64_t>& sampleComponent0,
                                                 hidl_vec<uint64_t>& sampleComponent1,
                                                 hidl_vec<uint64_t>& sampleComponent2,
                                                 hidl_vec<uint64_t>& sampleComponent3) {
    if (display != static_cast<uint64_t>(kDisplayId)) {
        return Error::BAD_DISPLAY;
    }
    frameCount = 0;
    // hidl_vec has resize(), not clear() (system/libhidl/base/include/hidl/HidlSupport.h:510).
    sampleComponent0.resize(0);
    sampleComponent1.resize(0);
    sampleComponent2.resize(0);
    sampleComponent3.resize(0);
    return Error::UNSUPPORTED;
}

Error NullComposerHal::getDisplayCapabilities(
        Display display,
        std::vector<IComposerClient::DisplayCapability>* outCapabilities) {
    if (!isValidDisplay(display) || !outCapabilities) {
        return Error::BAD_DISPLAY;
    }
    outCapabilities->clear();
    return Error::NONE;
}

Error NullComposerHal::setLayerPerFrameMetadataBlobs(
        Display display, Layer /* layer */,
        std::vector<IComposerClient::PerFrameMetadataBlob>& /* blobs */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

Error NullComposerHal::getDisplayBrightnessSupport(Display display, bool* outSupport) {
    if (!isValidDisplay(display) || !outSupport) {
        return Error::BAD_DISPLAY;
    }
    *outSupport = false;
    return Error::NONE;
}

Error NullComposerHal::setDisplayBrightness(Display display, float /* brightness */) {
    return isValidDisplay(display) ? Error::NONE : Error::BAD_DISPLAY;
}

void NullComposerHal::startVsyncThreadLocked() {
    if (mVsyncThread.joinable()) {
        return;
    }
    mStopVsync = false;
    mVsyncThread = std::thread(&NullComposerHal::vsyncLoop, this);
}

void NullComposerHal::stopVsyncThreadLocked() {
    mStopVsync = true;
    mVsyncCv.notify_all();
    if (mVsyncThread.joinable()) {
        mVsyncThread.join();
    }
}

void NullComposerHal::vsyncLoop() {
    using namespace std::chrono_literals;

    std::unique_lock<std::mutex> lock(mVsyncMutex);
    while (!mStopVsync) {
        mVsyncCv.wait(lock, [this] { return mStopVsync || mVsyncEnabled; });
        if (mStopVsync) {
            break;
        }
        if (!mVsyncEnabled) {
            continue;
        }

        // (Re-)start the phase from the current time so a disable/enable
        // cycle does not produce a burst of catch-up callbacks.
        auto nextVsync = std::chrono::steady_clock::now() + std::chrono::nanoseconds(kVsyncPeriodNs);

        while (mVsyncEnabled && !mStopVsync) {
            lock.unlock();
            std::this_thread::sleep_until(nextVsync);
            nextVsync += std::chrono::nanoseconds(kVsyncPeriodNs);

            EventCallback* callback = nullptr;
            {
                std::lock_guard<std::mutex> cbLock(mVsyncMutex);
                callback = mCallback;
            }
            if (callback) {
                auto now = std::chrono::steady_clock::now().time_since_epoch();
                int64_t timestamp =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
                callback->onVsync(kDisplayId, timestamp);
            }

            lock.lock();
        }
    }
}

}  // namespace hal
}  // namespace V2_3
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android
