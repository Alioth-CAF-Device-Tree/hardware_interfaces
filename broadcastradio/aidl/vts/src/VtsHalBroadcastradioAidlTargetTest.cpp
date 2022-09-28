/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define EGMOCK_VERBOSE 1

#include <aidl/android/hardware/broadcastradio/BnAnnouncementListener.h>
#include <aidl/android/hardware/broadcastradio/BnTunerCallback.h>
#include <aidl/android/hardware/broadcastradio/ConfigFlag.h>
#include <aidl/android/hardware/broadcastradio/IBroadcastRadio.h>
#include <aidl/android/hardware/broadcastradio/ProgramListChunk.h>
#include <aidl/android/hardware/broadcastradio/ProgramSelector.h>
#include <aidl/android/hardware/broadcastradio/VendorKeyValue.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/thread_annotations.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <broadcastradio-utils-aidl/Utils.h>
#include <broadcastradio-vts-utils/mock-timeout.h>
#include <cutils/bitops.h>
#include <gmock/gmock.h>

#include <chrono>
#include <optional>
#include <regex>

namespace aidl::android::hardware::broadcastradio::vts {

namespace {

using ::aidl::android::hardware::broadcastradio::utils::makeIdentifier;
using ::aidl::android::hardware::broadcastradio::utils::makeSelectorAmfm;
using ::aidl::android::hardware::broadcastradio::utils::resultToInt;
using ::ndk::ScopedAStatus;
using ::ndk::SharedRefBase;
using ::std::string;
using ::std::vector;
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::ByMove;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::SaveArg;

namespace bcutils = ::aidl::android::hardware::broadcastradio::utils;

inline constexpr std::chrono::seconds kTuneTimeoutSec =
        std::chrono::seconds(IBroadcastRadio::TUNER_TIMEOUT_MS * 1000);
inline constexpr std::chrono::seconds kProgramListScanTimeoutSec =
        std::chrono::seconds(IBroadcastRadio::LIST_COMPLETE_TIMEOUT_MS * 1000);

const ConfigFlag kConfigFlagValues[] = {
        ConfigFlag::FORCE_MONO,
        ConfigFlag::FORCE_ANALOG,
        ConfigFlag::FORCE_DIGITAL,
        ConfigFlag::RDS_AF,
        ConfigFlag::RDS_REG,
        ConfigFlag::DAB_DAB_LINKING,
        ConfigFlag::DAB_FM_LINKING,
        ConfigFlag::DAB_DAB_SOFT_LINKING,
        ConfigFlag::DAB_FM_SOFT_LINKING,
};

void printSkipped(const string& msg) {
    const auto testInfo = testing::UnitTest::GetInstance()->current_test_info();
    LOG(INFO) << "[  SKIPPED ] " << testInfo->test_case_name() << "." << testInfo->name()
              << " with message: " << msg;
}

bool isValidAmFmFreq(int64_t freq) {
    ProgramIdentifier id = bcutils::makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, freq);
    return bcutils::isValid(id);
}

void validateRange(const AmFmBandRange& range) {
    EXPECT_TRUE(isValidAmFmFreq(range.lowerBound));
    EXPECT_TRUE(isValidAmFmFreq(range.upperBound));
    EXPECT_LT(range.lowerBound, range.upperBound);
    EXPECT_GT(range.spacing, 0u);
    EXPECT_EQ((range.upperBound - range.lowerBound) % range.spacing, 0u);
}

bool supportsFM(const AmFmRegionConfig& config) {
    for (const auto& range : config.ranges) {
        if (bcutils::getBand(range.lowerBound) == bcutils::FrequencyBand::FM) {
            return true;
        }
    }
    return false;
}

}  // namespace

class TunerCallbackMock : public BnTunerCallback {
  public:
    TunerCallbackMock();

    MOCK_METHOD2(onTuneFailed, ScopedAStatus(Result, const ProgramSelector&));
    MOCK_TIMEOUT_METHOD1(onCurrentProgramInfoChangedMock, ScopedAStatus(const ProgramInfo&));
    ScopedAStatus onCurrentProgramInfoChanged(const ProgramInfo& info) override;
    ScopedAStatus onProgramListUpdated(const ProgramListChunk& chunk) override;
    MOCK_METHOD1(onAntennaStateChange, ScopedAStatus(bool connected));
    MOCK_METHOD1(onParametersUpdated, ScopedAStatus(const vector<VendorKeyValue>& parameters));
    MOCK_METHOD2(onConfigFlagUpdated, ScopedAStatus(ConfigFlag in_flag, bool in_value));
    MOCK_TIMEOUT_METHOD0(onProgramListReady, void());

    std::mutex mLock;
    bcutils::ProgramInfoSet mProgramList GUARDED_BY(mLock);
};

struct AnnouncementListenerMock : public BnAnnouncementListener {
    MOCK_METHOD1(onListUpdated, ScopedAStatus(const vector<Announcement>&));
};

class BroadcastRadioHalTest : public testing::TestWithParam<string> {
  protected:
    void SetUp() override;
    void TearDown() override;

    bool getAmFmRegionConfig(bool full, AmFmRegionConfig* config);
    std::optional<bcutils::ProgramInfoSet> getProgramList();
    std::optional<bcutils::ProgramInfoSet> getProgramList(const ProgramFilter& filter);

    std::shared_ptr<IBroadcastRadio> mModule;
    Properties mProperties;
    std::shared_ptr<TunerCallbackMock> mCallback = SharedRefBase::make<TunerCallbackMock>();
};

MATCHER_P(InfoHasId, id, string(negation ? "does not contain" : "contains") + " " + id.toString()) {
    vector<int> ids = bcutils::getAllIds(arg.selector, id.type);
    return ids.end() != find(ids.begin(), ids.end(), id.value);
}

TunerCallbackMock::TunerCallbackMock() {
    EXPECT_TIMEOUT_CALL(*this, onCurrentProgramInfoChangedMock, _).Times(AnyNumber());

    // we expect the antenna is connected through the whole test
    EXPECT_CALL(*this, onAntennaStateChange(false)).Times(0);
}

ScopedAStatus TunerCallbackMock::onCurrentProgramInfoChanged(const ProgramInfo& info) {
    for (const auto& id : info.selector) {
        EXPECT_NE(id.type, IdentifierType::INVALID);
    }

    IdentifierType logically = info.logicallyTunedTo.type;
    // This field is required for currently tuned program and should be INVALID
    // for entries from the program list.
    EXPECT_TRUE(logically == IdentifierType::AMFM_FREQUENCY_KHZ ||
                logically == IdentifierType::RDS_PI ||
                logically == IdentifierType::HD_STATION_ID_EXT ||
                logically == IdentifierType::DAB_SID_EXT ||
                logically == IdentifierType::DRMO_SERVICE_ID ||
                logically == IdentifierType::SXM_SERVICE_ID ||
                (logically >= IdentifierType::VENDOR_START &&
                 logically <= IdentifierType::VENDOR_END) ||
                logically > IdentifierType::SXM_CHANNEL);

    IdentifierType physically = info.physicallyTunedTo.type;
    // ditto (see "logically" above)
    EXPECT_TRUE(physically == IdentifierType::AMFM_FREQUENCY_KHZ ||
                physically == IdentifierType::DAB_ENSEMBLE ||
                physically == IdentifierType::DRMO_FREQUENCY_KHZ ||
                physically == IdentifierType::SXM_CHANNEL ||
                (physically >= IdentifierType::VENDOR_START &&
                 physically <= IdentifierType::VENDOR_END) ||
                physically > IdentifierType::SXM_CHANNEL);

    if (logically == IdentifierType::AMFM_FREQUENCY_KHZ) {
        std::optional<string> ps = bcutils::getMetadataString(info, Metadata::rdsPs);
        if (ps.has_value()) {
            EXPECT_NE(::android::base::Trim(*ps), "")
                    << "Don't use empty RDS_PS as an indicator of missing RSD PS data.";
        }
    }

    return onCurrentProgramInfoChangedMock(info);
}

ScopedAStatus TunerCallbackMock::onProgramListUpdated(const ProgramListChunk& chunk) {
    std::lock_guard<std::mutex> lk(mLock);

    updateProgramList(chunk, &mProgramList);

    if (chunk.complete) {
        onProgramListReady();
    }

    return ndk::ScopedAStatus::ok();
}

void BroadcastRadioHalTest::SetUp() {
    EXPECT_EQ(mModule.get(), nullptr) << "Module is already open";

    // lookup AIDL service (radio module)
    AIBinder* binder = AServiceManager_waitForService(GetParam().c_str());
    ASSERT_NE(binder, nullptr);
    mModule = IBroadcastRadio::fromBinder(ndk::SpAIBinder(binder));
    ASSERT_NE(mModule, nullptr) << "Couldn't find broadcast radio HAL implementation";

    // get module properties
    auto propResult = mModule->getProperties(&mProperties);

    ASSERT_TRUE(propResult.isOk());
    EXPECT_FALSE(mProperties.maker.empty());
    EXPECT_FALSE(mProperties.product.empty());
    EXPECT_GT(mProperties.supportedIdentifierTypes.size(), 0u);

    // set callback
    EXPECT_TRUE(mModule->setTunerCallback(mCallback).isOk());
}

void BroadcastRadioHalTest::TearDown() {
    if (mModule) {
        ASSERT_TRUE(mModule->unsetTunerCallback().isOk());
    }
}

bool BroadcastRadioHalTest::getAmFmRegionConfig(bool full, AmFmRegionConfig* config) {
    auto halResult = mModule->getAmFmRegionConfig(full, config);

    if (halResult.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        return false;
    }

    EXPECT_TRUE(halResult.isOk());
    return halResult.isOk();
}

std::optional<bcutils::ProgramInfoSet> BroadcastRadioHalTest::getProgramList() {
    ProgramFilter emptyFilter = {};
    return getProgramList(emptyFilter);
}

std::optional<bcutils::ProgramInfoSet> BroadcastRadioHalTest::getProgramList(
        const ProgramFilter& filter) {
    EXPECT_TIMEOUT_CALL(*mCallback, onProgramListReady).Times(AnyNumber());

    auto startResult = mModule->startProgramListUpdates(filter);

    if (startResult.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        printSkipped("Program list not supported");
        return std::nullopt;
    }
    EXPECT_TRUE(startResult.isOk());
    if (!startResult.isOk()) {
        return std::nullopt;
    }
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onProgramListReady, kProgramListScanTimeoutSec);

    auto stopResult = mModule->stopProgramListUpdates();

    EXPECT_TRUE(stopResult.isOk());

    return mCallback->mProgramList;
}

/**
 * Test setting tuner callback to null.
 *
 * Verifies that:
 *  - Setting to a null tuner callback results with INVALID_ARGUMENTS.
 */
TEST_P(BroadcastRadioHalTest, TunerCallbackFailsWithNull) {
    LOG(DEBUG) << "TunerCallbackFailsWithNull Test";

    auto halResult = mModule->setTunerCallback(nullptr);

    EXPECT_EQ(halResult.getServiceSpecificError(), resultToInt(Result::INVALID_ARGUMENTS));
}

/**
 * Test fetching AM/FM regional configuration.
 *
 * Verifies that:
 *  - AM/FM regional configuration is either set at startup or not supported at all by the hardware;
 *  - FM Deemphasis and RDS are correctly configured for FM-capable radio;
 */
TEST_P(BroadcastRadioHalTest, GetAmFmRegionConfig) {
    LOG(DEBUG) << "GetAmFmRegionConfig Test";

    AmFmRegionConfig config;

    bool supported = getAmFmRegionConfig(/* full= */ false, &config);

    if (!supported) {
        printSkipped("AM/FM not supported");
        return;
    }

    EXPECT_LE(popcountll(static_cast<unsigned long long>(config.fmDeemphasis)), 1);
    EXPECT_LE(popcountll(static_cast<unsigned long long>(config.fmRds)), 1);

    if (supportsFM(config)) {
        EXPECT_EQ(popcountll(static_cast<unsigned long long>(config.fmDeemphasis)), 1);
    }
}

/**
 * Test fetching ranges of AM/FM regional configuration.
 *
 * Verifies that:
 *  - AM/FM regional configuration is either set at startup or not supported at all by the hardware;
 *  - there is at least one AM/FM band configured;
 *  - all channel grids (frequency ranges and spacings) are valid;
 *  - seek spacing is a multiple of the manual spacing value.
 */
TEST_P(BroadcastRadioHalTest, GetAmFmRegionConfigRanges) {
    LOG(DEBUG) << "GetAmFmRegionConfigRanges Test";

    AmFmRegionConfig config;

    bool supported = getAmFmRegionConfig(/* full= */ false, &config);

    if (!supported) {
        printSkipped("AM/FM not supported");
        return;
    }

    EXPECT_GT(config.ranges.size(), 0u);
    for (const auto& range : config.ranges) {
        validateRange(range);
        EXPECT_EQ(range.seekSpacing % range.spacing, 0u);
        EXPECT_GE(range.seekSpacing, range.spacing);
    }
}

/**
 * Test fetching FM regional capabilities.
 *
 * Verifies that:
 *  - AM/FM regional capabilities are either available or not supported at all by the hardware;
 *  - there is at least one de-emphasis filter mode supported for FM-capable radio;
 */
TEST_P(BroadcastRadioHalTest, GetAmFmRegionConfigCapabilitiesForFM) {
    LOG(DEBUG) << "GetAmFmRegionConfigCapabilitiesForFM Test";

    AmFmRegionConfig config;

    bool supported = getAmFmRegionConfig(/* full= */ true, &config);

    if (supported && supportsFM(config)) {
        EXPECT_GE(popcountll(static_cast<unsigned long long>(config.fmDeemphasis)), 1);
    } else {
        printSkipped("FM not supported");
    }
}

/**
 * Test fetching the ranges of AM/FM regional capabilities.
 *
 * Verifies that:
 *  - AM/FM regional capabilities are either available or not supported at all by the hardware;
 *  - there is at least one AM/FM range supported;
 *  - all channel grids (frequency ranges and spacings) are valid;
 *  - seek spacing is not set.
 */
TEST_P(BroadcastRadioHalTest, GetAmFmRegionConfigCapabilitiesRanges) {
    LOG(DEBUG) << "GetAmFmRegionConfigCapabilitiesRanges Test";

    AmFmRegionConfig config;

    bool supported = getAmFmRegionConfig(/* full= */ true, &config);

    if (!supported) {
        printSkipped("AM/FM not supported");
        return;
    }

    EXPECT_GT(config.ranges.size(), 0u);

    for (const auto& range : config.ranges) {
        validateRange(range);
        EXPECT_EQ(range.seekSpacing, 0u);
    }
}

/**
 * Test fetching DAB regional configuration.
 *
 * Verifies that:
 *  - DAB regional configuration is either set at startup or not supported at all by the hardware;
 *  - all channel labels match correct format;
 *  - all channel frequencies are in correct range.
 */
TEST_P(BroadcastRadioHalTest, GetDabRegionConfig) {
    LOG(DEBUG) << "GetDabRegionConfig Test";
    vector<DabTableEntry> config;

    auto halResult = mModule->getDabRegionConfig(&config);

    if (halResult.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        printSkipped("DAB not supported");
        return;
    }
    ASSERT_TRUE(halResult.isOk());

    std::regex re("^[A-Z0-9][A-Z0-9 ]{0,5}[A-Z0-9]$");

    for (const auto& entry : config) {
        EXPECT_TRUE(std::regex_match(string(entry.label), re));

        ProgramIdentifier id =
                bcutils::makeIdentifier(IdentifierType::DAB_FREQUENCY_KHZ, entry.frequencyKhz);
        EXPECT_TRUE(bcutils::isValid(id));
    }
}

/**
 * Test tuning without tuner callback set.
 *
 * Verifies that:
 *  - No tuner callback set results in INVALID_STATE, regardless of whether the selector is
 * supported.
 */
TEST_P(BroadcastRadioHalTest, TuneFailsWithoutTunerCallback) {
    LOG(DEBUG) << "TuneFailsWithoutTunerCallback Test";

    mModule->unsetTunerCallback();
    int64_t freq = 90900;  // 90.9 FM
    ProgramSelector sel = makeSelectorAmfm(freq);

    auto result = mModule->tune(sel);

    EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::INVALID_STATE));
}

/**
 * Test tuning with selectors that can be not supported.
 *
 * Verifies that:
 *  - if the selector is not supported, an invalid value results with NOT_SUPPORTED, regardless of
 *    whether it is valid;
 *  - if it is supported, the test is ignored;
 */
TEST_P(BroadcastRadioHalTest, TuneFailsWithNotSupported) {
    LOG(DEBUG) << "TuneFailsWithInvalid Test";

    vector<ProgramIdentifier> supportTestId = {
            makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, 0),           // invalid
            makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, 94900),       // valid
            makeIdentifier(IdentifierType::RDS_PI, 0x10000),                 // invalid
            makeIdentifier(IdentifierType::RDS_PI, 0x1001),                  // valid
            makeIdentifier(IdentifierType::HD_STATION_ID_EXT, 0x100000000),  // invalid
            makeIdentifier(IdentifierType::HD_STATION_ID_EXT, 0x10000001),   // valid
            makeIdentifier(IdentifierType::DAB_SID_EXT, 0),                  // invalid
            makeIdentifier(IdentifierType::DAB_SID_EXT, 0xA00001),           // valid
            makeIdentifier(IdentifierType::DRMO_SERVICE_ID, 0x100000000),    // invalid
            makeIdentifier(IdentifierType::DRMO_SERVICE_ID, 0x10000001),     // valid
            makeIdentifier(IdentifierType::SXM_SERVICE_ID, 0x100000000),     // invalid
            makeIdentifier(IdentifierType::SXM_SERVICE_ID, 0x10000001),      // valid
    };

    auto notSupportedError = resultToInt(Result::NOT_SUPPORTED);
    for (const auto& id : supportTestId) {
        ProgramSelector sel{id, {}};

        auto result = mModule->tune(sel);

        if (!bcutils::isSupported(mProperties, sel)) {
            EXPECT_EQ(result.getServiceSpecificError(), notSupportedError);
        }
    }
}

/**
 * Test tuning with invalid selectors.
 *
 * Verifies that:
 *  - if the selector is not supported, it's ignored;
 *  - if it is supported, an invalid value results with INVALID_ARGUMENTS;
 */
TEST_P(BroadcastRadioHalTest, TuneFailsWithInvalid) {
    LOG(DEBUG) << "TuneFailsWithInvalid Test";

    vector<ProgramIdentifier> invalidId = {
            makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, 0),
            makeIdentifier(IdentifierType::RDS_PI, 0x10000),
            makeIdentifier(IdentifierType::HD_STATION_ID_EXT, 0x100000000),
            makeIdentifier(IdentifierType::DAB_SID_EXT, 0),
            makeIdentifier(IdentifierType::DRMO_SERVICE_ID, 0x100000000),
            makeIdentifier(IdentifierType::SXM_SERVICE_ID, 0x100000000),
    };

    auto invalidArgumentsError = resultToInt(Result::INVALID_ARGUMENTS);
    for (const auto& id : invalidId) {
        ProgramSelector sel{id, {}};

        auto result = mModule->tune(sel);

        if (bcutils::isSupported(mProperties, sel)) {
            EXPECT_EQ(result.getServiceSpecificError(), invalidArgumentsError);
        }
    }
}

/**
 * Test tuning with empty program selector.
 *
 * Verifies that:
 *  - tune fails with NOT_SUPPORTED when program selector is not initialized.
 */
TEST_P(BroadcastRadioHalTest, TuneFailsWithEmpty) {
    LOG(DEBUG) << "TuneFailsWithEmpty Test";

    // Program type is 1-based, so 0 will always be invalid.
    ProgramSelector sel = {};

    auto result = mModule->tune(sel);

    ASSERT_EQ(result.getServiceSpecificError(), resultToInt(Result::NOT_SUPPORTED));
}

/**
 * Test tuning with FM selector.
 *
 * Verifies that:
 *  - if AM/FM selector is not supported, the method returns NOT_SUPPORTED;
 *  - if it is supported, the method succeeds;
 *  - after a successful tune call, onCurrentProgramInfoChanged callback is
 *    invoked carrying a proper selector;
 *  - program changes exactly to what was requested.
 */
TEST_P(BroadcastRadioHalTest, FmTune) {
    LOG(DEBUG) << "FmTune Test";

    int64_t freq = 90900;  // 90.9 FM
    ProgramSelector sel = makeSelectorAmfm(freq);
    // try tuning
    ProgramInfo infoCb = {};
    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock,
                        InfoHasId(makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, freq)))
            .Times(AnyNumber())
            .WillOnce(DoAll(SaveArg<0>(&infoCb), testing::Return(ByMove(ndk::ScopedAStatus::ok()))))
            .WillRepeatedly(testing::InvokeWithoutArgs([] { return ndk::ScopedAStatus::ok(); }));

    auto result = mModule->tune(sel);

    // expect a failure if it's not supported
    if (!bcutils::isSupported(mProperties, sel)) {
        EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::NOT_SUPPORTED));
        return;
    }

    // expect a callback if it succeeds
    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);

    LOG(DEBUG) << "Current program info: " << infoCb.toString();

    // it should tune exactly to what was requested
    vector<int> freqs = bcutils::getAllIds(infoCb.selector, IdentifierType::AMFM_FREQUENCY_KHZ);
    EXPECT_NE(freqs.end(), find(freqs.begin(), freqs.end(), freq))
            << "FM freq " << freq << " kHz is not sent back by callback.";
}

/**
 * Test tuning with DAB selector.
 *
 * Verifies that:
 *  - if DAB selector is not supported, the method returns NOT_SUPPORTED;
 *  - if it is supported, the method succeeds;
 *  - after a successful tune call, onCurrentProgramInfoChanged callback is
 *    invoked carrying a proper selector;
 *  - program changes exactly to what was requested.
 */
TEST_P(BroadcastRadioHalTest, DabTune) {
    LOG(DEBUG) << "DabTune Test";
    vector<DabTableEntry> config;

    auto halResult = mModule->getDabRegionConfig(&config);

    if (halResult.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        printSkipped("DAB not supported");
        return;
    }
    ASSERT_TRUE(halResult.isOk());
    ASSERT_NE(config.size(), 0U);

    // TODO(245787803): use a DAB frequency that can actually be tuned to.
    ProgramSelector sel = {};
    int64_t freq = config[config.size() / 2].frequencyKhz;
    sel.primaryId = makeIdentifier(IdentifierType::DAB_FREQUENCY_KHZ, freq);

    // try tuning
    ProgramInfo infoCb = {};
    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock,
                        InfoHasId(makeIdentifier(IdentifierType::DAB_FREQUENCY_KHZ, freq)))
            .Times(AnyNumber())
            .WillOnce(
                    DoAll(SaveArg<0>(&infoCb), testing::Return(ByMove(ndk::ScopedAStatus::ok()))));

    auto result = mModule->tune(sel);

    // expect a failure if it's not supported
    if (!bcutils::isSupported(mProperties, sel)) {
        EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::NOT_SUPPORTED));
        return;
    }

    // expect a callback if it succeeds
    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);
    LOG(DEBUG) << "Current program info: " << infoCb.toString();

    // it should tune exactly to what was requested
    vector<int> freqs = bcutils::getAllIds(infoCb.selector, IdentifierType::DAB_FREQUENCY_KHZ);
    EXPECT_NE(freqs.end(), find(freqs.begin(), freqs.end(), freq))
            << "DAB freq " << freq << " kHz is not sent back by callback.";
    ;
}

/**
 * Test seeking to next/prev station via IBroadcastRadio::seek().
 *
 * Verifies that:
 *  - the method succeeds;
 *  - the program info is changed within kTuneTimeoutSec;
 *  - works both directions and with or without skipping sub-channel.
 */
TEST_P(BroadcastRadioHalTest, Seek) {
    LOG(DEBUG) << "Seek Test";

    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock, _).Times(AnyNumber());

    auto result = mModule->seek(/* in_directionUp= */ true, /* in_skipSubChannel= */ true);

    if (result.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        printSkipped("Seek not supported");
        return;
    }

    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);

    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock, _).Times(AnyNumber());

    result = mModule->seek(/* in_directionUp= */ false, /* in_skipSubChannel= */ false);

    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);
}

/**
 * Test seeking without tuner callback set.
 *
 * Verifies that:
 *  - No tuner callback set results in INVALID_STATE.
 */
TEST_P(BroadcastRadioHalTest, SeekFailsWithoutTunerCallback) {
    LOG(DEBUG) << "SeekFailsWithoutTunerCallback Test";

    mModule->unsetTunerCallback();

    auto result = mModule->seek(/* in_directionUp= */ true, /* in_skipSubChannel= */ true);

    EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::INVALID_STATE));

    result = mModule->seek(/* in_directionUp= */ false, /* in_skipSubChannel= */ false);

    EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::INVALID_STATE));
}

/**
 * Test step operation.
 *
 * Verifies that:
 *  - the method succeeds or returns NOT_SUPPORTED;
 *  - the program info is changed within kTuneTimeoutSec if the method succeeded;
 *  - works both directions.
 */
TEST_P(BroadcastRadioHalTest, Step) {
    LOG(DEBUG) << "Step Test";

    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock, _).Times(AnyNumber());

    auto result = mModule->step(/* in_directionUp= */ true);

    if (result.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        printSkipped("Step not supported");
        return;
    }
    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);

    EXPECT_TIMEOUT_CALL(*mCallback, onCurrentProgramInfoChangedMock, _).Times(AnyNumber());

    result = mModule->step(/* in_directionUp= */ false);

    EXPECT_TRUE(result.isOk());
    EXPECT_TIMEOUT_CALL_WAIT(*mCallback, onCurrentProgramInfoChangedMock, kTuneTimeoutSec);
}

/**
 * Test step operation without tuner callback set.
 *
 * Verifies that:
 *  - No tuner callback set results in INVALID_STATE.
 */
TEST_P(BroadcastRadioHalTest, StepFailsWithoutTunerCallback) {
    LOG(DEBUG) << "StepFailsWithoutTunerCallback Test";

    mModule->unsetTunerCallback();

    auto result = mModule->step(/* in_directionUp= */ true);

    EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::INVALID_STATE));

    result = mModule->step(/* in_directionUp= */ false);

    EXPECT_EQ(result.getServiceSpecificError(), resultToInt(Result::INVALID_STATE));
}

/**
 * Test tune cancellation.
 *
 * Verifies that:
 *  - the method does not crash after being invoked multiple times.
 *
 * Since cancel() might be called after the HAL completes an operation (tune, seek, and step)
 * and before the callback completions, the operation might not be actually canceled and the
 * effect of cancel() is not deterministic to be tested here.
 */
TEST_P(BroadcastRadioHalTest, Cancel) {
    LOG(DEBUG) << "Cancel Test";

    auto notSupportedError = resultToInt(Result::NOT_SUPPORTED);
    for (int i = 0; i < 10; i++) {
        auto result = mModule->seek(/* in_directionUp= */ true, /* in_skipSubChannel= */ true);

        if (result.getServiceSpecificError() == notSupportedError) {
            printSkipped("Cancel is skipped because of seek not supported");
            return;
        }
        EXPECT_TRUE(result.isOk());

        auto cancelResult = mModule->cancel();

        ASSERT_TRUE(cancelResult.isOk());
    }
}

/**
 * Test IBroadcastRadio::get|setParameters() methods called with no parameters.
 *
 * Verifies that:
 *  - callback is called for empty parameters set.
 */
TEST_P(BroadcastRadioHalTest, NoParameters) {
    LOG(DEBUG) << "NoParameters Test";

    vector<VendorKeyValue> parametersResults = {};

    auto halResult = mModule->setParameters({}, &parametersResults);

    ASSERT_TRUE(halResult.isOk());
    ASSERT_EQ(parametersResults.size(), 0u);

    parametersResults.clear();

    halResult = mModule->getParameters({}, &parametersResults);

    ASSERT_TRUE(halResult.isOk());
    ASSERT_EQ(parametersResults.size(), 0u);
}

/**
 * Test IBroadcastRadio::get|setParameters() methods called with unknown parameters.
 *
 * Verifies that:
 *  - unknown parameters are ignored;
 *  - callback is called also for empty results set.
 */
TEST_P(BroadcastRadioHalTest, UnknownParameters) {
    LOG(DEBUG) << "UnknownParameters Test";

    vector<VendorKeyValue> parametersResults = {};

    auto halResult =
            mModule->setParameters({{"com.android.unknown", "sample"}}, &parametersResults);

    ASSERT_TRUE(halResult.isOk());
    ASSERT_EQ(parametersResults.size(), 0u);

    parametersResults.clear();

    halResult = mModule->getParameters({"com.android.unknown*", "sample"}, &parametersResults);

    ASSERT_TRUE(halResult.isOk());
    ASSERT_EQ(parametersResults.size(), 0u);
}

/**
 * Test geting image of invalid ID.
 *
 * Verifies that:
 * - getImage call handles argument 0 gracefully.
 */
TEST_P(BroadcastRadioHalTest, GetNoImage) {
    LOG(DEBUG) << "GetNoImage Test";
    vector<uint8_t> rawImage;

    auto result = mModule->getImage(IBroadcastRadio::INVALID_IMAGE, &rawImage);

    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(rawImage.size(), 0u);
}

/**
 * Test getting config flags.
 *
 * Verifies that:
 * - isConfigFlagSet either succeeds or ends with NOT_SUPPORTED or INVALID_STATE;
 * - call success or failure is consistent with setConfigFlag.
 */
TEST_P(BroadcastRadioHalTest, FetchConfigFlags) {
    LOG(DEBUG) << "FetchConfigFlags Test";

    for (const auto& flag : kConfigFlagValues) {
        bool gotValue = false;

        auto halResult = mModule->isConfigFlagSet(flag, &gotValue);

        if (halResult.getServiceSpecificError() != resultToInt(Result::NOT_SUPPORTED) &&
            halResult.getServiceSpecificError() != resultToInt(Result::INVALID_STATE)) {
            ASSERT_TRUE(halResult.isOk());
        }

        // set must fail or succeed the same way as get
        auto setResult = mModule->setConfigFlag(flag, /* value= */ false);

        EXPECT_TRUE((halResult.isOk() && setResult.isOk()) ||
                    (halResult.getServiceSpecificError()) == setResult.getServiceSpecificError());

        setResult = mModule->setConfigFlag(flag, /* value= */ true);

        EXPECT_TRUE((halResult.isOk() && setResult.isOk()) ||
                    (halResult.getServiceSpecificError()) == setResult.getServiceSpecificError());
    }
}

/**
 * Test setting config flags.
 *
 * Verifies that:
 * - setConfigFlag either succeeds or ends with NOT_SUPPORTED or INVALID_STATE;
 * - isConfigFlagSet reflects the state requested immediately after the set call.
 */
TEST_P(BroadcastRadioHalTest, SetConfigFlags) {
    LOG(DEBUG) << "SetConfigFlags Test";

    auto get = [&](ConfigFlag flag) -> bool {
        bool* gotValue = nullptr;

        auto halResult = mModule->isConfigFlagSet(flag, gotValue);

        EXPECT_FALSE(gotValue == nullptr);
        EXPECT_TRUE(halResult.isOk());
        return *gotValue;
    };

    auto notSupportedError = resultToInt(Result::NOT_SUPPORTED);
    auto invalidStateError = resultToInt(Result::INVALID_STATE);
    for (const auto& flag : kConfigFlagValues) {
        auto result = mModule->setConfigFlag(flag, /* value= */ false);

        if (result.getServiceSpecificError() == notSupportedError ||
            result.getServiceSpecificError() == invalidStateError) {
            // setting to true must result in the same error as false
            auto secondResult = mModule->setConfigFlag(flag, /* value= */ true);

            EXPECT_TRUE((result.isOk() && secondResult.isOk()) ||
                        result.getServiceSpecificError() == secondResult.getServiceSpecificError());
            continue;
        } else {
            ASSERT_TRUE(result.isOk());
        }

        // verify false is set
        bool value = get(flag);
        EXPECT_FALSE(value);

        // try setting true this time
        result = mModule->setConfigFlag(flag, /* value= */ true);

        ASSERT_TRUE(result.isOk());
        value = get(flag);
        EXPECT_TRUE(value);

        // false again
        result = mModule->setConfigFlag(flag, /* value= */ false);

        ASSERT_TRUE(result.isOk());
        value = get(flag);
        EXPECT_FALSE(value);
    }
}

/**
 * Test getting program list using empty program filter.
 *
 * Verifies that:
 * - startProgramListUpdates either succeeds or returns NOT_SUPPORTED;
 * - the complete list is fetched within kProgramListScanTimeoutSec;
 * - stopProgramListUpdates does not crash.
 */
TEST_P(BroadcastRadioHalTest, GetProgramListFromEmptyFilter) {
    LOG(DEBUG) << "GetProgramListFromEmptyFilter Test";

    getProgramList();
}

/**
 * Test getting program list using AMFM frequency program filter.
 *
 * Verifies that:
 * - startProgramListUpdates either succeeds or returns NOT_SUPPORTED;
 * - the complete list is fetched within kProgramListScanTimeoutSec;
 * - stopProgramListUpdates does not crash;
 * - result for startProgramListUpdates using a filter with AMFM_FREQUENCY_KHZ value of the first
 *   AMFM program matches the expected result.
 */
TEST_P(BroadcastRadioHalTest, GetProgramListFromAmFmFilter) {
    LOG(DEBUG) << "GetProgramListFromAmFmFilter Test";

    std::optional<bcutils::ProgramInfoSet> completeList = getProgramList();
    if (!completeList) {
        printSkipped("No program list available");
        return;
    }

    ProgramFilter amfmFilter = {};
    int expectedResultSize = 0;
    uint64_t expectedFreq = 0;
    for (const auto& program : *completeList) {
        vector<int> amfmIds =
                bcutils::getAllIds(program.selector, IdentifierType::AMFM_FREQUENCY_KHZ);
        EXPECT_LE(amfmIds.size(), 1u);
        if (amfmIds.size() == 0) {
            continue;
        }

        if (expectedResultSize == 0) {
            expectedFreq = amfmIds[0];
            amfmFilter.identifiers = {
                    makeIdentifier(IdentifierType::AMFM_FREQUENCY_KHZ, expectedFreq)};
            expectedResultSize = 1;
        } else if (amfmIds[0] == expectedFreq) {
            expectedResultSize++;
        }
    }

    if (expectedResultSize == 0) {
        printSkipped("No Am/FM programs available");
        return;
    }
    std::optional<bcutils::ProgramInfoSet> amfmList = getProgramList(amfmFilter);
    ASSERT_EQ(amfmList->size(), expectedResultSize) << "amfm filter result size is wrong";
}

/**
 * Test getting program list using DAB ensemble program filter.
 *
 * Verifies that:
 * - startProgramListUpdates either succeeds or returns NOT_SUPPORTED;
 * - the complete list is fetched within kProgramListScanTimeoutSec;
 * - stopProgramListUpdates does not crash;
 * - result for startProgramListUpdates using a filter with DAB_ENSEMBLE value of the first DAB
 *   program matches the expected result.
 */
TEST_P(BroadcastRadioHalTest, GetProgramListFromDabFilter) {
    LOG(DEBUG) << "GetProgramListFromDabFilter Test";

    std::optional<bcutils::ProgramInfoSet> completeList = getProgramList();
    if (!completeList) {
        printSkipped("No program list available");
        return;
    }

    ProgramFilter dabFilter = {};
    int expectedResultSize = 0;
    uint64_t expectedEnsemble = 0;
    for (const auto& program : *completeList) {
        auto dabEnsembles = bcutils::getAllIds(program.selector, IdentifierType::DAB_ENSEMBLE);
        EXPECT_LE(dabEnsembles.size(), 1u);
        if (dabEnsembles.size() == 0) {
            continue;
        }

        if (expectedResultSize == 0) {
            expectedEnsemble = dabEnsembles[0];
            dabFilter.identifiers = {
                    makeIdentifier(IdentifierType::DAB_ENSEMBLE, expectedEnsemble)};
            expectedResultSize = 1;
        } else if (dabEnsembles[0] == expectedEnsemble) {
            expectedResultSize++;
        }
    }

    if (expectedResultSize == 0) {
        printSkipped("No DAB programs available");
        return;
    }
    std::optional<bcutils::ProgramInfoSet> dabList = getProgramList(dabFilter);
    ASSERT_EQ(dabList->size(), expectedResultSize) << "dab filter result size is wrong";
}

/**
 * Test HD_STATION_NAME correctness.
 *
 * Verifies that if a program on the list contains HD_STATION_NAME identifier:
 *  - the program provides station name in its metadata;
 *  - the identifier matches the name;
 *  - there is only one identifier of that type.
 */
TEST_P(BroadcastRadioHalTest, HdRadioStationNameId) {
    LOG(DEBUG) << "HdRadioStationNameId Test";

    std::optional<bcutils::ProgramInfoSet> list = getProgramList();
    if (!list) {
        printSkipped("No program list");
        return;
    }

    for (const auto& program : *list) {
        vector<int> nameIds = bcutils::getAllIds(program.selector, IdentifierType::HD_STATION_NAME);
        EXPECT_LE(nameIds.size(), 1u);
        if (nameIds.size() == 0) {
            continue;
        }

        std::optional<string> name = bcutils::getMetadataString(program, Metadata::programName);
        if (!name) {
            name = bcutils::getMetadataString(program, Metadata::rdsPs);
        }
        ASSERT_TRUE(name.has_value());

        ProgramIdentifier expectedId = bcutils::makeHdRadioStationName(*name);
        EXPECT_EQ(nameIds[0], expectedId.value);
    }
}

/**
 * Test announcement listener registration.
 *
 * Verifies that:
 *  - registerAnnouncementListener either succeeds or returns NOT_SUPPORTED;
 *  - if it succeeds, it returns a valid close handle (which is a nullptr otherwise);
 *  - closing handle does not crash.
 */
TEST_P(BroadcastRadioHalTest, AnnouncementListenerRegistration) {
    LOG(DEBUG) << "AnnouncementListenerRegistration Test";
    std::shared_ptr<AnnouncementListenerMock> listener =
            SharedRefBase::make<AnnouncementListenerMock>();
    std::shared_ptr<ICloseHandle> closeHandle = nullptr;

    auto halResult = mModule->registerAnnouncementListener(listener, {AnnouncementType::EMERGENCY},
                                                           &closeHandle);

    if (halResult.getServiceSpecificError() == resultToInt(Result::NOT_SUPPORTED)) {
        ASSERT_EQ(closeHandle.get(), nullptr);
        printSkipped("Announcements not supported");
        return;
    }

    ASSERT_TRUE(halResult.isOk());
    ASSERT_NE(closeHandle.get(), nullptr);

    closeHandle->close();
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(BroadcastRadioHalTest);
INSTANTIATE_TEST_SUITE_P(
        PerInstance, BroadcastRadioHalTest,
        testing::ValuesIn(::android::getAidlHalInstanceNames(IBroadcastRadio::descriptor)),
        ::android::PrintInstanceNameToString);

}  // namespace aidl::android::hardware::broadcastradio::vts

int main(int argc, char** argv) {
    android::base::SetDefaultTag("BcRadio.vts");
    android::base::SetMinimumLogSeverity(android::base::VERBOSE);
    ::testing::InitGoogleTest(&argc, argv);
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    return RUN_ALL_TESTS();
}
