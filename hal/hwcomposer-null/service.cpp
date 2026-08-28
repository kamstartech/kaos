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
 * android.hardware.graphics.composer@2.3-service-null
 *
 * This service replaces the real vendor composer HAL inside the Halium LXC
 * container on Xiaomi Mi Mix 3 (perseus).  It registers as the exact same
 * HIDL service the vendor HAL provides:
 *
 *     android.hardware.graphics.composer@2.3::IComposer/default
 *
 * so surfaceflinger uses it transparently.  The backend (NullComposerHal)
 * reports one fake physical display and accepts all HWC2 commands as no-ops,
 * so the container's surfaceflinger never powers on or writes to the real
 * panel.  The physical display remains owned by Lomiri.
 *
 * This file deliberately never opens /dev/dri/card0, /dev/dri/renderD128,
 * /dev/kgsl-3d0, or any other display/DRM device.  See NullComposerHal.cpp
 * for the full rationale.
 */

#include <sched.h>

#include <android/hardware/graphics/composer/2.3/IComposer.h>
#include <binder/ProcessState.h>
#include <composer-hal/2.3/Composer.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include "NullComposerHal.h"

using android::hardware::graphics::composer::V2_3::IComposer;
using android::hardware::graphics::composer::V2_3::hal::Composer;
using android::hardware::graphics::composer::V2_3::hal::NullComposerHal;

int main() {
    // The conventional HAL might start binder services.
    android::ProcessState::initWithDriver("/dev/vndbinder");
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();

    // Same FIFO priority as the stock composer service and surfaceflinger.
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO: %d", errno);
    }

    android::hardware::configureRpcThreadpool(4, true /* will join */);

    auto hal = std::make_unique<NullComposerHal>();
    auto composer = Composer::create(std::move(hal));
    if (composer == nullptr) {
        ALOGE("failed to create null composer");
        return 1;
    }

    if (composer->registerAsService() != android::NO_ERROR) {
        ALOGE("failed to register null composer service");
        return 1;
    }

    android::hardware::joinRpcThreadpool();

    ALOGE("null composer service is terminating");
    return 1;
}
