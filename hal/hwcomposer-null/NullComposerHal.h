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

#pragma once

#define LOG_TAG "ComposerNull"

#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <android-base/unique_fd.h>
#include <android/hardware/graphics/composer/2.3/IComposerClient.h>
#include <composer-hal/2.3/ComposerHal.h>
// IComposerCallback is defined only at composer 2.1 (2.3 never re-versions
// it, confirmed: hardware/interfaces/graphics/composer/2.3 has no
// IComposerCallback.hal, only 2.1 and 2.4 do). composer-hal/2.3/ComposerHal.h
// (via its 2.1 base class) uses IComposerCallback::Connection unqualified
// without its own explicit include -- it's pulled in transitively through
// IComposer.h's own generated code, matching real AOSP's own ComposerHal.h,
// which likewise never includes IComposerCallback.h directly.

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_3 {
namespace hal {

// composer-hal/2.3/ComposerHal.h (and 2.2's) already "using V2_1::Display;
// using V2_1::Error; using V2_1::Layer;" at this same namespace scope, which
// this file inherits since it reopens the same namespace -- but neither
// brings in V2_1::Config or V2_1::IComposerCallback (Config is a plain
// "typedef uint32_t Config;" in .../2.1/types.h; IComposerCallback is only
// ever versioned at 2.1 and 2.4, never 2.3), since neither is referenced
// unqualified at their own level. We reference both, so they need their own
// using declarations here.
using V2_1::Config;
using V2_1::IComposerCallback;

// A no-op composer HAL backend that reports one fake physical display and
// accepts every command without ever touching real display/DRM hardware.
// It is used inside the Halium LXC container on Xiaomi Mi Mix 3 (perseus)
// to keep surfaceflinger alive without allowing it to take over the physical
// panel.  See the implementation file header for the full rationale.
class NullComposerHal : public ComposerHal {
public:
    NullComposerHal();
    ~NullComposerHal() override;

    // V2.1 pure virtuals that are NOT superseded by V2.2/V2.3 shims.
    bool hasCapability(hwc2_capability_t capability) override;
    std::string dumpDebugInfo() override;
    void registerEventCallback(EventCallback* callback) override;
    void unregisterEventCallback() override;

    uint32_t getMaxVirtualDisplayCount() override;
    Error destroyVirtualDisplay(Display display) override;
    Error createLayer(Display display, Layer* outLayer) override;
    Error destroyLayer(Display display, Layer layer) override;

    Error getActiveConfig(Display display, Config* outConfig) override;
    Error getDisplayAttribute(Display display, Config config,
                              IComposerClient::Attribute attribute, int32_t* outValue) override;
    Error getDisplayConfigs(Display display, hidl_vec<Config>* outConfigs) override;
    Error getDisplayName(Display display, hidl_string* outName) override;
    Error getDisplayType(Display display, IComposerClient::DisplayType* outType) override;
    Error getDozeSupport(Display display, bool* outSupport) override;
    Error getHdrCapabilities(Display display, hidl_vec<common::V1_0::Hdr>* outTypes,
                             float* outMaxLuminance, float* outMaxAverageLuminance,
                             float* outMinLuminance) override;

    Error setActiveConfig(Display display, Config config) override;
    Error setVsyncEnabled(Display display, IComposerClient::Vsync enabled) override;

    Error setColorTransform(Display display, const float* matrix, int32_t hint) override;
    Error setClientTarget(Display display, buffer_handle_t target, int32_t acquireFence,
                          int32_t dataspace, const std::vector<hwc_rect_t>& damage) override;
    Error setOutputBuffer(Display display, buffer_handle_t buffer, int32_t releaseFence) override;
    Error validateDisplay(Display display, std::vector<Layer>* outChangedLayers,
                          std::vector<IComposerClient::Composition>* outCompositionTypes,
                          uint32_t* outDisplayRequestMask,
                          std::vector<Layer>* outRequestedLayers,
                          std::vector<uint32_t>* outRequestMasks) override;
    Error acceptDisplayChanges(Display display) override;
    Error presentDisplay(Display display, int32_t* outPresentFence,
                         std::vector<Layer>* outLayers,
                         std::vector<int32_t>* outReleaseFences) override;

    Error setLayerCursorPosition(Display display, Layer layer, int32_t x, int32_t y) override;
    Error setLayerBuffer(Display display, Layer layer, buffer_handle_t buffer,
                         int32_t acquireFence) override;
    Error setLayerSurfaceDamage(Display display, Layer layer,
                                const std::vector<hwc_rect_t>& damage) override;
    Error setLayerBlendMode(Display display, Layer layer, int32_t mode) override;
    Error setLayerColor(Display display, Layer layer, IComposerClient::Color color) override;
    Error setLayerCompositionType(Display display, Layer layer, int32_t type) override;
    Error setLayerDataspace(Display display, Layer layer, int32_t dataspace) override;
    Error setLayerDisplayFrame(Display display, Layer layer, const hwc_rect_t& frame) override;
    Error setLayerPlaneAlpha(Display display, Layer layer, float alpha) override;
    Error setLayerSidebandStream(Display display, Layer layer, buffer_handle_t stream) override;
    Error setLayerSourceCrop(Display display, Layer layer, const hwc_frect_t& crop) override;
    Error setLayerTransform(Display display, Layer layer, int32_t transform) override;
    Error setLayerVisibleRegion(Display display, Layer layer,
                                const std::vector<hwc_rect_t>& visible) override;
    Error setLayerZOrder(Display display, Layer layer, uint32_t z) override;

    // V2.2 pure virtuals.
    Error getPerFrameMetadataKeys(
            Display display,
            std::vector<V2_2::IComposerClient::PerFrameMetadataKey>* outKeys) override;
    Error setLayerPerFrameMetadata(
            Display display, Layer layer,
            const std::vector<V2_2::IComposerClient::PerFrameMetadata>& metadata) override;
    Error getReadbackBufferAttributes(Display display, common::V1_1::PixelFormat* outFormat,
                                      common::V1_1::Dataspace* outDataspace) override;
    Error setReadbackBuffer(Display display, const native_handle_t* bufferHandle,
                            base::unique_fd fenceFd) override;
    Error getReadbackBufferFence(Display display, base::unique_fd* outFenceFd) override;
    Error createVirtualDisplay_2_2(uint32_t width, uint32_t height,
                                   common::V1_1::PixelFormat* format,
                                   Display* outDisplay) override;
    Error getClientTargetSupport_2_2(Display display, uint32_t width, uint32_t height,
                                     common::V1_1::PixelFormat format,
                                     common::V1_1::Dataspace dataspace) override;
    Error setPowerMode_2_2(Display display, IComposerClient::PowerMode mode) override;
    Error setLayerFloatColor(Display display, Layer layer,
                             IComposerClient::FloatColor color) override;
    Error getColorModes_2_2(Display display,
                            hidl_vec<common::V1_1::ColorMode>* outModes) override;
    Error getRenderIntents(Display display, common::V1_1::ColorMode mode,
                           std::vector<common::V1_1::RenderIntent>* outIntents) override;
    Error setColorMode_2_2(Display display, common::V1_1::ColorMode mode,
                           common::V1_1::RenderIntent intent) override;
    std::array<float, 16> getDataspaceSaturationMatrix(common::V1_1::Dataspace dataspace) override;

    // V2.3 pure virtuals.
    Error getPerFrameMetadataKeys_2_3(
            Display display,
            std::vector<IComposerClient::PerFrameMetadataKey>* outKeys) override;
    Error setColorMode_2_3(Display display, common::V1_2::ColorMode mode,
                           common::V1_1::RenderIntent intent) override;
    Error getRenderIntents_2_3(Display display, common::V1_2::ColorMode mode,
                               std::vector<common::V1_1::RenderIntent>* outIntents) override;
    Error getColorModes_2_3(Display display,
                            hidl_vec<common::V1_2::ColorMode>* outModes) override;
    Error getClientTargetSupport_2_3(Display display, uint32_t width, uint32_t height,
                                     common::V1_2::PixelFormat format,
                                     common::V1_2::Dataspace dataspace) override;
    Error getReadbackBufferAttributes_2_3(Display display, common::V1_2::PixelFormat* outFormat,
                                          common::V1_2::Dataspace* outDataspace) override;
    Error getHdrCapabilities_2_3(Display display, hidl_vec<common::V1_2::Hdr>* outTypes,
                                 float* outMaxLuminance, float* outMaxAverageLuminance,
                                 float* outMinLuminance) override;
    Error setLayerPerFrameMetadata_2_3(
            Display display, Layer layer,
            const std::vector<IComposerClient::PerFrameMetadata>& metadata) override;
    Error getDisplayIdentificationData(Display display, uint8_t* outPort,
                                       std::vector<uint8_t>* outData) override;
    Error setLayerColorTransform(Display display, Layer layer, const float* matrix) override;
    Error getDisplayedContentSamplingAttributes(
            uint64_t display, common::V1_2::PixelFormat& format,
            common::V1_2::Dataspace& dataspace,
            hidl_bitfield<IComposerClient::FormatColorComponent>& componentMask) override;
    Error setDisplayedContentSamplingEnabled(
            uint64_t display, IComposerClient::DisplayedContentSampling enable,
            hidl_bitfield<IComposerClient::FormatColorComponent> componentMask,
            uint64_t maxFrames) override;
    Error getDisplayedContentSample(uint64_t display, uint64_t maxFrames, uint64_t timestamp,
                                    uint64_t& frameCount, hidl_vec<uint64_t>& sampleComponent0,
                                    hidl_vec<uint64_t>& sampleComponent1,
                                    hidl_vec<uint64_t>& sampleComponent2,
                                    hidl_vec<uint64_t>& sampleComponent3) override;
    Error getDisplayCapabilities(
            Display display,
            std::vector<IComposerClient::DisplayCapability>* outCapabilities) override;
    Error setLayerPerFrameMetadataBlobs(
            Display display, Layer layer,
            std::vector<IComposerClient::PerFrameMetadataBlob>& blobs) override;
    Error getDisplayBrightnessSupport(Display display, bool* outSupport) override;
    Error setDisplayBrightness(Display display, float brightness) override;

private:
    // Panel parameters.  Defaults match the Mi Mix 3 Samsung AMOLED panel
    // (1080x2340 @ ~60 Hz, 403 ppi).  They are exposed as preprocessor
    // defines so a future second device can override them via cflags in
    // its device tree without forking this generic module.
#ifndef KAOS_HWC_NULL_WIDTH
#define KAOS_HWC_NULL_WIDTH 1080
#endif
#ifndef KAOS_HWC_NULL_HEIGHT
#define KAOS_HWC_NULL_HEIGHT 2340
#endif
#ifndef KAOS_HWC_NULL_VSYNC_NS
#define KAOS_HWC_NULL_VSYNC_NS 16666666  // ~60 Hz
#endif
#ifndef KAOS_HWC_NULL_DPI
#define KAOS_HWC_NULL_DPI 403000  // 403 ppi * 1000
#endif

    static constexpr Display kDisplayId = 0;
    static constexpr Config kConfigId = 0;
    static constexpr int32_t kWidth = KAOS_HWC_NULL_WIDTH;
    static constexpr int32_t kHeight = KAOS_HWC_NULL_HEIGHT;
    static constexpr int32_t kVsyncPeriodNs = KAOS_HWC_NULL_VSYNC_NS;
    static constexpr int32_t kDpiX = KAOS_HWC_NULL_DPI;
    static constexpr int32_t kDpiY = KAOS_HWC_NULL_DPI;

    bool isValidDisplay(Display display) const { return display == kDisplayId; }

    void startVsyncThreadLocked();
    void stopVsyncThreadLocked();
    void vsyncLoop();

    EventCallback* mCallback = nullptr;

    std::mutex mVsyncMutex;
    bool mVsyncEnabled = false;
    bool mStopVsync = false;
    std::condition_variable mVsyncCv;
    std::thread mVsyncThread;

    std::mutex mLayerMutex;
    Layer mNextLayer = 1;
    std::unordered_set<Layer> mLayers;
};

}  // namespace hal
}  // namespace V2_3
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android
