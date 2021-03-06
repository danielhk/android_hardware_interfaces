/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "camera_hidl_hal_test"
#include <VtsHalHidlTargetTestBase.h>
#include <android/hardware/camera/device/1.0/ICameraDevice.h>
#include <android/hardware/camera/device/3.2/ICameraDevice.h>
#include <android/hardware/camera/provider/2.4/ICameraProvider.h>
#include <android/log.h>
#include <binder/MemoryHeapBase.h>
#include <grallocusage/GrallocUsageConversion.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <inttypes.h>
#include <system/camera.h>
#include <ui/GraphicBuffer.h>
#include <utils/Errors.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <unordered_map>
#include "CameraParameters.h"
#include "system/camera_metadata.h"

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::sp;
using ::android::wp;
using ::android::GraphicBuffer;
using ::android::IGraphicBufferProducer;
using ::android::IGraphicBufferConsumer;
using ::android::BufferQueue;
using ::android::BufferItemConsumer;
using ::android::Surface;
using ::android::CameraParameters;
using ::android::hardware::graphics::common::V1_0::BufferUsage;
using ::android::hardware::graphics::common::V1_0::PixelFormat;
using ::android::hardware::camera::common::V1_0::Status;
using ::android::hardware::camera::common::V1_0::CameraDeviceStatus;
using ::android::hardware::camera::common::V1_0::TorchMode;
using ::android::hardware::camera::common::V1_0::TorchModeStatus;
using ::android::hardware::camera::provider::V2_4::ICameraProvider;
using ::android::hardware::camera::provider::V2_4::ICameraProviderCallback;
using ::android::hardware::camera::device::V3_2::ICameraDevice;
using ::android::hardware::camera::device::V3_2::BufferCache;
using ::android::hardware::camera::device::V3_2::CaptureRequest;
using ::android::hardware::camera::device::V3_2::CaptureResult;
using ::android::hardware::camera::device::V3_2::ICameraDeviceCallback;
using ::android::hardware::camera::device::V3_2::ICameraDeviceSession;
using ::android::hardware::camera::device::V3_2::NotifyMsg;
using ::android::hardware::camera::device::V3_2::RequestTemplate;
using ::android::hardware::camera::device::V3_2::Stream;
using ::android::hardware::camera::device::V3_2::StreamType;
using ::android::hardware::camera::device::V3_2::StreamRotation;
using ::android::hardware::camera::device::V3_2::StreamConfiguration;
using ::android::hardware::camera::device::V3_2::StreamConfigurationMode;
using ::android::hardware::camera::device::V3_2::CameraMetadata;
using ::android::hardware::camera::device::V3_2::HalStreamConfiguration;
using ::android::hardware::camera::device::V3_2::BufferStatus;
using ::android::hardware::camera::device::V3_2::StreamBuffer;
using ::android::hardware::camera::device::V3_2::MsgType;
using ::android::hardware::camera::device::V3_2::ErrorMsg;
using ::android::hardware::camera::device::V3_2::ErrorCode;
using ::android::hardware::camera::device::V1_0::CameraFacing;
using ::android::hardware::camera::device::V1_0::NotifyCallbackMsg;
using ::android::hardware::camera::device::V1_0::CommandType;
using ::android::hardware::camera::device::V1_0::DataCallbackMsg;
using ::android::hardware::camera::device::V1_0::CameraFrameMetadata;
using ::android::hardware::camera::device::V1_0::ICameraDevicePreviewCallback;
using ::android::hardware::camera::device::V1_0::FrameCallbackFlag;
using ::android::hardware::camera::device::V1_0::HandleTimestampMessage;

const char kCameraPassthroughServiceName[] = "legacy/0";
const uint32_t kMaxPreviewWidth = 1920;
const uint32_t kMaxPreviewHeight = 1080;
const uint32_t kMaxVideoWidth = 4096;
const uint32_t kMaxVideoHeight = 2160;
const int64_t kStreamBufferTimeoutSec = 3;
const int64_t kAutoFocusTimeoutSec = 5;
const int64_t kTorchTimeoutSec = 1;
const int64_t kEmptyFlushTimeoutMSec = 200;
const char kDumpOutput[] = "/dev/null";

struct AvailableStream {
    int32_t width;
    int32_t height;
    int32_t format;
};

struct AvailableZSLInputOutput {
    int32_t inputFormat;
    int32_t outputFormat;
};

namespace {
    // "device@<version>/legacy/<id>"
    const char *kDeviceNameRE = "device@([0-9]+\\.[0-9]+)/legacy/(.+)";
    const int CAMERA_DEVICE_API_VERSION_3_2 = 0x302;
    const int CAMERA_DEVICE_API_VERSION_1_0 = 0x100;
    const char *kHAL3_2 = "3.2";
    const char *kHAL1_0 = "1.0";

    bool matchDeviceName(const hidl_string& deviceName, std::smatch& sm) {
        std::regex e(kDeviceNameRE);
        std::string deviceNameStd(deviceName.c_str());
        return std::regex_match(deviceNameStd, sm, e);
    }

    int getCameraDeviceVersion(const hidl_string& deviceName) {
        std::smatch sm;
        bool match = matchDeviceName(deviceName, sm);
        if (!match) {
            return -1;
        }
        std::string version = sm[1].str();
        if (version.compare(kHAL3_2) == 0) {
            // maybe switched to 3.4 or define the hidl version enumlater
            return CAMERA_DEVICE_API_VERSION_3_2;
        } else if (version.compare(kHAL1_0) == 0) {
            return CAMERA_DEVICE_API_VERSION_1_0;
        }
        return 0;
    }

    Status mapToStatus(::android::status_t s)  {
        switch(s) {
            case ::android::OK:
                return Status::OK ;
            case ::android::BAD_VALUE:
                return Status::ILLEGAL_ARGUMENT ;
            case -EBUSY:
                return Status::CAMERA_IN_USE;
            case -EUSERS:
                return Status::MAX_CAMERAS_IN_USE;
            case ::android::UNKNOWN_TRANSACTION:
                return Status::METHOD_NOT_SUPPORTED;
            case ::android::INVALID_OPERATION:
                return Status::OPERATION_NOT_SUPPORTED;
            case ::android::DEAD_OBJECT:
                return Status::CAMERA_DISCONNECTED;
        }
        ALOGW("Unexpected HAL status code %d", s);
        return Status::OPERATION_NOT_SUPPORTED;
    }
}

// Test environment for camera
class CameraHidlEnvironment : public ::testing::Environment {
public:
    // get the test environment singleton
    static CameraHidlEnvironment* Instance() {
        static CameraHidlEnvironment* instance = new CameraHidlEnvironment;
        return instance;
    }

    virtual void SetUp() override;
    virtual void TearDown() override;

    sp<ICameraProvider> mProvider;

private:
    CameraHidlEnvironment() {}

    GTEST_DISALLOW_COPY_AND_ASSIGN_(CameraHidlEnvironment);
};

void CameraHidlEnvironment::SetUp() {
    // TODO: test the binderized mode
    mProvider = ::testing::VtsHalHidlTargetTestBase::getService<ICameraProvider>(kCameraPassthroughServiceName);
    // TODO: handle the device doesn't have any camera case
    ALOGI_IF(mProvider, "provider is not nullptr, %p", mProvider.get());
    ASSERT_NE(mProvider, nullptr);
}

void CameraHidlEnvironment::TearDown() {
    ALOGI("TearDown CameraHidlEnvironment");
}

struct BufferItemHander: public BufferItemConsumer::FrameAvailableListener {
    BufferItemHander(wp<BufferItemConsumer> consumer) : mConsumer(consumer) {}

    void onFrameAvailable(const android::BufferItem&) override {
        sp<BufferItemConsumer> consumer = mConsumer.promote();
        ASSERT_NE(nullptr, consumer.get());

        android::BufferItem buffer;
        ASSERT_EQ(android::OK, consumer->acquireBuffer(&buffer, 0));
        ASSERT_EQ(android::OK, consumer->releaseBuffer(buffer));
    }

 private:
    wp<BufferItemConsumer> mConsumer;
};

struct PreviewWindowCb : public ICameraDevicePreviewCallback {
    PreviewWindowCb(sp<ANativeWindow> anw) : mPreviewWidth(0),
            mPreviewHeight(0), mFormat(0), mPreviewUsage(0),
            mPreviewSwapInterval(-1), mCrop{-1, -1, -1, -1}, mAnw(anw) {}

    using dequeueBuffer_cb =
            std::function<void(Status status, uint64_t bufferId,
                    const hidl_handle& buffer, uint32_t stride)>;
    Return<void> dequeueBuffer(dequeueBuffer_cb _hidl_cb) override;

    Return<Status> enqueueBuffer(uint64_t bufferId) override;

    Return<Status> cancelBuffer(uint64_t bufferId) override;

    Return<Status> setBufferCount(uint32_t count) override;

    Return<Status> setBuffersGeometry(uint32_t w,
            uint32_t h, PixelFormat format) override;

    Return<Status> setCrop(int32_t left, int32_t top,
            int32_t right, int32_t bottom) override;

    Return<Status> setUsage(BufferUsage usage) override;

    Return<Status> setSwapInterval(int32_t interval) override;

    using getMinUndequeuedBufferCount_cb =
            std::function<void(Status status, uint32_t count)>;
    Return<void> getMinUndequeuedBufferCount(
            getMinUndequeuedBufferCount_cb _hidl_cb) override;

    Return<Status> setTimestamp(int64_t timestamp) override;

 private:
    struct BufferHasher {
        size_t operator()(const buffer_handle_t& buf) const {
            if (buf == nullptr)
                return 0;

            size_t result = 1;
            result = 31 * result + buf->numFds;
            result = 31 * result + buf->numInts;
            int length = buf->numFds + buf->numInts;
            for (int i = 0; i < length; i++) {
                result = 31 * result + buf->data[i];
            }
            return result;
        }
    };

    struct BufferComparator {
        bool operator()(const buffer_handle_t& buf1,
                const buffer_handle_t& buf2) const {
            if ((buf1->numFds == buf2->numFds) &&
                    (buf1->numInts == buf2->numInts)) {
                int length = buf1->numFds + buf1->numInts;
                for (int i = 0; i < length; i++) {
                    if (buf1->data[i] != buf2->data[i]) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        }
    };

    std::pair<bool, uint64_t> getBufferId(ANativeWindowBuffer* anb);
    void cleanupCirculatingBuffers();

    std::mutex mBufferIdMapLock; // protecting mBufferIdMap and mNextBufferId
    typedef std::unordered_map<const buffer_handle_t, uint64_t,
            BufferHasher, BufferComparator> BufferIdMap;

    BufferIdMap mBufferIdMap; // stream ID -> per stream buffer ID map
    std::unordered_map<uint64_t, ANativeWindowBuffer*> mReversedBufMap;
    uint64_t mNextBufferId = 1;

    uint32_t mPreviewWidth, mPreviewHeight;
    int mFormat, mPreviewUsage;
    int32_t mPreviewSwapInterval;
    android_native_rect_t mCrop;
    sp<ANativeWindow> mAnw;     //Native window reference
};

std::pair<bool, uint64_t> PreviewWindowCb::getBufferId(
        ANativeWindowBuffer* anb) {
    std::lock_guard<std::mutex> lock(mBufferIdMapLock);

    buffer_handle_t& buf = anb->handle;
    auto it = mBufferIdMap.find(buf);
    if (it == mBufferIdMap.end()) {
        uint64_t bufId = mNextBufferId++;
        mBufferIdMap[buf] = bufId;
        mReversedBufMap[bufId] = anb;
        return std::make_pair(true, bufId);
    } else {
        return std::make_pair(false, it->second);
    }
}

void PreviewWindowCb::cleanupCirculatingBuffers() {
    std::lock_guard<std::mutex> lock(mBufferIdMapLock);
    mBufferIdMap.clear();
    mReversedBufMap.clear();
}

Return<void> PreviewWindowCb::dequeueBuffer(dequeueBuffer_cb _hidl_cb) {
    ANativeWindowBuffer* anb;
    auto rc = native_window_dequeue_buffer_and_wait(mAnw.get(), &anb);
    uint64_t bufferId = 0;
    uint32_t stride = 0;
    hidl_handle buf = nullptr;
    if (rc == ::android::OK) {
        auto pair = getBufferId(anb);
        buf = (pair.first) ? anb->handle : nullptr;
        bufferId = pair.second;
        stride = anb->stride;
    }

    _hidl_cb(mapToStatus(rc), bufferId, buf, stride);
    return Void();
}

Return<Status> PreviewWindowCb::enqueueBuffer(uint64_t bufferId) {
    if (mReversedBufMap.count(bufferId) == 0) {
        ALOGE("%s: bufferId %" PRIu64 " not found", __FUNCTION__, bufferId);
        return Status::ILLEGAL_ARGUMENT;
    }
    return mapToStatus(mAnw->queueBuffer(mAnw.get(),
            mReversedBufMap.at(bufferId), -1));
}

Return<Status> PreviewWindowCb::cancelBuffer(uint64_t bufferId) {
    if (mReversedBufMap.count(bufferId) == 0) {
        ALOGE("%s: bufferId %" PRIu64 " not found", __FUNCTION__, bufferId);
        return Status::ILLEGAL_ARGUMENT;
    }
    return mapToStatus(mAnw->cancelBuffer(mAnw.get(),
            mReversedBufMap.at(bufferId), -1));
}

Return<Status> PreviewWindowCb::setBufferCount(uint32_t count) {
    if (mAnw.get() != nullptr) {
        // WAR for b/27039775
        native_window_api_disconnect(mAnw.get(), NATIVE_WINDOW_API_CAMERA);
        native_window_api_connect(mAnw.get(), NATIVE_WINDOW_API_CAMERA);
        if (mPreviewWidth != 0) {
            native_window_set_buffers_dimensions(mAnw.get(),
                    mPreviewWidth, mPreviewHeight);
            native_window_set_buffers_format(mAnw.get(), mFormat);
        }
        if (mPreviewUsage != 0) {
            native_window_set_usage(mAnw.get(), mPreviewUsage);
        }
        if (mPreviewSwapInterval >= 0) {
            mAnw->setSwapInterval(mAnw.get(), mPreviewSwapInterval);
        }
        if (mCrop.left >= 0) {
            native_window_set_crop(mAnw.get(), &(mCrop));
        }
    }

    auto rc = native_window_set_buffer_count(mAnw.get(), count);
    if (rc == ::android::OK) {
        cleanupCirculatingBuffers();
    }

    return mapToStatus(rc);
}

Return<Status> PreviewWindowCb::setBuffersGeometry(uint32_t w, uint32_t h,
        PixelFormat format) {
    auto rc = native_window_set_buffers_dimensions(mAnw.get(), w, h);
    if (rc == ::android::OK) {
        mPreviewWidth = w;
        mPreviewHeight = h;
        rc = native_window_set_buffers_format(mAnw.get(),
                static_cast<int>(format));
        if (rc == ::android::OK) {
            mFormat = static_cast<int>(format);
        }
    }

    return mapToStatus(rc);
}

Return<Status> PreviewWindowCb::setCrop(int32_t left, int32_t top,
        int32_t right, int32_t bottom) {
    android_native_rect_t crop = { left, top, right, bottom };
    auto rc = native_window_set_crop(mAnw.get(), &crop);
    if (rc == ::android::OK) {
        mCrop = crop;
    }
    return mapToStatus(rc);
}

Return<Status> PreviewWindowCb::setUsage(BufferUsage usage) {
    auto rc = native_window_set_usage(mAnw.get(), static_cast<int>(usage));
    if (rc == ::android::OK) {
        mPreviewUsage =  static_cast<int>(usage);
    }
    return mapToStatus(rc);
}

Return<Status> PreviewWindowCb::setSwapInterval(int32_t interval) {
    auto rc = mAnw->setSwapInterval(mAnw.get(), interval);
    if (rc == ::android::OK) {
        mPreviewSwapInterval = interval;
    }
    return mapToStatus(rc);
}

Return<void> PreviewWindowCb::getMinUndequeuedBufferCount(
        getMinUndequeuedBufferCount_cb _hidl_cb) {
    int count = 0;
    auto rc = mAnw->query(mAnw.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &count);
    _hidl_cb(mapToStatus(rc), count);
    return Void();
}

Return<Status> PreviewWindowCb::setTimestamp(int64_t timestamp) {
    return mapToStatus(native_window_set_buffers_timestamp(mAnw.get(),
            timestamp));
}

// The main test class for camera HIDL HAL.
class CameraHidlTest : public ::testing::VtsHalHidlTargetTestBase {
public:
    virtual void SetUp() override {}
    virtual void TearDown() override {}

    hidl_vec<hidl_string> getCameraDeviceNames();

    struct EmptyDeviceCb : public ICameraDeviceCallback {
        virtual Return<void> processCaptureResult(const hidl_vec<CaptureResult>& /*results*/) override {
            ALOGI("processCaptureResult callback");
            ADD_FAILURE(); // Empty callback should not reach here
            return Void();
        }

        virtual Return<void> notify(const hidl_vec<NotifyMsg>& /*msgs*/) override {
            ALOGI("notify callback");
            ADD_FAILURE(); // Empty callback should not reach here
            return Void();
        }
    };

    struct DeviceCb : public ICameraDeviceCallback {
        DeviceCb(CameraHidlTest *parent) : mParent(parent) {}
        Return<void> processCaptureResult(const hidl_vec<CaptureResult>& results) override;
        Return<void> notify(const hidl_vec<NotifyMsg>& msgs) override;

     private:
        CameraHidlTest *mParent;               // Parent object
    };

    struct TorchProviderCb : public ICameraProviderCallback {
        TorchProviderCb(CameraHidlTest *parent) : mParent(parent) {}
        virtual Return<void> cameraDeviceStatusChange(
                const hidl_string&, CameraDeviceStatus) override {
            return Void();
        }

        virtual Return<void> torchModeStatusChange(
                const hidl_string&, TorchModeStatus newStatus) override {
            std::lock_guard<std::mutex> l(mParent->mTorchLock);
            mParent->mTorchStatus = newStatus;
            mParent->mTorchCond.notify_one();
            return Void();
        }

     private:
        CameraHidlTest *mParent;               // Parent object
    };

    struct Camera1DeviceCb :
            public ::android::hardware::camera::device::V1_0::ICameraDeviceCallback {
        Camera1DeviceCb(CameraHidlTest *parent) : mParent(parent) {}

        Return<void> notifyCallback(NotifyCallbackMsg msgType,
                int32_t ext1, int32_t ext2) override;

        Return<uint32_t> registerMemory(const hidl_handle& descriptor,
                uint32_t bufferSize, uint32_t bufferCount) override;

        Return<void> unregisterMemory(uint32_t memId) override;

        Return<void> dataCallback(DataCallbackMsg msgType,
                uint32_t data, uint32_t bufferIndex,
                const CameraFrameMetadata& metadata) override;

        Return<void> dataCallbackTimestamp(DataCallbackMsg msgType,
                uint32_t data, uint32_t bufferIndex,
                int64_t timestamp) override;

        Return<void> handleCallbackTimestamp(DataCallbackMsg msgType,
                const hidl_handle& frameData,uint32_t data,
                uint32_t bufferIndex, int64_t timestamp) override;

        Return<void> handleCallbackTimestampBatch(DataCallbackMsg msgType,
                const ::android::hardware::hidl_vec<HandleTimestampMessage>& batch) override;


     private:
        CameraHidlTest *mParent;               // Parent object
    };

    void openCameraDevice(const std::string &name,const CameraHidlEnvironment* env,
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> *device /*out*/);
    void setupPreviewWindow(
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
            sp<BufferItemConsumer> *bufferItemConsumer /*out*/,
            sp<BufferItemHander> *bufferHandler /*out*/);
    void stopPreviewAndClose(
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device);
    void startPreview(
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device);
    void enableMsgType(unsigned int msgType,
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device);
    void disableMsgType(unsigned int msgType,
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device);
    void getParameters(
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
            CameraParameters *cameraParams /*out*/);
    void setParameters(
            const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
            const CameraParameters &cameraParams);
    void waitForFrameLocked(DataCallbackMsg msgFrame,
            std::unique_lock<std::mutex> &l);
    void openEmptyDeviceSession(const std::string &name,
            const CameraHidlEnvironment* env,
            sp<ICameraDeviceSession> *session /*out*/,
            camera_metadata_t **staticMeta /*out*/);
    void configurePreviewStream(const std::string &name,
            const CameraHidlEnvironment* env,
            const AvailableStream *previewThreshold,
            sp<ICameraDeviceSession> *session /*out*/,
            Stream *previewStream /*out*/,
            HalStreamConfiguration *halStreamConfig /*out*/);
    static Status getAvailableOutputStreams(camera_metadata_t *staticMeta,
            std::vector<AvailableStream> &outputStreams,
            const AvailableStream *threshold = nullptr);
    static Status isConstrainedModeAvailable(camera_metadata_t *staticMeta);
    static Status pickConstrainedModeSize(camera_metadata_t *staticMeta,
            AvailableStream &hfrStream);
    static Status isZSLModeAvailable(camera_metadata_t *staticMeta);
    static Status getZSLInputOutputMap(camera_metadata_t *staticMeta,
            std::vector<AvailableZSLInputOutput> &inputOutputMap);
    static Status findLargestSize(
            const std::vector<AvailableStream> &streamSizes,
            int32_t format, AvailableStream &result);
    static Status isAutoFocusModeAvailable(
            ::android::CameraParameters &cameraParams, const char *mode) ;

protected:
    std::mutex mLock;                          // Synchronize access to member variables
    std::condition_variable mResultCondition;  // Condition variable for incoming results
    uint32_t mResultFrameNumber;               // Expected result frame number
    std::vector<StreamBuffer> mResultBuffers;  // Holds stream buffers from capture result
    std::vector<ErrorMsg> mErrors;             // Holds incoming error notifications
    DataCallbackMsg mDataMessageTypeReceived;  // Most recent message type received through data callbacks
    uint32_t mVideoBufferIndex;                // Buffer index of the most recent video buffer
    uint32_t mVideoData;                       // Buffer data of the most recent video buffer
    hidl_handle mVideoNativeHandle;            // Most recent video buffer native handle
    NotifyCallbackMsg mNotifyMessage;          // Current notification message

    std::mutex mTorchLock;                     // Synchronize access to torch status
    std::condition_variable mTorchCond;        // Condition variable for torch status
    TorchModeStatus mTorchStatus;              // Current torch status

    // Holds camera registered buffers
    std::unordered_map<uint32_t, sp<::android::MemoryHeapBase> > mMemoryPool;
};

Return<void> CameraHidlTest::Camera1DeviceCb::notifyCallback(
        NotifyCallbackMsg msgType, int32_t ext1 __unused,
        int32_t ext2 __unused) {
    std::unique_lock<std::mutex> l(mParent->mLock);
    mParent->mNotifyMessage = msgType;
    mParent->mResultCondition.notify_one();

    return Void();
}

Return<uint32_t> CameraHidlTest::Camera1DeviceCb::registerMemory(
        const hidl_handle& descriptor, uint32_t bufferSize,
        uint32_t bufferCount) {
    if (descriptor->numFds != 1) {
        ADD_FAILURE() << "camera memory descriptor has"
                " numFds " <<  descriptor->numFds << " (expect 1)" ;
        return 0;
    }
    if (descriptor->data[0] < 0) {
        ADD_FAILURE() << "camera memory descriptor has"
                " FD " << descriptor->data[0] << " (expect >= 0)";
        return 0;
    }

    sp<::android::MemoryHeapBase> pool = new ::android::MemoryHeapBase(
            descriptor->data[0], bufferSize*bufferCount, 0, 0);
    mParent->mMemoryPool.emplace(pool->getHeapID(), pool);

    return pool->getHeapID();
}

Return<void> CameraHidlTest::Camera1DeviceCb::unregisterMemory(uint32_t memId) {
    if (mParent->mMemoryPool.count(memId) == 0) {
        ALOGE("%s: memory pool ID %d not found", __FUNCTION__, memId);
        ADD_FAILURE();
        return Void();
    }

    mParent->mMemoryPool.erase(memId);
    return Void();
}

Return<void> CameraHidlTest::Camera1DeviceCb::dataCallback(
        DataCallbackMsg msgType __unused, uint32_t data __unused,
        uint32_t bufferIndex __unused,
        const CameraFrameMetadata& metadata __unused) {
    std::unique_lock<std::mutex> l(mParent->mLock);
    mParent->mDataMessageTypeReceived = msgType;
    mParent->mResultCondition.notify_one();

    return Void();
}

Return<void> CameraHidlTest::Camera1DeviceCb::dataCallbackTimestamp(
        DataCallbackMsg msgType, uint32_t data,
        uint32_t bufferIndex, int64_t timestamp __unused) {
    std::unique_lock<std::mutex> l(mParent->mLock);
    mParent->mDataMessageTypeReceived = msgType;
    mParent->mVideoBufferIndex = bufferIndex;
    if (mParent->mMemoryPool.count(data) == 0) {
        ADD_FAILURE() << "memory pool ID " << data << "not found";
    }
    mParent->mVideoData = data;
    mParent->mResultCondition.notify_one();

    return Void();
}

Return<void> CameraHidlTest::Camera1DeviceCb::handleCallbackTimestamp(
        DataCallbackMsg msgType, const hidl_handle& frameData,
        uint32_t data __unused, uint32_t bufferIndex,
        int64_t timestamp __unused) {
    std::unique_lock<std::mutex> l(mParent->mLock);
    mParent->mDataMessageTypeReceived = msgType;
    mParent->mVideoBufferIndex = bufferIndex;
    if (mParent->mMemoryPool.count(data) == 0) {
        ADD_FAILURE() << "memory pool ID " << data << " not found";
    }
    mParent->mVideoData = data;
    mParent->mVideoNativeHandle = frameData;
    mParent->mResultCondition.notify_one();

    return Void();
}

Return<void> CameraHidlTest::Camera1DeviceCb::handleCallbackTimestampBatch(
        DataCallbackMsg msgType,
        const hidl_vec<HandleTimestampMessage>& batch) {
    std::unique_lock<std::mutex> l(mParent->mLock);
    for (auto& msg : batch) {
        mParent->mDataMessageTypeReceived = msgType;
        mParent->mVideoBufferIndex = msg.bufferIndex;
        if (mParent->mMemoryPool.count(msg.data) == 0) {
            ADD_FAILURE() << "memory pool ID " << msg.data << " not found";
        }
        mParent->mVideoData = msg.data;
        mParent->mVideoNativeHandle = msg.frameData;
        mParent->mResultCondition.notify_one();
    }
    return Void();
}

Return<void> CameraHidlTest::DeviceCb::processCaptureResult(
        const hidl_vec<CaptureResult>& results) {
    if (nullptr == mParent) {
        return Void();
    }

    std::unique_lock<std::mutex> l(mParent->mLock);
    const CaptureResult& result = results[0];

    if(mParent->mResultFrameNumber != result.frameNumber) {
        ALOGE("%s: Unexpected frame number! Expected: %u received: %u",
              __func__, mParent->mResultFrameNumber, result.frameNumber);
        ADD_FAILURE();
    }

    size_t resultLength = result.outputBuffers.size();
    for (size_t i = 0; i < resultLength; i++) {
        mParent->mResultBuffers.push_back(result.outputBuffers[i]);
    }

    // TODO(epeev): Handle partial results in case client supports them and
    //              verify the result against request settings.

    l.unlock();
    mParent->mResultCondition.notify_one();

    return Void();
}

Return<void> CameraHidlTest::DeviceCb::notify(
        const hidl_vec<NotifyMsg>& messages) {
    const NotifyMsg& message = messages[0];

    if (MsgType::ERROR == message.type) {
        {
            std::lock_guard<std::mutex> l(mParent->mLock);
            mParent->mErrors.push_back(message.msg.error);
        }

        if ((ErrorCode::ERROR_REQUEST == message.msg.error.errorCode)
                || (ErrorCode::ERROR_BUFFER == message.msg.error.errorCode)) {
            mParent->mResultCondition.notify_one();
        }
    }

    return Void();
}

hidl_vec<hidl_string> CameraHidlTest::getCameraDeviceNames() {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames;
    Return<void> ret;
    ret = env->mProvider->getCameraIdList(
        [&](auto status, const auto& idList) {
            ALOGI("getCameraIdList returns status:%d", (int)status);
            for (size_t i = 0; i < idList.size(); i++) {
                ALOGI("Camera Id[%zu] is %s", i, idList[i].c_str());
            }
            ASSERT_EQ(Status::OK, status);
            cameraDeviceNames = idList;
        });
    if (!ret.isOk()) {
        ADD_FAILURE();
    }
    return cameraDeviceNames;
}

// Test if ICameraProvider::isTorchModeSupported returns Status::OK
TEST_F(CameraHidlTest, isTorchModeSupported) {
    Return<void> ret;
    ret = CameraHidlEnvironment::Instance()->mProvider->isSetTorchModeSupported(
        [&](auto status, bool support) {
            ALOGI("isSetTorchModeSupported returns status:%d supported:%d",
                    (int)status, support);
            ASSERT_EQ(Status::OK, status);
        });
    ASSERT_TRUE(ret.isOk());
}

// TODO: consider removing this test if getCameraDeviceNames() has the same coverage
TEST_F(CameraHidlTest, getCameraIdList) {
    Return<void> ret;
    ret = CameraHidlEnvironment::Instance()->mProvider->getCameraIdList(
        [&](auto status, const auto& idList) {
            ALOGI("getCameraIdList returns status:%d", (int)status);
            for (size_t i = 0; i < idList.size(); i++) {
                ALOGI("Camera Id[%zu] is %s", i, idList[i].c_str());
            }
            ASSERT_EQ(Status::OK, status);
            // This is true for internal camera provider.
            // Not necessary hold for external cameras providers
            ASSERT_GT(idList.size(), 0u);
        });
    ASSERT_TRUE(ret.isOk());
}

// Test if ICameraProvider::getVendorTags returns Status::OK
TEST_F(CameraHidlTest, getVendorTags) {
    Return<void> ret;
    ret = CameraHidlEnvironment::Instance()->mProvider->getVendorTags(
        [&](auto status, const auto& vendorTagSecs) {
            ALOGI("getVendorTags returns status:%d numSections %zu",
                    (int)status, vendorTagSecs.size());
            for (size_t i = 0; i < vendorTagSecs.size(); i++) {
                ALOGI("Vendor tag section %zu name %s",
                        i, vendorTagSecs[i].sectionName.c_str());
                for (size_t j = 0; j < vendorTagSecs[i].tags.size(); j++) {
                    const auto& tag = vendorTagSecs[i].tags[j];
                    ALOGI("Vendor tag id %u name %s type %d",
                            tag.tagId,
                            tag.tagName.c_str(),
                            (int) tag.tagType);
                }
            }
            ASSERT_EQ(Status::OK, status);
        });
    ASSERT_TRUE(ret.isOk());
}

// Test if ICameraProvider::setCallback returns Status::OK
TEST_F(CameraHidlTest, setCallback) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    struct ProviderCb : public ICameraProviderCallback {
        virtual Return<void> cameraDeviceStatusChange(
                const hidl_string& cameraDeviceName,
                CameraDeviceStatus newStatus) override {
            ALOGI("camera device status callback name %s, status %d",
                    cameraDeviceName.c_str(), (int) newStatus);
            return Void();
        }

        virtual Return<void> torchModeStatusChange(
                const hidl_string& cameraDeviceName,
                TorchModeStatus newStatus) override {
            ALOGI("Torch mode status callback name %s, status %d",
                    cameraDeviceName.c_str(), (int) newStatus);
            return Void();
        }
    };
    sp<ProviderCb> cb = new ProviderCb;
    auto status = env->mProvider->setCallback(cb);
    ASSERT_TRUE(status.isOk());
    ASSERT_EQ(Status::OK, status);
}

// Test if ICameraProvider::getCameraDeviceInterface returns Status::OK and non-null device
TEST_F(CameraHidlTest, getCameraDeviceInterface) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device3_2) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device3_2, nullptr);
                });
            ASSERT_TRUE(ret.isOk());
        } else if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device1) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device1, nullptr);
                });
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that the device resource cost can be retrieved and the values are
// sane.
TEST_F(CameraHidlTest, getResourceCost) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            ALOGI("getResourceCost: Testing camera device %s", name.c_str());
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            ret = device3_2->getResourceCost(
                [&](auto status, const auto& resourceCost) {
                    ALOGI("getResourceCost returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ALOGI("    Resource cost is %d", resourceCost.resourceCost);
                    ASSERT_LE(resourceCost.resourceCost, 100u);
                    for (const auto& name : resourceCost.conflictingDevices) {
                        ALOGI("    Conflicting device: %s", name.c_str());
                    }
                });
            ASSERT_TRUE(ret.isOk());
        } else {
            ::android::sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            ALOGI("getResourceCost: Testing camera device %s", name.c_str());
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device1 = device;
                });
            ASSERT_TRUE(ret.isOk());

            ret = device1->getResourceCost(
                [&](auto status, const auto& resourceCost) {
                    ALOGI("getResourceCost returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ALOGI("    Resource cost is %d", resourceCost.resourceCost);
                    ASSERT_LE(resourceCost.resourceCost, 100u);
                    for (const auto& name : resourceCost.conflictingDevices) {
                        ALOGI("    Conflicting device: %s", name.c_str());
                    }
                });
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that the static camera info can be retrieved
// successfully.
TEST_F(CameraHidlTest, getCameraInfo) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            ::android::sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            ALOGI("getCameraCharacteristics: Testing camera device %s", name.c_str());
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device1 = device;
                });
            ASSERT_TRUE(ret.isOk());

            ret = device1->getCameraInfo(
                [&](auto status, const auto& info) {
                    ALOGI("getCameraInfo returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    switch(info.orientation) {
                        case 0:
                        case 90:
                        case 180:
                        case 270:
                            //Expected cases
                            ALOGI("camera orientation: %d", info.orientation);
                            break;
                        default:
                            FAIL() << "Unexpected camera orientation:" << info.orientation;
                    }
                    switch(info.facing) {
                        case CameraFacing::BACK:
                        case CameraFacing::FRONT:
                        case CameraFacing::EXTERNAL:
                            //Expected cases
                            ALOGI("camera facing: %d", info.facing);
                            break;
                        default:
                            FAIL() << "Unexpected camera facing:" << static_cast<uint32_t> (info.facing);
                    }
                });
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check whether preview window can be configured
TEST_F(CameraHidlTest, setPreviewWindow) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
                    openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1,
                    &bufferItemConsumer /*out*/, &bufferHandler /*out*/);

            Return<void> ret;
            ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that setting preview window fails in case device is not open
TEST_F(CameraHidlTest, setPreviewWindowInvalid) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            ::android::sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            ALOGI("getCameraCharacteristics: Testing camera device %s", name.c_str());
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device1 = device;
                });
            ASSERT_TRUE(ret.isOk());

            Return<Status> returnStatus = device1->setPreviewWindow(nullptr);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OPERATION_NOT_SUPPORTED, returnStatus);
        }
    }
}

// Start and stop preview checking whether it gets enabled in between.
TEST_F(CameraHidlTest, startStopPreview) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
                    openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1,
                    &bufferItemConsumer /*out*/, &bufferHandler /*out*/);

            startPreview(device1);

            Return<bool> returnBoolStatus = device1->previewEnabled();
            ASSERT_TRUE(returnBoolStatus.isOk());
            ASSERT_TRUE(returnBoolStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Start preview without active preview window. Preview should start as soon
// as a valid active window gets configured.
TEST_F(CameraHidlTest, startStopPreviewDelayed) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            Return<Status> returnStatus = device1->setPreviewWindow(nullptr);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            startPreview(device1);

            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);

            //Preview should get enabled now
            Return<bool> returnBoolStatus = device1->previewEnabled();
            ASSERT_TRUE(returnBoolStatus.isOk());
            ASSERT_TRUE(returnBoolStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Verify that image capture behaves as expected along with preview callbacks.
TEST_F(CameraHidlTest, takePicture) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);

            {
                std::unique_lock<std::mutex> l(mLock);
                mDataMessageTypeReceived = DataCallbackMsg::RAW_IMAGE_NOTIFY;
            }

            enableMsgType((unsigned int)DataCallbackMsg::PREVIEW_FRAME, device1);
            startPreview(device1);

            {
                std::unique_lock<std::mutex> l(mLock);
                waitForFrameLocked(DataCallbackMsg::PREVIEW_FRAME, l);
            }

            disableMsgType((unsigned int)DataCallbackMsg::PREVIEW_FRAME,
                            device1);
            enableMsgType((unsigned int)DataCallbackMsg::COMPRESSED_IMAGE,
                    device1);

            {
                std::unique_lock<std::mutex> l(mLock);
                mDataMessageTypeReceived = DataCallbackMsg::RAW_IMAGE_NOTIFY;
            }

            Return<Status> returnStatus = device1->takePicture();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            {
                std::unique_lock<std::mutex> l(mLock);
                waitForFrameLocked(DataCallbackMsg::COMPRESSED_IMAGE, l);
            }

            disableMsgType((unsigned int)DataCallbackMsg::COMPRESSED_IMAGE,
                    device1);
            stopPreviewAndClose(device1);
        }
    }
}

// Image capture should fail in case preview didn't get enabled first.
TEST_F(CameraHidlTest, takePictureFail) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            Return<Status> returnStatus = device1->takePicture();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_NE(Status::OK, returnStatus);

            Return<void> ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that image capture can be cancelled.
TEST_F(CameraHidlTest, cancelPicture) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);

            Return<Status> returnStatus = device1->takePicture();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            returnStatus = device1->cancelPicture();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Image capture cancel should fail when image capture is not running.
TEST_F(CameraHidlTest, cancelPictureFail) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);

            Return<Status> returnStatus = device1->cancelPicture();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_NE(Status::OK, returnStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Test basic video recording.
TEST_F(CameraHidlTest, startStopRecording) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());
            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);

            {
                std::unique_lock<std::mutex> l(mLock);
                mDataMessageTypeReceived = DataCallbackMsg::RAW_IMAGE_NOTIFY;
            }

            enableMsgType((unsigned int)DataCallbackMsg::PREVIEW_FRAME, device1);
            startPreview(device1);

            {
                std::unique_lock<std::mutex> l(mLock);
                waitForFrameLocked(DataCallbackMsg::PREVIEW_FRAME, l);
                mDataMessageTypeReceived = DataCallbackMsg::RAW_IMAGE_NOTIFY;
                mVideoBufferIndex = UINT32_MAX;
            }

            disableMsgType((unsigned int)DataCallbackMsg::PREVIEW_FRAME, device1);

            bool videoMetaEnabled = false;
            Return<Status> returnStatus = device1->storeMetaDataInBuffers(true);
            ASSERT_TRUE(returnStatus.isOk());
            // It is allowed for devices to not support this feature
            ASSERT_TRUE((Status::OK == returnStatus) ||
                    (Status::OPERATION_NOT_SUPPORTED == returnStatus));
            if (Status::OK == returnStatus) {
                videoMetaEnabled = true;
            }

            enableMsgType((unsigned int)DataCallbackMsg::VIDEO_FRAME, device1);
            Return<bool> returnBoolStatus = device1->recordingEnabled();
            ASSERT_TRUE(returnBoolStatus.isOk());
            ASSERT_FALSE(returnBoolStatus);

            returnStatus = device1->startRecording();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            {
                std::unique_lock<std::mutex> l(mLock);
                waitForFrameLocked(DataCallbackMsg::VIDEO_FRAME, l);
                ASSERT_NE(UINT32_MAX, mVideoBufferIndex);
                disableMsgType((unsigned int)DataCallbackMsg::VIDEO_FRAME,
                        device1);
            }

            returnBoolStatus = device1->recordingEnabled();
            ASSERT_TRUE(returnBoolStatus.isOk());
            ASSERT_TRUE(returnBoolStatus);

            Return<void> ret;
            if (videoMetaEnabled) {
                ret = device1->releaseRecordingFrameHandle(mVideoData,
                        mVideoBufferIndex, mVideoNativeHandle);
                ASSERT_TRUE(ret.isOk());
            } else {
                ret = device1->releaseRecordingFrame(mVideoData, mVideoBufferIndex);
                ASSERT_TRUE(ret.isOk());
            }

            ret = device1->stopRecording();
            ASSERT_TRUE(ret.isOk());

            stopPreviewAndClose(device1);
        }
    }
}

// It shouldn't be possible to start recording without enabling preview first.
TEST_F(CameraHidlTest, startRecordingFail) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            Return<bool> returnBoolStatus = device1->recordingEnabled();
            ASSERT_TRUE(returnBoolStatus.isOk());
            ASSERT_FALSE(returnBoolStatus);

            Return<Status> returnStatus = device1->startRecording();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_NE(Status::OK, returnStatus);

            Return<void> ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check autofocus support if available.
TEST_F(CameraHidlTest, autoFocus) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<const char *> focusModes = {CameraParameters::FOCUS_MODE_AUTO,
            CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE,
            CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO};

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            ::android::CameraParameters cameraParams;
            getParameters(device1, &cameraParams /*out*/);

            if (Status::OK != isAutoFocusModeAvailable(cameraParams,
                    CameraParameters::FOCUS_MODE_AUTO)) {
                Return<void> ret = device1->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }

            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);
            enableMsgType((unsigned int)NotifyCallbackMsg::FOCUS, device1);

            for (auto &iter : focusModes) {
                if (Status::OK != isAutoFocusModeAvailable(cameraParams,
                        iter)) {
                    continue;
                }

                cameraParams.set(CameraParameters::KEY_FOCUS_MODE, iter);
                setParameters(device1, cameraParams);
                {
                    std::unique_lock<std::mutex> l(mLock);
                    mNotifyMessage = NotifyCallbackMsg::ERROR;
                }

                Return<Status> returnStatus = device1->autoFocus();
                ASSERT_TRUE(returnStatus.isOk());
                ASSERT_EQ(Status::OK, returnStatus);

                {
                    std::unique_lock<std::mutex> l(mLock);
                    while (NotifyCallbackMsg::FOCUS != mNotifyMessage) {
                        auto timeout = std::chrono::system_clock::now() +
                                std::chrono::seconds(kAutoFocusTimeoutSec);
                        ASSERT_NE(std::cv_status::timeout,
                                mResultCondition.wait_until(l, timeout));
                    }
                }
            }

            disableMsgType((unsigned int)NotifyCallbackMsg::FOCUS, device1);
            stopPreviewAndClose(device1);
        }
    }
}

// In case autofocus is supported verify that it can be cancelled.
TEST_F(CameraHidlTest, cancelAutoFocus) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            ::android::CameraParameters cameraParams;
            getParameters(device1, &cameraParams /*out*/);

            if (Status::OK != isAutoFocusModeAvailable(cameraParams,
                    CameraParameters::FOCUS_MODE_AUTO)) {
                Return<void> ret = device1->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }

            // It should be fine to call before preview starts.
            ASSERT_EQ(Status::OK, device1->cancelAutoFocus());

            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);

            // It should be fine to call after preview starts too.
            Return<Status> returnStatus = device1->cancelAutoFocus();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            returnStatus = device1->autoFocus();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            returnStatus = device1->cancelAutoFocus();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Check whether face detection is available and try to enable&disable.
TEST_F(CameraHidlTest, sendCommandFaceDetection) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            ::android::CameraParameters cameraParams;
            getParameters(device1, &cameraParams /*out*/);

            int32_t hwFaces = cameraParams.getInt(
                    CameraParameters::KEY_MAX_NUM_DETECTED_FACES_HW);
            int32_t swFaces = cameraParams.getInt(
                    CameraParameters::KEY_MAX_NUM_DETECTED_FACES_SW);
            if ((0 >= hwFaces) && (0 >= swFaces)) {
                Return<void> ret = device1->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }

            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);

            if (0 < hwFaces) {
                Return<Status> returnStatus = device1->sendCommand(
                        CommandType::START_FACE_DETECTION,
                        CAMERA_FACE_DETECTION_HW, 0);
                ASSERT_TRUE(returnStatus.isOk());
                ASSERT_EQ(Status::OK, returnStatus);
                // TODO(epeev) : Enable and check for face notifications
                returnStatus = device1->sendCommand(
                        CommandType::STOP_FACE_DETECTION,
                        CAMERA_FACE_DETECTION_HW, 0);
                ASSERT_TRUE(returnStatus.isOk());
                ASSERT_EQ(Status::OK, returnStatus);
            }

            if (0 < swFaces) {
                Return<Status> returnStatus = device1->sendCommand(
                        CommandType::START_FACE_DETECTION,
                        CAMERA_FACE_DETECTION_SW, 0);
                ASSERT_TRUE(returnStatus.isOk());
                ASSERT_EQ(Status::OK, returnStatus);
                // TODO(epeev) : Enable and check for face notifications
                returnStatus = device1->sendCommand(
                        CommandType::STOP_FACE_DETECTION,
                        CAMERA_FACE_DETECTION_SW, 0);
                ASSERT_TRUE(returnStatus.isOk());
                ASSERT_EQ(Status::OK, returnStatus);
            }

            stopPreviewAndClose(device1);
        }
    }
}

// Check whether smooth zoom is available and try to enable&disable.
TEST_F(CameraHidlTest, sendCommandSmoothZoom) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            ::android::CameraParameters cameraParams;
            getParameters(device1, &cameraParams /*out*/);

            const char *smoothZoomStr = cameraParams.get(
                    CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED);
            bool smoothZoomSupported = ((nullptr != smoothZoomStr) &&
                    (strcmp(smoothZoomStr, CameraParameters::TRUE) == 0)) ?
                            true : false;
            if (!smoothZoomSupported) {
                Return<void> ret = device1->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }

            int32_t maxZoom = cameraParams.getInt(
                    CameraParameters::KEY_MAX_ZOOM);
            ASSERT_TRUE(0 < maxZoom);

            sp<BufferItemConsumer> bufferItemConsumer;
            sp<BufferItemHander> bufferHandler;
            setupPreviewWindow(device1, &bufferItemConsumer /*out*/,
                    &bufferHandler /*out*/);
            startPreview(device1);
            setParameters(device1, cameraParams);

            Return<Status> returnStatus = device1->sendCommand(
                    CommandType::START_SMOOTH_ZOOM, maxZoom, 0);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);
            // TODO(epeev) : Enable and check for face notifications
            returnStatus = device1->sendCommand(CommandType::STOP_SMOOTH_ZOOM,
                    0, 0);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            stopPreviewAndClose(device1);
        }
    }
}

// Basic sanity tests related to camera parameters.
TEST_F(CameraHidlTest, getSetParameters) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            ::android::CameraParameters cameraParams;
            getParameters(device1, &cameraParams /*out*/);

            int32_t width, height;
            cameraParams.getPictureSize(&width, &height);
            ASSERT_TRUE((0 < width) && (0 < height));
            cameraParams.getPreviewSize(&width, &height);
            ASSERT_TRUE((0 < width) && (0 < height));
            int32_t minFps, maxFps;
            cameraParams.getPreviewFpsRange(&minFps, &maxFps);
            ASSERT_TRUE((0 < minFps) && (0 < maxFps));
            ASSERT_NE(nullptr, cameraParams.getPreviewFormat());
            ASSERT_NE(nullptr, cameraParams.getPictureFormat());
            ASSERT_TRUE(strcmp(CameraParameters::PIXEL_FORMAT_JPEG,
                    cameraParams.getPictureFormat()) == 0);

            const char *flashMode = cameraParams.get(
                    CameraParameters::KEY_FLASH_MODE);
            ASSERT_TRUE((nullptr == flashMode) || (strcmp(
                    CameraParameters::FLASH_MODE_OFF, flashMode) == 0));

            const char *wbMode = cameraParams.get(
                    CameraParameters::KEY_WHITE_BALANCE);
            ASSERT_TRUE((nullptr == wbMode) || (strcmp(
                    CameraParameters::WHITE_BALANCE_AUTO, wbMode) == 0));

            const char *effect = cameraParams.get(CameraParameters::KEY_EFFECT);
            ASSERT_TRUE((nullptr == effect) || (strcmp(
                    CameraParameters::EFFECT_NONE, effect) == 0));

            ::android::Vector<::android::Size> previewSizes;
            cameraParams.getSupportedPreviewSizes(previewSizes);
            ASSERT_FALSE(previewSizes.empty());
            ::android::Vector<::android::Size> pictureSizes;
            cameraParams.getSupportedPictureSizes(pictureSizes);
            ASSERT_FALSE(pictureSizes.empty());
            const char *previewFormats = cameraParams.get(
                    CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS);
            ASSERT_NE(nullptr, previewFormats);
            ::android::String8 previewFormatsString(previewFormats);
            ASSERT_TRUE(previewFormatsString.contains(
                    CameraParameters::PIXEL_FORMAT_YUV420SP));
            ASSERT_NE(nullptr, cameraParams.get(
                    CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS));
            ASSERT_NE(nullptr, cameraParams.get(
                    CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES));
            const char *focusModes = cameraParams.get(
                    CameraParameters::KEY_SUPPORTED_FOCUS_MODES);
            ASSERT_NE(nullptr, focusModes);
            ::android::String8 focusModesString(focusModes);
            const char *focusMode = cameraParams.get(
                    CameraParameters::KEY_FOCUS_MODE);
            ASSERT_NE(nullptr, focusMode);
            // Auto focus mode should be default
            if (focusModesString.contains(CameraParameters::FOCUS_MODE_AUTO)) {
                ASSERT_TRUE(strcmp(
                        CameraParameters::FOCUS_MODE_AUTO, focusMode) == 0);
            }
            ASSERT_TRUE(0 < cameraParams.getInt(
                    CameraParameters::KEY_FOCAL_LENGTH));
            int32_t horizontalViewAngle = cameraParams.getInt(
                    CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE);
            ASSERT_TRUE((0 < horizontalViewAngle) && (360 >= horizontalViewAngle));
            int32_t verticalViewAngle = cameraParams.getInt(
                    CameraParameters::KEY_VERTICAL_VIEW_ANGLE);
            ASSERT_TRUE((0 < verticalViewAngle) && (360 >= verticalViewAngle));
            int32_t jpegQuality = cameraParams.getInt(
                    CameraParameters::KEY_JPEG_QUALITY);
            ASSERT_TRUE((1 <= jpegQuality) && (100 >= jpegQuality));
            int32_t jpegThumbQuality = cameraParams.getInt(
                    CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY);
            ASSERT_TRUE((1 <= jpegThumbQuality) && (100 >= jpegThumbQuality));

            cameraParams.setPictureSize(pictureSizes[0].width,
                    pictureSizes[0].height);
            cameraParams.setPreviewSize(previewSizes[0].width,
                    previewSizes[0].height);

            setParameters(device1, cameraParams);
            getParameters(device1, &cameraParams /*out*/);

            cameraParams.getPictureSize(&width, &height);
            ASSERT_TRUE((pictureSizes[0].width == width) &&
                    (pictureSizes[0].height == height));
            cameraParams.getPreviewSize(&width, &height);
            ASSERT_TRUE((previewSizes[0].width == width) &&
                    (previewSizes[0].height == height));

            Return<void> ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that the static camera characteristics can be retrieved
// successfully.
TEST_F(CameraHidlTest, getCameraCharacteristics) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            ALOGI("getCameraCharacteristics: Testing camera device %s", name.c_str());
            Return<void> ret;
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            ret = device3_2->getCameraCharacteristics(
                [&](auto status, const auto& chars) {
                    ALOGI("getCameraCharacteristics returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    const camera_metadata_t* metadata = (camera_metadata_t*) chars.data();
                    size_t expectedSize = chars.size();
                    int result = validate_camera_metadata_structure(metadata, &expectedSize);
                    ASSERT_TRUE(result == 0 || result == CAMERA_METADATA_VALIDATION_SHIFTED);
                    size_t entryCount = get_camera_metadata_entry_count(metadata);
                    // TODO: we can do better than 0 here. Need to check how many required
                    // characteristics keys we've defined.
                    ASSERT_GT(entryCount, 0u);
                    ALOGI("getCameraCharacteristics metadata entry count is %zu", entryCount);
                });
            ASSERT_TRUE(ret.isOk());
        }
    }
}

//In case it is supported verify that torch can be enabled.
//Check for corresponding toch callbacks as well.
TEST_F(CameraHidlTest, setTorchMode) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    bool torchControlSupported = false;
    Return<void> ret;

    ret = CameraHidlEnvironment::Instance()->mProvider->isSetTorchModeSupported(
        [&](auto status, bool support) {
            ALOGI("isSetTorchModeSupported returns status:%d supported:%d",
                    (int)status, support);
            ASSERT_EQ(Status::OK, status);
            torchControlSupported = support;
        });


    sp<TorchProviderCb> cb = new TorchProviderCb(this);
    Return<Status> returnStatus = env->mProvider->setCallback(cb);
    ASSERT_TRUE(returnStatus.isOk());
    ASSERT_EQ(Status::OK, returnStatus);

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            ALOGI("setTorchMode: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            mTorchStatus = TorchModeStatus::NOT_AVAILABLE;
            returnStatus = device3_2->setTorchMode(TorchMode::ON);
            ASSERT_TRUE(returnStatus.isOk());
            if (!torchControlSupported) {
                ASSERT_EQ(Status::METHOD_NOT_SUPPORTED, returnStatus);
            } else {
                ASSERT_TRUE(returnStatus == Status::OK ||
                            returnStatus == Status::OPERATION_NOT_SUPPORTED);
                if (returnStatus == Status::OK) {
                    {
                        std::unique_lock<std::mutex> l(mTorchLock);
                        while (TorchModeStatus::NOT_AVAILABLE == mTorchStatus) {
                            auto timeout = std::chrono::system_clock::now() +
                                    std::chrono::seconds(kTorchTimeoutSec);
                            ASSERT_NE(std::cv_status::timeout,
                                    mTorchCond.wait_until(l, timeout));
                        }
                        ASSERT_EQ(TorchModeStatus::AVAILABLE_ON, mTorchStatus);
                        mTorchStatus = TorchModeStatus::NOT_AVAILABLE;
                    }

                    returnStatus = device3_2->setTorchMode(TorchMode::OFF);
                    ASSERT_TRUE(returnStatus.isOk());
                    ASSERT_EQ(Status::OK, returnStatus);

                    {
                        std::unique_lock<std::mutex> l(mTorchLock);
                        while (TorchModeStatus::NOT_AVAILABLE == mTorchStatus) {
                            auto timeout = std::chrono::system_clock::now() +
                                    std::chrono::seconds(kTorchTimeoutSec);
                            ASSERT_NE(std::cv_status::timeout,
                                    mTorchCond.wait_until(l, timeout));
                        }
                        ASSERT_EQ(TorchModeStatus::AVAILABLE_OFF, mTorchStatus);
                    }
                }
            }
        } else if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            ::android::sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            ALOGI("dumpState: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device1 = device;
                });
            ASSERT_TRUE(ret.isOk());

            mTorchStatus = TorchModeStatus::NOT_AVAILABLE;
            returnStatus = device1->setTorchMode(TorchMode::ON);
            ASSERT_TRUE(returnStatus.isOk());
            if (!torchControlSupported) {
                ASSERT_EQ(Status::METHOD_NOT_SUPPORTED, returnStatus);
            } else {
                ASSERT_TRUE(returnStatus == Status::OK ||
                            returnStatus == Status::OPERATION_NOT_SUPPORTED);
                if (returnStatus == Status::OK) {
                    {
                        std::unique_lock<std::mutex> l(mTorchLock);
                        while (TorchModeStatus::NOT_AVAILABLE == mTorchStatus) {
                            auto timeout = std::chrono::system_clock::now() +
                                    std::chrono::seconds(kTorchTimeoutSec);
                            ASSERT_NE(std::cv_status::timeout,
                                    mTorchCond.wait_until(l, timeout));
                        }
                        ASSERT_EQ(TorchModeStatus::AVAILABLE_ON, mTorchStatus);
                        mTorchStatus = TorchModeStatus::NOT_AVAILABLE;
                    }

                    returnStatus = device1->setTorchMode(TorchMode::OFF);
                    ASSERT_TRUE(returnStatus.isOk());
                    ASSERT_EQ(Status::OK, returnStatus);

                    {
                        std::unique_lock<std::mutex> l(mTorchLock);
                        while (TorchModeStatus::NOT_AVAILABLE == mTorchStatus) {
                            auto timeout = std::chrono::system_clock::now() +
                                    std::chrono::seconds(kTorchTimeoutSec);
                            ASSERT_NE(std::cv_status::timeout,
                                    mTorchCond.wait_until(l, timeout));
                        }
                        ASSERT_EQ(TorchModeStatus::AVAILABLE_OFF, mTorchStatus);
                    }
                }
            }
            ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }

    returnStatus = env->mProvider->setCallback(nullptr);
    ASSERT_TRUE(returnStatus.isOk());
    ASSERT_EQ(Status::OK, returnStatus);
}

// Check dump functionality.
TEST_F(CameraHidlTest, dumpState) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    Return<void> ret;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            ALOGI("dumpState: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            native_handle_t* raw_handle = native_handle_create(1, 0);
            raw_handle->data[0] = open(kDumpOutput, O_RDWR);
            ASSERT_GE(raw_handle->data[0], 0);
            hidl_handle handle = raw_handle;
            ret= device3_2->dumpState(handle);
            ASSERT_TRUE(ret.isOk());
            close(raw_handle->data[0]);
            native_handle_delete(raw_handle);
        } else if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            ::android::sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            ALOGI("dumpState: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V1_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V1_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device1 = device;
                });
            ASSERT_TRUE(ret.isOk());

            native_handle_t* raw_handle = native_handle_create(1, 0);
            raw_handle->data[0] = open(kDumpOutput, O_RDWR);
            ASSERT_GE(raw_handle->data[0], 0);
            hidl_handle handle = raw_handle;
            Return<Status> returnStatus = device1->dumpState(handle);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);
            close(raw_handle->data[0]);
            native_handle_delete(raw_handle);
        }
    }
}

// Open, dumpStates, then close
TEST_F(CameraHidlTest, openClose) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    Return<void> ret;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            ALOGI("openClose: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            sp<EmptyDeviceCb> cb = new EmptyDeviceCb;
            sp<ICameraDeviceSession> session;
            ret = device3_2->open(
                cb,
                [&](auto status, const auto& newSession) {
                    ALOGI("device::open returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(newSession, nullptr);
                    session = newSession;
                });
            ASSERT_TRUE(ret.isOk());

            native_handle_t* raw_handle = native_handle_create(1, 0);
            raw_handle->data[0] = open(kDumpOutput, O_RDWR);
            ASSERT_GE(raw_handle->data[0], 0);
            hidl_handle handle = raw_handle;
            ret = device3_2->dumpState(handle);
            ASSERT_TRUE(ret.isOk());
            close(raw_handle->data[0]);
            native_handle_delete(raw_handle);

            ret = session->close();
            ASSERT_TRUE(ret.isOk());
            // TODO: test all session API calls return INTERNAL_ERROR after close
            // TODO: keep a wp copy here and verify session cannot be promoted out of this scope
        } else if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_1_0) {
            sp<::android::hardware::camera::device::V1_0::ICameraDevice> device1;
            openCameraDevice(name, env, &device1 /*out*/);
            ASSERT_NE(nullptr, device1.get());

            native_handle_t* raw_handle = native_handle_create(1, 0);
            raw_handle->data[0] = open(kDumpOutput, O_RDWR);
            ASSERT_GE(raw_handle->data[0], 0);
            hidl_handle handle = raw_handle;
            Return<Status> returnStatus = device1->dumpState(handle);
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);
            close(raw_handle->data[0]);
            native_handle_delete(raw_handle);

            ret = device1->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check whether all common default request settings can be sucessfully
// constructed.
TEST_F(CameraHidlTest, constructDefaultRequestSettings) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            ::android::sp<::android::hardware::camera::device::V3_2::ICameraDevice> device3_2;
            Return<void> ret;
            ALOGI("constructDefaultRequestSettings: Testing camera device %s", name.c_str());
            ret = env->mProvider->getCameraDeviceInterface_V3_x(
                name,
                [&](auto status, const auto& device) {
                    ALOGI("getCameraDeviceInterface_V3_x returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(device, nullptr);
                    device3_2 = device;
                });
            ASSERT_TRUE(ret.isOk());

            sp<EmptyDeviceCb> cb = new EmptyDeviceCb;
            sp<ICameraDeviceSession> session;
            ret = device3_2->open(
                cb,
                [&](auto status, const auto& newSession) {
                    ALOGI("device::open returns status:%d", (int)status);
                    ASSERT_EQ(Status::OK, status);
                    ASSERT_NE(newSession, nullptr);
                    session = newSession;
                });
            ASSERT_TRUE(ret.isOk());

            for (uint32_t t = (uint32_t) RequestTemplate::PREVIEW;
                    t <= (uint32_t) RequestTemplate::MANUAL; t++) {
                RequestTemplate reqTemplate = (RequestTemplate) t;
                ret = session->constructDefaultRequestSettings(
                    reqTemplate,
                    [&](auto status, const auto& req) {
                        ALOGI("constructDefaultRequestSettings returns status:%d", (int)status);
                        if (reqTemplate == RequestTemplate::ZERO_SHUTTER_LAG ||
                                reqTemplate == RequestTemplate::MANUAL) {
                            // optional templates
                            ASSERT_TRUE(status == Status::OK || status == Status::ILLEGAL_ARGUMENT);
                        } else {
                            ASSERT_EQ(Status::OK, status);
                        }

                        if (status == Status::OK) {
                            const camera_metadata_t* metadata =
                                (camera_metadata_t*) req.data();
                            size_t expectedSize = req.size();
                            int result = validate_camera_metadata_structure(
                                    metadata, &expectedSize);
                            ASSERT_TRUE(result == 0 || result == CAMERA_METADATA_VALIDATION_SHIFTED);
                            size_t entryCount = get_camera_metadata_entry_count(metadata);
                            // TODO: we can do better than 0 here. Need to check how many required
                            // request keys we've defined for each template
                            ASSERT_GT(entryCount, 0u);
                            ALOGI("template %u metadata entry count is %zu", t, entryCount);
                        } else {
                            ASSERT_EQ(0u, req.size());
                        }
                    });
                ASSERT_TRUE(ret.isOk());
            }
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that all supported stream formats and sizes can be configured
// successfully.
TEST_F(CameraHidlTest, configureStreamsAvailableOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputStreams;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            outputStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputStreams));
            ASSERT_NE(0u, outputStreams.size());

            int32_t streamId = 0;
            for (auto &it : outputStreams) {
                Stream stream = {streamId, StreamType::OUTPUT,
                        static_cast<uint32_t> (it.width),
                        static_cast<uint32_t> (it.height),
                        static_cast<PixelFormat> (it.format), 0, 0,
                        StreamRotation::ROTATION_0};
                ::android::hardware::hidl_vec<Stream> streams = {stream};
                StreamConfiguration config = {streams,
                        StreamConfigurationMode::NORMAL_MODE};
                ret = session->configureStreams(config, [streamId] (Status s,
                        HalStreamConfiguration halConfig) {
                    ASSERT_EQ(Status::OK, s);
                    ASSERT_EQ(1u, halConfig.streams.size());
                    ASSERT_EQ(halConfig.streams[0].id, streamId);
                });
                ASSERT_TRUE(ret.isOk());
                streamId++;
            }

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check for correct handling of invalid/incorrect configuration parameters.
TEST_F(CameraHidlTest, configureStreamsInvalidOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputStreams;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            outputStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputStreams));
            ASSERT_NE(0u, outputStreams.size());

            int32_t streamId = 0;
            Stream stream = {streamId++, StreamType::OUTPUT,
                    static_cast<uint32_t> (0),
                    static_cast<uint32_t> (0),
                    static_cast<PixelFormat> (outputStreams[0].format),
                    0, 0, StreamRotation::ROTATION_0};
            ::android::hardware::hidl_vec<Stream> streams = {stream};
            StreamConfiguration config = {streams,
                    StreamConfigurationMode::NORMAL_MODE};
            ret = session->configureStreams(config, [] (Status s,
                    HalStreamConfiguration) {
                ASSERT_TRUE((Status::ILLEGAL_ARGUMENT == s) ||
                            (Status::INTERNAL_ERROR == s));
            });
            ASSERT_TRUE(ret.isOk());

            stream = {streamId++, StreamType::OUTPUT,
                    static_cast<uint32_t> (UINT32_MAX),
                    static_cast<uint32_t> (UINT32_MAX),
                    static_cast<PixelFormat> (outputStreams[0].format),
                    0, 0, StreamRotation::ROTATION_0};
            streams[0] = stream;
            config = {streams,
                    StreamConfigurationMode::NORMAL_MODE};
            ret = session->configureStreams(config, [] (Status s,
                    HalStreamConfiguration) {
                ASSERT_EQ(Status::ILLEGAL_ARGUMENT, s);
            });
            ASSERT_TRUE(ret.isOk());

            for (auto &it : outputStreams) {
                stream = {streamId++, StreamType::OUTPUT,
                        static_cast<uint32_t> (it.width),
                        static_cast<uint32_t> (it.height),
                        static_cast<PixelFormat> (UINT32_MAX),
                        0, 0, StreamRotation::ROTATION_0};
                streams[0] = stream;
                config = {streams,
                        StreamConfigurationMode::NORMAL_MODE};
                ret = session->configureStreams(config, [] (Status s,
                        HalStreamConfiguration) {
                    ASSERT_EQ(Status::ILLEGAL_ARGUMENT, s);
                });
                ASSERT_TRUE(ret.isOk());

                stream = {streamId++, StreamType::OUTPUT,
                        static_cast<uint32_t> (it.width),
                        static_cast<uint32_t> (it.height),
                        static_cast<PixelFormat> (it.format),
                        0, 0, static_cast<StreamRotation> (UINT32_MAX)};
                streams[0] = stream;
                config = {streams,
                        StreamConfigurationMode::NORMAL_MODE};
                ret = session->configureStreams(config, [] (Status s,
                        HalStreamConfiguration) {
                    ASSERT_EQ(Status::ILLEGAL_ARGUMENT, s);
                });
                ASSERT_TRUE(ret.isOk());
            }

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check whether all supported ZSL output stream combinations can be
// configured successfully.
TEST_F(CameraHidlTest, configureStreamsZSLInputOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> inputStreams;
    std::vector<AvailableZSLInputOutput> inputOutputMap;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            Status rc = isZSLModeAvailable(staticMeta);
            if (Status::METHOD_NOT_SUPPORTED == rc) {
                ret = session->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }
            ASSERT_EQ(Status::OK, rc);

            inputStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    inputStreams));
            ASSERT_NE(0u, inputStreams.size());

            inputOutputMap.clear();
            ASSERT_EQ(Status::OK, getZSLInputOutputMap(staticMeta,
                    inputOutputMap));
            ASSERT_NE(0u, inputOutputMap.size());

            int32_t streamId = 0;
            for (auto &inputIter : inputOutputMap) {
                AvailableStream input;
                ASSERT_EQ(Status::OK,
                        findLargestSize(inputStreams, inputIter.inputFormat, input));
                ASSERT_NE(0u, inputStreams.size());

                AvailableStream outputThreshold = {INT32_MAX, INT32_MAX,
                        inputIter.outputFormat};
                std::vector<AvailableStream> outputStreams;
                ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                        outputStreams, &outputThreshold));
                for (auto &outputIter : outputStreams) {
                    Stream zslStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (input.width),
                            static_cast<uint32_t> (input.height),
                            static_cast<PixelFormat> (input.format),
                            GRALLOC_USAGE_HW_CAMERA_ZSL, 0,
                            StreamRotation::ROTATION_0};
                    Stream inputStream = {streamId++, StreamType::INPUT,
                            static_cast<uint32_t> (input.width),
                            static_cast<uint32_t> (input.height),
                            static_cast<PixelFormat> (input.format), 0, 0,
                            StreamRotation::ROTATION_0};
                    Stream outputStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (outputIter.width),
                            static_cast<uint32_t> (outputIter.height),
                            static_cast<PixelFormat> (outputIter.format), 0, 0,
                            StreamRotation::ROTATION_0};

                    ::android::hardware::hidl_vec<Stream> streams = {
                            inputStream, zslStream, outputStream};
                    StreamConfiguration config = {streams,
                            StreamConfigurationMode::NORMAL_MODE};
                    ret = session->configureStreams(config, [streamId] (Status s,
                            HalStreamConfiguration halConfig) {
                        ASSERT_EQ(Status::OK, s);
                        ASSERT_EQ(3u, halConfig.streams.size());
                    });
                    ASSERT_TRUE(ret.isOk());
                }
            }

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that all supported preview + still capture stream combinations
// can be configured successfully.
TEST_F(CameraHidlTest, configureStreamsPreviewStillOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputBlobStreams;
    std::vector<AvailableStream> outputPreviewStreams;
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    AvailableStream blobThreshold = {INT32_MAX, INT32_MAX,
            static_cast<int32_t>(PixelFormat::BLOB)};

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            outputBlobStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputBlobStreams, &blobThreshold));
            ASSERT_NE(0u, outputBlobStreams.size());

            outputPreviewStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputPreviewStreams, &previewThreshold));
            ASSERT_NE(0u, outputPreviewStreams.size());

            int32_t streamId = 0;
            for (auto &blobIter : outputBlobStreams) {
                for (auto &previewIter : outputPreviewStreams) {
                    Stream previewStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (previewIter.width),
                            static_cast<uint32_t> (previewIter.height),
                            static_cast<PixelFormat> (previewIter.format), 0, 0,
                            StreamRotation::ROTATION_0};
                    Stream blobStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (blobIter.width),
                            static_cast<uint32_t> (blobIter.height),
                            static_cast<PixelFormat> (blobIter.format), 0, 0,
                            StreamRotation::ROTATION_0};
                    ::android::hardware::hidl_vec<Stream> streams = {
                            previewStream, blobStream};
                    StreamConfiguration config = {streams,
                            StreamConfigurationMode::NORMAL_MODE};
                    ret = session->configureStreams(config, [streamId] (Status s,
                            HalStreamConfiguration halConfig) {
                        ASSERT_EQ(Status::OK, s);
                        ASSERT_EQ(2u, halConfig.streams.size());
                    });
                    ASSERT_TRUE(ret.isOk());
                }
            }

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// In case constrained mode is supported, test whether it can be
// configured. Additionally check for common invalid inputs when
// using this mode.
TEST_F(CameraHidlTest, configureStreamsConstrainedOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            Status rc = isConstrainedModeAvailable(staticMeta);
            if (Status::METHOD_NOT_SUPPORTED == rc) {
                ret = session->close();
                ASSERT_TRUE(ret.isOk());
                continue;
            }
            ASSERT_EQ(Status::OK, rc);

            AvailableStream hfrStream;
            rc = pickConstrainedModeSize(staticMeta, hfrStream);
            ASSERT_EQ(Status::OK, rc);

            int32_t streamId = 0;
            Stream stream = {streamId, StreamType::OUTPUT,
                    static_cast<uint32_t> (hfrStream.width),
                    static_cast<uint32_t> (hfrStream.height),
                    static_cast<PixelFormat> (hfrStream.format), 0, 0,
                    StreamRotation::ROTATION_0};
            ::android::hardware::hidl_vec<Stream> streams = {stream};
            StreamConfiguration config = {streams,
                    StreamConfigurationMode::CONSTRAINED_HIGH_SPEED_MODE};
            ret = session->configureStreams(config, [streamId] (Status s,
                    HalStreamConfiguration halConfig) {
                ASSERT_EQ(Status::OK, s);
                ASSERT_EQ(1u, halConfig.streams.size());
                ASSERT_EQ(halConfig.streams[0].id, streamId);
            });
            ASSERT_TRUE(ret.isOk());

            stream = {streamId++, StreamType::OUTPUT,
                    static_cast<uint32_t> (0),
                    static_cast<uint32_t> (0),
                    static_cast<PixelFormat> (hfrStream.format), 0, 0,
                    StreamRotation::ROTATION_0};
            streams[0] = stream;
            config = {streams,
                    StreamConfigurationMode::CONSTRAINED_HIGH_SPEED_MODE};
            ret = session->configureStreams(config, [streamId] (Status s,
                    HalStreamConfiguration) {
                ASSERT_TRUE((Status::ILLEGAL_ARGUMENT == s) ||
                            (Status::INTERNAL_ERROR == s));
            });
            ASSERT_TRUE(ret.isOk());

            stream = {streamId++, StreamType::OUTPUT,
                    static_cast<uint32_t> (UINT32_MAX),
                    static_cast<uint32_t> (UINT32_MAX),
                    static_cast<PixelFormat> (hfrStream.format), 0, 0,
                    StreamRotation::ROTATION_0};
            streams[0] = stream;
            config = {streams,
                    StreamConfigurationMode::CONSTRAINED_HIGH_SPEED_MODE};
            ret = session->configureStreams(config, [streamId] (Status s,
                    HalStreamConfiguration) {
                ASSERT_EQ(Status::ILLEGAL_ARGUMENT, s);
            });
            ASSERT_TRUE(ret.isOk());

            stream = {streamId++, StreamType::OUTPUT,
                    static_cast<uint32_t> (hfrStream.width),
                    static_cast<uint32_t> (hfrStream.height),
                    static_cast<PixelFormat> (UINT32_MAX), 0, 0,
                    StreamRotation::ROTATION_0};
            streams[0] = stream;
            config = {streams,
                    StreamConfigurationMode::CONSTRAINED_HIGH_SPEED_MODE};
            ret = session->configureStreams(config, [streamId] (Status s,
                    HalStreamConfiguration) {
                ASSERT_EQ(Status::ILLEGAL_ARGUMENT, s);
            });
            ASSERT_TRUE(ret.isOk());

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that all supported video + snapshot stream combinations can
// be configured successfully.
TEST_F(CameraHidlTest, configureStreamsVideoStillOutputs) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputBlobStreams;
    std::vector<AvailableStream> outputVideoStreams;
    AvailableStream videoThreshold = {kMaxVideoWidth, kMaxVideoHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    AvailableStream blobThreshold = {kMaxVideoWidth, kMaxVideoHeight,
            static_cast<int32_t>(PixelFormat::BLOB)};

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            camera_metadata_t *staticMeta;
            Return<void> ret;
            sp<ICameraDeviceSession> session;
            openEmptyDeviceSession(name, env, &session /*out*/,
                    &staticMeta /*out*/);

            outputBlobStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputBlobStreams, &blobThreshold));
            ASSERT_NE(0u, outputBlobStreams.size());

            outputVideoStreams.clear();
            ASSERT_EQ(Status::OK, getAvailableOutputStreams(staticMeta,
                    outputVideoStreams, &videoThreshold));
            ASSERT_NE(0u, outputVideoStreams.size());

            int32_t streamId = 0;
            for (auto &blobIter : outputBlobStreams) {
                for (auto &videoIter : outputVideoStreams) {
                    Stream videoStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (videoIter.width),
                            static_cast<uint32_t> (videoIter.height),
                            static_cast<PixelFormat> (videoIter.format), 0, 0,
                            StreamRotation::ROTATION_0};
                    Stream blobStream = {streamId++, StreamType::OUTPUT,
                            static_cast<uint32_t> (blobIter.width),
                            static_cast<uint32_t> (blobIter.height),
                            static_cast<PixelFormat> (blobIter.format),
                            GRALLOC_USAGE_HW_VIDEO_ENCODER, 0,
                            StreamRotation::ROTATION_0};
                    ::android::hardware::hidl_vec<Stream> streams = {
                            videoStream, blobStream};
                    StreamConfiguration config = {streams,
                            StreamConfigurationMode::NORMAL_MODE};
                    ret = session->configureStreams(config, [streamId] (Status s,
                            HalStreamConfiguration halConfig) {
                        ASSERT_EQ(Status::OK, s);
                        ASSERT_EQ(2u, halConfig.streams.size());
                    });
                    ASSERT_TRUE(ret.isOk());
                }
            }

            free_camera_metadata(staticMeta);
            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Generate and verify a camera capture request
TEST_F(CameraHidlTest, processCaptureRequestPreview) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    uint64_t bufferId = 1;
    uint32_t frameNumber = 1;
    ::android::hardware::hidl_vec<uint8_t> settings;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Stream previewStream;
            HalStreamConfiguration halStreamConfig;
            sp<ICameraDeviceSession> session;
            configurePreviewStream(name, env, &previewThreshold,
                    &session /*out*/, &previewStream /*out*/,
                    &halStreamConfig /*out*/);

            RequestTemplate reqTemplate = RequestTemplate::PREVIEW;
            Return<void> ret;
            ret = session->constructDefaultRequestSettings(reqTemplate,
                [&](auto status, const auto& req) {
                    ASSERT_EQ(Status::OK, status);
                    settings = req; });
            ASSERT_TRUE(ret.isOk());

            sp<GraphicBuffer> gb = new GraphicBuffer(
                previewStream.width, previewStream.height,
                static_cast<int32_t>(halStreamConfig.streams[0].overrideFormat),
                1, android_convertGralloc1To0Usage(
                       halStreamConfig.streams[0].producerUsage,
                       halStreamConfig.streams[0].consumerUsage));
            ASSERT_NE(nullptr, gb.get());
            StreamBuffer outputBuffer = {halStreamConfig.streams[0].id,
                    bufferId, hidl_handle(gb->getNativeBuffer()->handle),
                    BufferStatus::OK, nullptr, nullptr};
            ::android::hardware::hidl_vec<StreamBuffer> outputBuffers = {
                    outputBuffer};
            StreamBuffer emptyInputBuffer = {-1, 0, nullptr,
                    BufferStatus::ERROR, nullptr, nullptr};
            CaptureRequest request = {frameNumber, 0 /* fmqSettingsSize */, settings,
                    emptyInputBuffer, outputBuffers};

            {
                std::unique_lock<std::mutex> l(mLock);
                mResultBuffers.clear();
                mResultFrameNumber = frameNumber;
            }

            Status status = Status::INTERNAL_ERROR;
            uint32_t numRequestProcessed = 0;
            hidl_vec<BufferCache> cachesToRemove;
            Return<void> returnStatus = session->processCaptureRequest(
                    {request},
                    cachesToRemove,
                    [&status, &numRequestProcessed] (auto s, uint32_t n) {
                        status = s;
                        numRequestProcessed = n;
                    });
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, status);
            ASSERT_EQ(numRequestProcessed, 1u);

            {
                std::unique_lock<std::mutex> l(mLock);
                while (0 == mResultBuffers.size()) {
                    auto timeout = std::chrono::system_clock::now() +
                            std::chrono::seconds(kStreamBufferTimeoutSec);
                    ASSERT_NE(std::cv_status::timeout,
                            mResultCondition.wait_until(l, timeout));
                }

                ASSERT_EQ(BufferStatus::OK, mResultBuffers[0].status);
                ASSERT_EQ(previewStream.id, mResultBuffers[0].streamId);

                request.frameNumber++;
                //Empty settings should be supported after the first call
                //for repeating requests.
                request.settings.setToExternal(nullptr, 0, true);
                mResultBuffers.clear();
                mResultFrameNumber++;
            }

            returnStatus = session->processCaptureRequest(
                    {request},
                    cachesToRemove,
                    [&status, &numRequestProcessed] (auto s, uint32_t n) {
                        status = s;
                        numRequestProcessed = n;
                    });
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, status);
            ASSERT_EQ(numRequestProcessed, 1u);

            {
                std::unique_lock<std::mutex> l(mLock);
                while (0 == mResultBuffers.size()) {
                    auto timeout = std::chrono::system_clock::now() +
                            std::chrono::seconds(kStreamBufferTimeoutSec);
                    ASSERT_NE(std::cv_status::timeout,
                            mResultCondition.wait_until(l, timeout));
                }
                ASSERT_EQ(BufferStatus::OK, mResultBuffers[0].status);
                ASSERT_EQ(previewStream.id, mResultBuffers[0].streamId);
            }

            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Test whether an incorrect capture request with missing settings will
// be reported correctly.
TEST_F(CameraHidlTest, processCaptureRequestInvalidSinglePreview) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputPreviewStreams;
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    uint64_t bufferId = 1;
    uint32_t frameNumber = 1;
    ::android::hardware::hidl_vec<uint8_t> settings;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Stream previewStream;
            HalStreamConfiguration halStreamConfig;
            sp<ICameraDeviceSession> session;
            configurePreviewStream(name, env, &previewThreshold,
                    &session /*out*/, &previewStream /*out*/,
                    &halStreamConfig /*out*/);

            sp<GraphicBuffer> gb = new GraphicBuffer(
                previewStream.width, previewStream.height,
                static_cast<int32_t>(halStreamConfig.streams[0].overrideFormat),
                1, android_convertGralloc1To0Usage(
                       halStreamConfig.streams[0].producerUsage,
                       halStreamConfig.streams[0].consumerUsage));

            StreamBuffer outputBuffer = {halStreamConfig.streams[0].id,
                    bufferId, hidl_handle(gb->getNativeBuffer()->handle),
                    BufferStatus::OK, nullptr, nullptr};
            ::android::hardware::hidl_vec<StreamBuffer> outputBuffers = {
                    outputBuffer};
            StreamBuffer emptyInputBuffer = {-1, 0, nullptr,
                    BufferStatus::ERROR, nullptr, nullptr};
            CaptureRequest request = {frameNumber, 0 /* fmqSettingsSize */, settings,
                    emptyInputBuffer, outputBuffers};

            //Settings were not correctly initialized, we should fail here
            Status status = Status::OK;
            uint32_t numRequestProcessed = 0;
            hidl_vec<BufferCache> cachesToRemove;
            Return<void> ret = session->processCaptureRequest(
                    {request},
                    cachesToRemove,
                    [&status, &numRequestProcessed] (auto s, uint32_t n) {
                        status = s;
                        numRequestProcessed = n;
                    });
            ASSERT_TRUE(ret.isOk());
            ASSERT_EQ(Status::INTERNAL_ERROR, status);
            ASSERT_EQ(numRequestProcessed, 0u);

            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Check whether an invalid capture request with missing output buffers
// will be reported correctly.
TEST_F(CameraHidlTest, processCaptureRequestInvalidBuffer) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputBlobStreams;
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    uint32_t frameNumber = 1;
    ::android::hardware::hidl_vec<uint8_t> settings;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Stream previewStream;
            HalStreamConfiguration halStreamConfig;
            sp<ICameraDeviceSession> session;
            configurePreviewStream(name, env, &previewThreshold,
                    &session /*out*/, &previewStream /*out*/,
                    &halStreamConfig /*out*/);

            RequestTemplate reqTemplate = RequestTemplate::PREVIEW;
            Return<void> ret;
            ret = session->constructDefaultRequestSettings(reqTemplate,
                [&](auto status, const auto& req) {
                    ASSERT_EQ(Status::OK, status);
                    settings = req; });
            ASSERT_TRUE(ret.isOk());

            ::android::hardware::hidl_vec<StreamBuffer> emptyOutputBuffers;
            StreamBuffer emptyInputBuffer = {-1, 0, nullptr,
                    BufferStatus::ERROR, nullptr, nullptr};
            CaptureRequest request = {frameNumber, 0/* fmqSettingsSize */, settings,
                    emptyInputBuffer, emptyOutputBuffers};

            //Output buffers are missing, we should fail here
            Status status = Status::OK;
            uint32_t numRequestProcessed = 0;
            hidl_vec<BufferCache> cachesToRemove;
            ret = session->processCaptureRequest(
                    {request},
                    cachesToRemove,
                    [&status, &numRequestProcessed] (auto s, uint32_t n) {
                        status = s;
                        numRequestProcessed = n;
                    });
            ASSERT_TRUE(ret.isOk());
            ASSERT_EQ(Status::INTERNAL_ERROR, status);
            ASSERT_EQ(numRequestProcessed, 0u);

            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Generate, trigger and flush a preview request
TEST_F(CameraHidlTest, flushPreviewRequest) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputPreviewStreams;
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    uint64_t bufferId = 1;
    uint32_t frameNumber = 1;
    ::android::hardware::hidl_vec<uint8_t> settings;

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Stream previewStream;
            HalStreamConfiguration halStreamConfig;
            sp<ICameraDeviceSession> session;
            configurePreviewStream(name, env, &previewThreshold,
                    &session /*out*/, &previewStream /*out*/,
                    &halStreamConfig /*out*/);

            RequestTemplate reqTemplate = RequestTemplate::PREVIEW;
            Return<void> ret;
            ret = session->constructDefaultRequestSettings(reqTemplate,
                [&](auto status, const auto& req) {
                    ASSERT_EQ(Status::OK, status);
                    settings = req; });
            ASSERT_TRUE(ret.isOk());

            sp<GraphicBuffer> gb = new GraphicBuffer(
                previewStream.width, previewStream.height,
                static_cast<int32_t>(halStreamConfig.streams[0].overrideFormat),
                1, android_convertGralloc1To0Usage(
                       halStreamConfig.streams[0].producerUsage,
                       halStreamConfig.streams[0].consumerUsage));
            ASSERT_NE(nullptr, gb.get());
            StreamBuffer outputBuffer = {halStreamConfig.streams[0].id,
                    bufferId, hidl_handle(gb->getNativeBuffer()->handle),
                    BufferStatus::OK, nullptr, nullptr};
            ::android::hardware::hidl_vec<StreamBuffer> outputBuffers = {
                    outputBuffer};
            const StreamBuffer emptyInputBuffer = {-1, 0, nullptr,
                    BufferStatus::ERROR, nullptr, nullptr};
            CaptureRequest request = {frameNumber, 0 /* fmqSettingsSize */, settings,
                    emptyInputBuffer, outputBuffers};

            {
                std::unique_lock<std::mutex> l(mLock);
                mResultBuffers.clear();
                mErrors.clear();
                mResultFrameNumber = frameNumber;
            }

            Status status = Status::INTERNAL_ERROR;
            uint32_t numRequestProcessed = 0;
            hidl_vec<BufferCache> cachesToRemove;
            ret = session->processCaptureRequest(
                    {request},
                    cachesToRemove,
                    [&status, &numRequestProcessed] (auto s, uint32_t n) {
                        status = s;
                        numRequestProcessed = n;
                    });

            ASSERT_TRUE(ret.isOk());
            ASSERT_EQ(Status::OK, status);
            ASSERT_EQ(numRequestProcessed, 1u);
            //Flush before waiting for request to complete.
            Return<Status> returnStatus = session->flush();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            {
                std::unique_lock<std::mutex> l(mLock);
                while ((0 == mResultBuffers.size()) && (0 == mErrors.size())) {
                    auto timeout = std::chrono::system_clock::now() +
                            std::chrono::seconds(kStreamBufferTimeoutSec);
                    ASSERT_NE(std::cv_status::timeout,
                            mResultCondition.wait_until(l, timeout));
                }

                if (mErrors.empty()) {
                    ASSERT_EQ(BufferStatus::OK, mResultBuffers[0].status);
                    ASSERT_EQ(previewStream.id, mResultBuffers[0].streamId);
                } else {
                    for (auto &error : mErrors) {
                        switch (error.errorCode) {
                            case ErrorCode::ERROR_REQUEST:
                            case ErrorCode::ERROR_RESULT:
                                //Expected
                                break;
                            case ErrorCode::ERROR_BUFFER:
                                //Expected as well
                                ASSERT_EQ(frameNumber, error.frameNumber);
                                ASSERT_EQ(previewStream.id, error.errorStreamId);
                                break;
                            case ErrorCode::ERROR_DEVICE:
                            default:
                                FAIL() <<"Unexpected error:" << static_cast<uint32_t> (error.errorCode);
                        }
                    }
                }
            }

            ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Verify that camera flushes correctly without any pending requests.
TEST_F(CameraHidlTest, flushEmpty) {
    CameraHidlEnvironment* env = CameraHidlEnvironment::Instance();
    hidl_vec<hidl_string> cameraDeviceNames = getCameraDeviceNames();
    std::vector<AvailableStream> outputPreviewStreams;
    AvailableStream previewThreshold = {kMaxPreviewWidth, kMaxPreviewHeight,
            static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};

    for (const auto& name : cameraDeviceNames) {
        if (getCameraDeviceVersion(name) == CAMERA_DEVICE_API_VERSION_3_2) {
            Stream previewStream;
            HalStreamConfiguration halStreamConfig;
            sp<ICameraDeviceSession> session;
            configurePreviewStream(name, env, &previewThreshold,
                    &session /*out*/, &previewStream /*out*/,
                    &halStreamConfig /*out*/);

            {
                std::unique_lock<std::mutex> l(mLock);
                mResultBuffers.clear();
                mErrors.clear();
                mResultFrameNumber = 0;
            }

            Return<Status> returnStatus = session->flush();
            ASSERT_TRUE(returnStatus.isOk());
            ASSERT_EQ(Status::OK, returnStatus);

            {
                std::unique_lock<std::mutex> l(mLock);
                auto timeout = std::chrono::system_clock::now() +
                        std::chrono::milliseconds(kEmptyFlushTimeoutMSec);
                ASSERT_EQ(std::cv_status::timeout,
                        mResultCondition.wait_until(l, timeout));
                ASSERT_TRUE(mErrors.empty());
                ASSERT_TRUE(mResultBuffers.empty());
            }

            Return<void> ret = session->close();
            ASSERT_TRUE(ret.isOk());
        }
    }
}

// Retrieve all valid output stream resolutions from the camera
// static characteristics.
Status CameraHidlTest::getAvailableOutputStreams(camera_metadata_t *staticMeta,
        std::vector<AvailableStream> &outputStreams,
        const AvailableStream *threshold) {
    if (nullptr == staticMeta) {
        return Status::ILLEGAL_ARGUMENT;
    }

    camera_metadata_ro_entry entry;
    int rc = find_camera_metadata_ro_entry(staticMeta,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);
    if ((0 != rc) || (0 != (entry.count % 4))) {
        return Status::ILLEGAL_ARGUMENT;
    }

    for (size_t i = 0; i < entry.count; i+=4) {
        if (ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT ==
                entry.data.i32[i + 3]) {
            if(nullptr == threshold) {
                AvailableStream s = {entry.data.i32[i+1],
                        entry.data.i32[i+2], entry.data.i32[i]};
                outputStreams.push_back(s);
            } else {
                if ((threshold->format == entry.data.i32[i]) &&
                        (threshold->width >= entry.data.i32[i+1]) &&
                        (threshold->height >= entry.data.i32[i+2])) {
                    AvailableStream s = {entry.data.i32[i+1],
                            entry.data.i32[i+2], threshold->format};
                    outputStreams.push_back(s);
                }
            }
        }

    }

    return Status::OK;
}

// Check if constrained mode is supported by using the static
// camera characteristics.
Status CameraHidlTest::isConstrainedModeAvailable(camera_metadata_t *staticMeta) {
    Status ret = Status::METHOD_NOT_SUPPORTED;
    if (nullptr == staticMeta) {
        return Status::ILLEGAL_ARGUMENT;
    }

    camera_metadata_ro_entry entry;
    int rc = find_camera_metadata_ro_entry(staticMeta,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if (0 != rc) {
        return Status::ILLEGAL_ARGUMENT;
    }

    for (size_t i = 0; i < entry.count; i++) {
        if (ANDROID_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO ==
                entry.data.u8[i]) {
            ret = Status::OK;
            break;
        }
    }

    return ret;
}

// Pick the largest supported HFR mode from the static camera
// characteristics.
Status CameraHidlTest::pickConstrainedModeSize(camera_metadata_t *staticMeta,
        AvailableStream &hfrStream) {
    if (nullptr == staticMeta) {
        return Status::ILLEGAL_ARGUMENT;
    }

    camera_metadata_ro_entry entry;
    int rc = find_camera_metadata_ro_entry(staticMeta,
            ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS, &entry);
    if (0 != rc) {
        return Status::METHOD_NOT_SUPPORTED;
    } else if (0 != (entry.count % 5)) {
        return Status::ILLEGAL_ARGUMENT;
    }

    hfrStream = {0, 0,
            static_cast<uint32_t>(PixelFormat::IMPLEMENTATION_DEFINED)};
    for (size_t i = 0; i < entry.count; i+=5) {
        int32_t w = entry.data.i32[i];
        int32_t h = entry.data.i32[i+1];
        if ((hfrStream.width * hfrStream.height) < (w *h)) {
            hfrStream.width = w;
            hfrStream.height = h;
        }
    }

    return Status::OK;
}

// Check whether ZSL is available using the static camera
// characteristics.
Status CameraHidlTest::isZSLModeAvailable(camera_metadata_t *staticMeta) {
    Status ret = Status::METHOD_NOT_SUPPORTED;
    if (nullptr == staticMeta) {
        return Status::ILLEGAL_ARGUMENT;
    }

    camera_metadata_ro_entry entry;
    int rc = find_camera_metadata_ro_entry(staticMeta,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &entry);
    if (0 != rc) {
        return Status::ILLEGAL_ARGUMENT;
    }

    for (size_t i = 0; i < entry.count; i++) {
        if ((ANDROID_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING ==
                entry.data.u8[i]) ||
                (ANDROID_REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING ==
                        entry.data.u8[i]) ){
            ret = Status::OK;
            break;
        }
    }

    return ret;
}

// Retrieve the reprocess input-output format map from the static
// camera characteristics.
Status CameraHidlTest::getZSLInputOutputMap(camera_metadata_t *staticMeta,
        std::vector<AvailableZSLInputOutput> &inputOutputMap) {
    if (nullptr == staticMeta) {
        return Status::ILLEGAL_ARGUMENT;
    }

    camera_metadata_ro_entry entry;
    int rc = find_camera_metadata_ro_entry(staticMeta,
            ANDROID_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP, &entry);
    if ((0 != rc) || (0 >= entry.count)) {
        return Status::ILLEGAL_ARGUMENT;
    }

    const int32_t* contents = &entry.data.i32[0];
    for (size_t i = 0; i < entry.count; ) {
        int32_t inputFormat = contents[i++];
        int32_t length = contents[i++];
        for (int32_t j = 0; j < length; j++) {
            int32_t outputFormat = contents[i+j];
            AvailableZSLInputOutput zslEntry = {inputFormat, outputFormat};
            inputOutputMap.push_back(zslEntry);
        }
        i += length;
    }

    return Status::OK;
}

// Search for the largest stream size for a given format.
Status CameraHidlTest::findLargestSize(
        const std::vector<AvailableStream> &streamSizes, int32_t format,
        AvailableStream &result) {
    result = {0, 0, 0};
    for (auto &iter : streamSizes) {
        if (format == iter.format) {
            if ((result.width * result.height) < (iter.width * iter.height)) {
                result = iter;
            }
        }
    }

    return (result.format == format) ? Status::OK : Status::ILLEGAL_ARGUMENT;
}

// Check whether the camera device supports specific focus mode.
Status CameraHidlTest::isAutoFocusModeAvailable(
        ::android::CameraParameters &cameraParams,
        const char *mode) {
    ::android::String8 focusModes(cameraParams.get(
            CameraParameters::KEY_SUPPORTED_FOCUS_MODES));
    if (focusModes.contains(mode)) {
        return Status::OK;
    }

    return Status::METHOD_NOT_SUPPORTED;
}

// Open a device session and configure a preview stream.
void CameraHidlTest::configurePreviewStream(const std::string &name,
        const CameraHidlEnvironment* env,
        const AvailableStream *previewThreshold,
        sp<ICameraDeviceSession> *session /*out*/,
        Stream *previewStream /*out*/,
        HalStreamConfiguration *halStreamConfig /*out*/) {
    ASSERT_NE(nullptr, env);
    ASSERT_NE(nullptr, session);
    ASSERT_NE(nullptr, previewStream);
    ASSERT_NE(nullptr, halStreamConfig);

    std::vector<AvailableStream> outputPreviewStreams;
    ::android::sp<ICameraDevice> device3_2;
    ALOGI("configureStreams: Testing camera device %s", name.c_str());
    Return<void> ret;
    ret = env->mProvider->getCameraDeviceInterface_V3_x(
        name,
        [&](auto status, const auto& device) {
            ALOGI("getCameraDeviceInterface_V3_x returns status:%d",
                  (int)status);
            ASSERT_EQ(Status::OK, status);
            ASSERT_NE(device, nullptr);
            device3_2 = device;
        });
    ASSERT_TRUE(ret.isOk());

    sp<DeviceCb> cb = new DeviceCb(this);
    ret = device3_2->open(
        cb,
        [&](auto status, const auto& newSession) {
            ALOGI("device::open returns status:%d", (int)status);
            ASSERT_EQ(Status::OK, status);
            ASSERT_NE(newSession, nullptr);
            *session = newSession;
        });
    ASSERT_TRUE(ret.isOk());

    camera_metadata_t *staticMeta;
    ret = device3_2->getCameraCharacteristics([&] (Status s,
            CameraMetadata metadata) {
        ASSERT_EQ(Status::OK, s);
        staticMeta = clone_camera_metadata(
                reinterpret_cast<const camera_metadata_t*>(metadata.data()));
         ASSERT_NE(nullptr, staticMeta);
    });
    ASSERT_TRUE(ret.isOk());

    outputPreviewStreams.clear();
    auto rc = getAvailableOutputStreams(staticMeta,
            outputPreviewStreams, previewThreshold);
    free_camera_metadata(staticMeta);
    ASSERT_EQ(Status::OK, rc);
    ASSERT_FALSE(outputPreviewStreams.empty());

    *previewStream = {0, StreamType::OUTPUT,
            static_cast<uint32_t> (outputPreviewStreams[0].width),
            static_cast<uint32_t> (outputPreviewStreams[0].height),
            static_cast<PixelFormat> (outputPreviewStreams[0].format),
            0, 0, StreamRotation::ROTATION_0};
    ::android::hardware::hidl_vec<Stream> streams = {*previewStream};
    StreamConfiguration config = {streams,
            StreamConfigurationMode::NORMAL_MODE};
    ret = (*session)->configureStreams(config, [&] (Status s,
            HalStreamConfiguration halConfig) {
        ASSERT_EQ(Status::OK, s);
        ASSERT_EQ(1u, halConfig.streams.size());
        *halStreamConfig = halConfig;
    });
    ASSERT_TRUE(ret.isOk());
}

// Open a device session with empty callbacks and return static metadata.
void CameraHidlTest::openEmptyDeviceSession(const std::string &name,
        const CameraHidlEnvironment* env,
        sp<ICameraDeviceSession> *session /*out*/,
        camera_metadata_t **staticMeta /*out*/) {
    ASSERT_NE(nullptr, env);
    ASSERT_NE(nullptr, session);
    ASSERT_NE(nullptr, staticMeta);

    ::android::sp<ICameraDevice> device3_2;
    ALOGI("configureStreams: Testing camera device %s", name.c_str());
    Return<void> ret;
    ret = env->mProvider->getCameraDeviceInterface_V3_x(
        name,
        [&](auto status, const auto& device) {
            ALOGI("getCameraDeviceInterface_V3_x returns status:%d",
                  (int)status);
            ASSERT_EQ(Status::OK, status);
            ASSERT_NE(device, nullptr);
            device3_2 = device;
        });
    ASSERT_TRUE(ret.isOk());

    sp<EmptyDeviceCb> cb = new EmptyDeviceCb();
    ret = device3_2->open(cb, [&](auto status, const auto& newSession) {
            ALOGI("device::open returns status:%d", (int)status);
            ASSERT_EQ(Status::OK, status);
            ASSERT_NE(newSession, nullptr);
            *session = newSession;
        });
    ASSERT_TRUE(ret.isOk());

    ret = device3_2->getCameraCharacteristics([&] (Status s,
            CameraMetadata metadata) {
        ASSERT_EQ(Status::OK, s);
        *staticMeta = clone_camera_metadata(
                reinterpret_cast<const camera_metadata_t*>(metadata.data()));
        ASSERT_NE(nullptr, *staticMeta);
    });
    ASSERT_TRUE(ret.isOk());
}

// Open a particular camera device.
void CameraHidlTest::openCameraDevice(const std::string &name,
        const CameraHidlEnvironment* env,
        sp<::android::hardware::camera::device::V1_0::ICameraDevice> *device1 /*out*/) {
    ASSERT_TRUE(nullptr != env);
    ASSERT_TRUE(nullptr != device1);

    Return<void> ret;
    ret = env->mProvider->getCameraDeviceInterface_V1_x(
            name,
            [&](auto status, const auto& device) {
            ALOGI("getCameraDeviceInterface_V1_x returns status:%d",
                  (int)status);
            ASSERT_EQ(Status::OK, status);
            ASSERT_NE(device, nullptr);
            *device1 = device;
        });
    ASSERT_TRUE(ret.isOk());

    sp<Camera1DeviceCb> deviceCb = new Camera1DeviceCb(this);
    Return<Status> returnStatus = (*device1)->open(deviceCb);
    ASSERT_TRUE(returnStatus.isOk());
    ASSERT_EQ(Status::OK, returnStatus);
}

// Initialize and configure a preview window.
void CameraHidlTest::setupPreviewWindow(
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
        sp<BufferItemConsumer> *bufferItemConsumer /*out*/,
        sp<BufferItemHander> *bufferHandler /*out*/) {
    ASSERT_NE(nullptr, device.get());
    ASSERT_NE(nullptr, bufferItemConsumer);
    ASSERT_NE(nullptr, bufferHandler);

    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    *bufferItemConsumer = new BufferItemConsumer(consumer,
            GraphicBuffer::USAGE_HW_TEXTURE); //Use GLConsumer default usage flags
    ASSERT_NE(nullptr, (*bufferItemConsumer).get());
    *bufferHandler = new BufferItemHander(*bufferItemConsumer);
    ASSERT_NE(nullptr, (*bufferHandler).get());
    (*bufferItemConsumer)->setFrameAvailableListener(*bufferHandler);
    sp<Surface> surface = new Surface(producer);
    sp<PreviewWindowCb> previewCb = new PreviewWindowCb(surface);

    auto rc = device->setPreviewWindow(previewCb);
    ASSERT_TRUE(rc.isOk());
    ASSERT_EQ(Status::OK, rc);
}

// Stop camera preview and close camera.
void CameraHidlTest::stopPreviewAndClose(
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device) {
    Return<void> ret = device->stopPreview();
    ASSERT_TRUE(ret.isOk());

    ret = device->close();
    ASSERT_TRUE(ret.isOk());
}

// Enable a specific camera message type.
void CameraHidlTest::enableMsgType(unsigned int msgType,
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device) {
    Return<void> ret = device->enableMsgType(msgType);
    ASSERT_TRUE(ret.isOk());

    Return<bool> returnBoolStatus = device->msgTypeEnabled(msgType);
    ASSERT_TRUE(returnBoolStatus.isOk());
    ASSERT_TRUE(returnBoolStatus);
}

// Disable a specific camera message type.
void CameraHidlTest::disableMsgType(unsigned int msgType,
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device) {
    Return<void> ret = device->disableMsgType(msgType);
    ASSERT_TRUE(ret.isOk());

    Return<bool> returnBoolStatus = device->msgTypeEnabled(msgType);
    ASSERT_TRUE(returnBoolStatus.isOk());
    ASSERT_FALSE(returnBoolStatus);
}

// Wait until a specific frame notification arrives.
void CameraHidlTest::waitForFrameLocked(DataCallbackMsg msgFrame,
        std::unique_lock<std::mutex> &l) {
    while (msgFrame != mDataMessageTypeReceived) {
        auto timeout = std::chrono::system_clock::now() +
                std::chrono::seconds(kStreamBufferTimeoutSec);
        ASSERT_NE(std::cv_status::timeout,
                mResultCondition.wait_until(l, timeout));
    }
}

// Start preview on a particular camera device
void CameraHidlTest::startPreview(
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device) {
    Return<Status> returnStatus = device->startPreview();
    ASSERT_TRUE(returnStatus.isOk());
    ASSERT_EQ(Status::OK, returnStatus);
}

// Retrieve camera parameters.
void CameraHidlTest::getParameters(
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
        CameraParameters *cameraParams /*out*/) {
    ASSERT_NE(nullptr, cameraParams);

    Return<void> ret;
    ret = device->getParameters([&] (const ::android::hardware::hidl_string& params) {
        ASSERT_FALSE(params.empty());
        ::android::String8 paramString(params.c_str());
        (*cameraParams).unflatten(paramString);
    });
    ASSERT_TRUE(ret.isOk());
}

// Set camera parameters.
void CameraHidlTest::setParameters(
        const sp<::android::hardware::camera::device::V1_0::ICameraDevice> &device,
        const CameraParameters &cameraParams) {
    Return<Status> returnStatus = device->setParameters(
            cameraParams.flatten().string());
    ASSERT_TRUE(returnStatus.isOk());
    ASSERT_EQ(Status::OK, returnStatus);
}

int main(int argc, char **argv) {
  ::testing::AddGlobalTestEnvironment(CameraHidlEnvironment::Instance());
  ::testing::InitGoogleTest(&argc, argv);
  int status = RUN_ALL_TESTS();
  ALOGI("Test result = %d", status);
  return status;
}
