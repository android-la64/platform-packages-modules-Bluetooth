//
// Copyright 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#define LOG_TAG "dual_mode_controller"

#include "vendor_libs/test_vendor_lib/include/dual_mode_controller.h"

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "vendor_libs/test_vendor_lib/include/event_packet.h"
#include "vendor_libs/test_vendor_lib/include/hci_transport.h"

extern "C" {
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "stack/include/hcidefs.h"
}  // extern "C"

namespace {

// Included in certain events to indicate success (specific to the event
// context).
const uint8_t kSuccessStatus = 0;

const uint8_t kUnknownHciCommand = 1;

// The default number encoded in event packets to indicate to the HCI how many
// command packets it can send to the controller.
const uint8_t kNumHciCommandPackets = 1;

// The location of the config file loaded to populate controller attributes.
const std::string kControllerPropertiesFile =
    "/etc/bluetooth/controller_properties.json";

// Inquiry modes for specifiying inquiry result formats.
const uint8_t kStandardInquiry = 0x00;
const uint8_t kRssiInquiry = 0x01;
const uint8_t kExtendedOrRssiInquiry = 0x02;

// The bd address of another (fake) device.
const std::vector<uint8_t> kOtherDeviceBdAddress = {6, 5, 4, 3, 2, 1};

// Fake inquiry response for a fake device.
const std::vector<uint8_t> kPageScanRepetitionMode = {0};
const std::vector<uint8_t> kPageScanPeriodMode = {0};
const std::vector<uint8_t> kPageScanMode = {0};
const std::vector<uint8_t> kClassOfDevice = {1, 2, 3};
const std::vector<uint8_t> kClockOffset = {1, 2};

void LogCommand(const char* command) {
  LOG_INFO(LOG_TAG, "Controller performing command: %s", command);
}

// Functions used by JSONValueConverter to read stringified JSON into Properties
// object.
bool ParseUint8t(const base::StringPiece& value, uint8_t* field) {
  *field = std::stoi(value.as_string());
  return true;
}

bool ParseUint16t(const base::StringPiece& value, uint16_t* field) {
  *field = std::stoi(value.as_string());
  return true;
}

}  // namespace

namespace test_vendor_lib {

void DualModeController::SendCommandComplete(
    uint16_t command_opcode,
    const std::vector<uint8_t>& return_parameters) const {
  std::unique_ptr<EventPacket> command_complete =
      EventPacket::CreateCommandCompleteEvent(
          kNumHciCommandPackets, command_opcode, return_parameters);
  send_event_(std::move(command_complete));
}

void DualModeController::SendCommandCompleteSuccess(
    uint16_t command_opcode) const {
  SendCommandComplete(command_opcode, {kSuccessStatus});
}

void DualModeController::SendCommandStatus(uint8_t status,
                                           uint16_t command_opcode) const {
  std::unique_ptr<EventPacket> command_status =
      EventPacket::CreateCommandStatusEvent(
          status, kNumHciCommandPackets, command_opcode);
  send_event_(std::move(command_status));
}

void DualModeController::SendCommandStatusSuccess(
    uint16_t command_opcode) const {
  SendCommandStatus(kSuccessStatus, command_opcode);
}

void DualModeController::SendInquiryResult() const {
  std::unique_ptr<EventPacket> inquiry_result =
      EventPacket::CreateInquiryResultEvent(1,
                                            kOtherDeviceBdAddress,
                                            kPageScanRepetitionMode,
                                            kPageScanPeriodMode,
                                            kPageScanMode,
                                            kClassOfDevice,
                                            kClockOffset);
  send_event_(std::move(inquiry_result));
}

void DualModeController::SendExtendedInquiryResult(
    const std::string& name, const std::string& address) const {
  std::vector<uint8_t> rssi = {0};
  std::vector<uint8_t> extended_inquiry_data = {
      static_cast<uint8_t>(name.length() + 1), 0x09};
  std::copy(
      name.begin(), name.end(), std::back_inserter(extended_inquiry_data));
  std::vector<uint8_t> bd_address(address.begin(), address.end());
  // TODO(dennischeng): Use constants for parameter sizes, here and elsewhere.
  while (extended_inquiry_data.size() < 240) {
    extended_inquiry_data.push_back(0);
  }
  std::unique_ptr<EventPacket> extended_inquiry_result =
      EventPacket::CreateExtendedInquiryResultEvent(bd_address,
                                                    kPageScanRepetitionMode,
                                                    kPageScanPeriodMode,
                                                    kClassOfDevice,
                                                    kClockOffset,
                                                    rssi,
                                                    extended_inquiry_data);
  send_event_(std::move(extended_inquiry_result));
}

DualModeController::DualModeController()
    : state_(kStandby),
      properties_(kControllerPropertiesFile),
      test_channel_state_(kNone) {
#define SET_HANDLER(opcode, method) \
  active_hci_commands_[opcode] =    \
      std::bind(&DualModeController::method, this, std::placeholders::_1);
  SET_HANDLER(HCI_RESET, HciReset);
  SET_HANDLER(HCI_READ_BUFFER_SIZE, HciReadBufferSize);
  SET_HANDLER(HCI_HOST_BUFFER_SIZE, HciHostBufferSize);
  SET_HANDLER(HCI_READ_LOCAL_VERSION_INFO, HciReadLocalVersionInformation);
  SET_HANDLER(HCI_READ_BD_ADDR, HciReadBdAddr);
  SET_HANDLER(HCI_READ_LOCAL_SUPPORTED_CMDS, HciReadLocalSupportedCommands);
  SET_HANDLER(HCI_READ_LOCAL_SUPPORTED_CODECS, HciReadLocalSupportedCodecs);
  SET_HANDLER(HCI_READ_LOCAL_EXT_FEATURES, HciReadLocalExtendedFeatures);
  SET_HANDLER(HCI_WRITE_SIMPLE_PAIRING_MODE, HciWriteSimplePairingMode);
  SET_HANDLER(HCI_WRITE_LE_HOST_SUPPORT, HciWriteLeHostSupport);
  SET_HANDLER(HCI_SET_EVENT_MASK, HciSetEventMask);
  SET_HANDLER(HCI_WRITE_INQUIRY_MODE, HciWriteInquiryMode);
  SET_HANDLER(HCI_WRITE_PAGESCAN_TYPE, HciWritePageScanType);
  SET_HANDLER(HCI_WRITE_INQSCAN_TYPE, HciWriteInquiryScanType);
  SET_HANDLER(HCI_WRITE_CLASS_OF_DEVICE, HciWriteClassOfDevice);
  SET_HANDLER(HCI_WRITE_PAGE_TOUT, HciWritePageTimeout);
  SET_HANDLER(HCI_WRITE_DEF_POLICY_SETTINGS, HciWriteDefaultLinkPolicySettings);
  SET_HANDLER(HCI_READ_LOCAL_NAME, HciReadLocalName);
  SET_HANDLER(HCI_CHANGE_LOCAL_NAME, HciWriteLocalName);
  SET_HANDLER(HCI_WRITE_EXT_INQ_RESPONSE, HciWriteExtendedInquiryResponse);
  SET_HANDLER(HCI_WRITE_VOICE_SETTINGS, HciWriteVoiceSetting);
  SET_HANDLER(HCI_WRITE_CURRENT_IAC_LAP, HciWriteCurrentIacLap);
  SET_HANDLER(HCI_WRITE_INQUIRYSCAN_CFG, HciWriteInquiryScanActivity);
  SET_HANDLER(HCI_WRITE_SCAN_ENABLE, HciWriteScanEnable);
  SET_HANDLER(HCI_SET_EVENT_FILTER, HciSetEventFilter);
  SET_HANDLER(HCI_INQUIRY, HciInquiry);
  SET_HANDLER(HCI_INQUIRY_CANCEL, HciInquiryCancel);
  SET_HANDLER(HCI_DELETE_STORED_LINK_KEY, HciDeleteStoredLinkKey);
  SET_HANDLER(HCI_RMT_NAME_REQUEST, HciRemoteNameRequest);
  SET_HANDLER(HCI_BLE_SET_EVENT_MASK, HciLeSetEventMask);
  SET_HANDLER(HCI_BLE_READ_BUFFER_SIZE, HciLeReadBufferSize);
  SET_HANDLER(HCI_BLE_READ_LOCAL_SPT_FEAT, HciLeReadLocalSupportedFeatures);
  SET_HANDLER(HCI_BLE_WRITE_RANDOM_ADDR, HciLeSetRandomAddress);
  SET_HANDLER(HCI_BLE_WRITE_SCAN_PARAMS, HciLeSetScanParameters);
  SET_HANDLER(HCI_BLE_WRITE_SCAN_ENABLE, HciLeSetScanEnable);
  SET_HANDLER(HCI_BLE_READ_WHITE_LIST_SIZE, HciLeReadWhiteListSize);
  SET_HANDLER(HCI_BLE_RAND, HciLeRand);
  SET_HANDLER(HCI_BLE_READ_SUPPORTED_STATES, HciLeReadSupportedStates);
  SET_HANDLER((HCI_GRP_VENDOR_SPECIFIC | 0x27), HciBleVendorSleepMode);
  SET_HANDLER(HCI_BLE_VENDOR_CAP_OCF, HciBleVendorCap);
  SET_HANDLER(HCI_BLE_MULTI_ADV_OCF, HciBleVendorMultiAdv);
  SET_HANDLER((HCI_GRP_VENDOR_SPECIFIC | 0x155), HciBleVendor155);
  SET_HANDLER((HCI_GRP_VENDOR_SPECIFIC | 0x157), HciBleVendor157);
  SET_HANDLER(HCI_BLE_ENERGY_INFO_OCF, HciBleEnergyInfo);
  SET_HANDLER(HCI_BLE_EXTENDED_SCAN_PARAMS_OCF, HciBleExtendedScanParams);
#undef SET_HANDLER

#define SET_TEST_HANDLER(command_name, method)  \
  active_test_channel_commands_[command_name] = \
      std::bind(&DualModeController::method, this, std::placeholders::_1);
  SET_TEST_HANDLER("CLEAR", TestChannelClear);
  SET_TEST_HANDLER("CLEAR_EVENT_DELAY", TestChannelClearEventDelay);
  SET_TEST_HANDLER("DISCOVER", TestChannelDiscover);
  SET_TEST_HANDLER("SET_EVENT_DELAY", TestChannelSetEventDelay);
  SET_TEST_HANDLER("TIMEOUT_ALL", TestChannelTimeoutAll);
#undef SET_TEST_HANDLER
}

void DualModeController::RegisterHandlersWithHciTransport(
    HciTransport& transport) {
  transport.RegisterCommandHandler(std::bind(
      &DualModeController::HandleCommand, this, std::placeholders::_1));
}

void DualModeController::RegisterHandlersWithTestChannelTransport(
    TestChannelTransport& transport) {
  transport.RegisterCommandHandler(
      std::bind(&DualModeController::HandleTestChannelCommand,
                this,
                std::placeholders::_1,
                std::placeholders::_2));
}

void DualModeController::HandleTestChannelCommand(
    const std::string& name, const std::vector<std::string>& args) {
  if (active_test_channel_commands_.count(name) == 0)
    return;
  active_test_channel_commands_[name](args);
}

void DualModeController::HandleCommand(
    std::unique_ptr<CommandPacket> command_packet) {
  uint16_t opcode = command_packet->GetOpcode();
  LOG_INFO(LOG_TAG,
           "Command opcode: 0x%04X, OGF: 0x%04X, OCF: 0x%04X",
           opcode,
           command_packet->GetOGF(),
           command_packet->GetOCF());

  // The command hasn't been registered with the handler yet. There is nothing
  // to do.
  if (active_hci_commands_.count(opcode) == 0)
    return;
  else if (test_channel_state_ == kTimeoutAll)
    return;
  active_hci_commands_[opcode](command_packet->GetPayload());
}

void DualModeController::RegisterEventChannel(
    std::function<void(std::unique_ptr<EventPacket>)> callback) {
  send_event_ = callback;
}

void DualModeController::RegisterDelayedEventChannel(
    std::function<void(std::unique_ptr<EventPacket>, base::TimeDelta)>
        callback) {
  send_delayed_event_ = callback;
  SetEventDelay(0);
}

void DualModeController::SetEventDelay(int64_t delay) {
  if (delay < 0)
    delay = 0;
  send_event_ = std::bind(send_delayed_event_,
                          std::placeholders::_1,
                          base::TimeDelta::FromMilliseconds(delay));
}

void DualModeController::TestChannelClear(
    const std::vector<std::string>& args UNUSED_ATTR) {
  LogCommand("TestChannel Clear");
  test_channel_state_ = kNone;
  SetEventDelay(0);
}

void DualModeController::TestChannelDiscover(
    const std::vector<std::string>& args) {
  LogCommand("TestChannel Discover");
  for (size_t i = 0; i < args.size() - 1; i += 2)
    SendExtendedInquiryResult(args[i], args[i + 1]);
}

void DualModeController::TestChannelTimeoutAll(
    const std::vector<std::string>& args UNUSED_ATTR) {
  LogCommand("TestChannel Timeout All");
  test_channel_state_ = kTimeoutAll;
}

void DualModeController::TestChannelSetEventDelay(
    const std::vector<std::string>& args) {
  LogCommand("TestChannel Set Event Delay");
  test_channel_state_ = kDelayedResponse;
  SetEventDelay(std::stoi(args[0]));
}

void DualModeController::TestChannelClearEventDelay(
    const std::vector<std::string>& args UNUSED_ATTR) {
  LogCommand("TestChannel Clear Event Delay");
  test_channel_state_ = kNone;
  SetEventDelay(0);
}

void DualModeController::HciReset(const std::vector<uint8_t>& /* args */) {
  LogCommand("Reset");
  state_ = kStandby;
  SendCommandCompleteSuccess(HCI_RESET);
}

void DualModeController::HciReadBufferSize(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Read Buffer Size");
  SendCommandComplete(HCI_READ_BUFFER_SIZE, properties_.GetBufferSize());
}

void DualModeController::HciHostBufferSize(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Host Buffer Size");
  SendCommandCompleteSuccess(HCI_HOST_BUFFER_SIZE);
}

void DualModeController::HciReadLocalVersionInformation(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Read Local Version Information");
  SendCommandComplete(HCI_READ_LOCAL_VERSION_INFO,
                      properties_.GetLocalVersionInformation());
}

void DualModeController::HciReadBdAddr(const std::vector<uint8_t>& /* args */) {
  std::vector<uint8_t> bd_address_with_status = {
      kSuccessStatus, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  SendCommandComplete(HCI_READ_BD_ADDR, bd_address_with_status);
}

void DualModeController::HciReadLocalSupportedCommands(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Read Local Supported Commands");
  SendCommandComplete(HCI_READ_LOCAL_SUPPORTED_CMDS,
                      properties_.GetLocalSupportedCommands());
}

void DualModeController::HciReadLocalSupportedCodecs(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  std::vector<uint8_t> supported_codecs = {kSuccessStatus, 0x2, 0x0, 0x01, 0x0};

  LogCommand("Read Local Supported Codecs");
  SendCommandComplete(HCI_READ_LOCAL_SUPPORTED_CODECS, supported_codecs);
  // TODO properties_.GetLocalSupportedCodecs());
}

void DualModeController::HciReadLocalExtendedFeatures(
    const std::vector<uint8_t>& args) {
  LogCommand("Read Local Extended Features");
  SendCommandComplete(HCI_READ_LOCAL_EXT_FEATURES,
                      properties_.GetLocalExtendedFeatures(args[0]));
}

void DualModeController::HciWriteSimplePairingMode(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Simple Pairing Mode");
  SendCommandCompleteSuccess(HCI_WRITE_SIMPLE_PAIRING_MODE);
}

void DualModeController::HciWriteLeHostSupport(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Le Host Support");
  SendCommandCompleteSuccess(HCI_WRITE_LE_HOST_SUPPORT);
}

void DualModeController::HciSetEventMask(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Set Event Mask");
  SendCommandCompleteSuccess(HCI_SET_EVENT_MASK);
}

void DualModeController::HciWriteInquiryMode(const std::vector<uint8_t>& args) {
  LogCommand("Write Inquiry Mode");
  CHECK(args.size() == 1);
  inquiry_mode_ = args[0];
  SendCommandCompleteSuccess(HCI_WRITE_INQUIRY_MODE);
}

void DualModeController::HciWritePageScanType(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Page Scan Type");
  SendCommandCompleteSuccess(HCI_WRITE_PAGESCAN_TYPE);
}

void DualModeController::HciWriteInquiryScanType(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Inquiry Scan Type");
  SendCommandCompleteSuccess(HCI_WRITE_INQSCAN_TYPE);
}

void DualModeController::HciWriteClassOfDevice(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Class Of Device");
  SendCommandCompleteSuccess(HCI_WRITE_CLASS_OF_DEVICE);
}

void DualModeController::HciWritePageTimeout(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Page Timeout");
  SendCommandCompleteSuccess(HCI_WRITE_PAGE_TOUT);
}

void DualModeController::HciWriteDefaultLinkPolicySettings(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Default Link Policy Settings");
  SendCommandCompleteSuccess(HCI_WRITE_DEF_POLICY_SETTINGS);
}

void DualModeController::HciReadLocalName(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Get Local Name");
  SendCommandComplete(HCI_READ_LOCAL_NAME, properties_.GetLocalName());
}

void DualModeController::HciWriteLocalName(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Local Name");
  SendCommandCompleteSuccess(HCI_CHANGE_LOCAL_NAME);
}

void DualModeController::HciWriteExtendedInquiryResponse(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Extended Inquiry Response");
  SendCommandCompleteSuccess(HCI_WRITE_EXT_INQ_RESPONSE);
}

void DualModeController::HciWriteVoiceSetting(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Voice Setting");
  SendCommandCompleteSuccess(HCI_WRITE_VOICE_SETTINGS);
}

void DualModeController::HciWriteCurrentIacLap(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Current IAC LAP");
  SendCommandCompleteSuccess(HCI_WRITE_CURRENT_IAC_LAP);
}

void DualModeController::HciWriteInquiryScanActivity(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Inquiry Scan Activity");
  SendCommandCompleteSuccess(HCI_WRITE_INQUIRYSCAN_CFG);
}

void DualModeController::HciWriteScanEnable(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Write Scan Enable");
  SendCommandCompleteSuccess(HCI_WRITE_SCAN_ENABLE);
}

void DualModeController::HciSetEventFilter(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Set Event Filter");
  SendCommandCompleteSuccess(HCI_SET_EVENT_FILTER);
}

void DualModeController::HciInquiry(const std::vector<uint8_t>& /* args */) {
  LogCommand("Inquiry");
  state_ = kInquiry;
  SendCommandStatusSuccess(HCI_INQUIRY);
  switch (inquiry_mode_) {
    case (kStandardInquiry):
      SendInquiryResult();
      break;

    case (kRssiInquiry):
      LOG_INFO(LOG_TAG, "RSSI Inquiry Mode currently not supported.");
      break;

    case (kExtendedOrRssiInquiry):
      SendExtendedInquiryResult("FooBar", "123456");
      break;
  }
}

void DualModeController::HciInquiryCancel(
    const std::vector<uint8_t>& /* args */) {
  LogCommand("Inquiry Cancel");
  CHECK(state_ == kInquiry);
  state_ = kStandby;
  SendCommandCompleteSuccess(HCI_INQUIRY_CANCEL);
}

void DualModeController::HciDeleteStoredLinkKey(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  LogCommand("Delete Stored Link Key");
  /* Check the last octect in |args|. If it is 0, delete only the link key for
   * the given BD_ADDR. If is is 1, delete all stored link keys. */
  SendCommandComplete(HCI_DELETE_STORED_LINK_KEY, {1});
}

void DualModeController::HciRemoteNameRequest(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  LogCommand("Remote Name Request");
  SendCommandStatusSuccess(HCI_RMT_NAME_REQUEST);
}

void DualModeController::HciLeSetEventMask(const std::vector<uint8_t>& args) {
  LogCommand("LE SetEventMask");
  le_event_mask_ = args;
  SendCommandComplete(HCI_BLE_SET_EVENT_MASK, {kSuccessStatus});
}

void DualModeController::HciLeReadBufferSize(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_READ_BUFFER_SIZE, properties_.GetLeBufferSize());
}

void DualModeController::HciLeReadLocalSupportedFeatures(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_READ_LOCAL_SPT_FEAT,
                      properties_.GetLeLocalSupportedFeatures());
}

void DualModeController::HciLeSetRandomAddress(
    const std::vector<uint8_t>& args) {
  LogCommand("LE SetRandomAddress");
  le_random_address_ = args;
  SendCommandComplete(HCI_BLE_WRITE_RANDOM_ADDR, {kSuccessStatus});
}

void DualModeController::HciLeSetScanParameters(
    const std::vector<uint8_t>& args) {
  LogCommand("LE SetScanParameters");
  le_scan_type_ = args[0];
  le_scan_interval_ = args[1] | (args[2] << 8);
  le_scan_window_ = args[3] | (args[4] << 8);
  own_address_type_ = args[5];
  scanning_filter_policy_ = args[6];
  SendCommandComplete(HCI_BLE_WRITE_SCAN_PARAMS, {kSuccessStatus});
}

void DualModeController::HciLeSetScanEnable(const std::vector<uint8_t>& args) {
  LogCommand("LE SetScanEnable");
  le_scan_enable_ = args[0];
  filter_duplicates_ = args[1];
  SendCommandComplete(HCI_BLE_WRITE_SCAN_ENABLE, {kSuccessStatus});
}

void DualModeController::HciLeReadWhiteListSize(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_READ_WHITE_LIST_SIZE,
                      properties_.GetLeWhiteListSize());
}

void DualModeController::HciLeRand(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_RAND, properties_.GetLeRand());
}

void DualModeController::HciLeReadSupportedStates(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_READ_SUPPORTED_STATES,
                      properties_.GetLeSupportedStates());
}

void DualModeController::HciBleVendorSleepMode(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  std::vector<uint8_t> success_multi_adv = {kSuccessStatus, 0x04};

  SendCommandComplete(HCI_GRP_VENDOR_SPECIFIC | 0x27, {kSuccessStatus});
}

void DualModeController::HciBleVendorCap(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_VENDOR_CAP_OCF, properties_.GetLeVendorCap());
}

void DualModeController::HciBleVendorMultiAdv(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  std::vector<uint8_t> success_multi_adv = {kSuccessStatus, 0x04};

  SendCommandComplete(HCI_BLE_MULTI_ADV_OCF, success_multi_adv);
}

void DualModeController::HciBleVendor155(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  std::vector<uint8_t> success155 = {kSuccessStatus, 0x04, 0x80};

  SendCommandComplete(HCI_GRP_VENDOR_SPECIFIC | 0x155, success155);
}

void DualModeController::HciBleVendor157(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_GRP_VENDOR_SPECIFIC | 0x157, {kUnknownHciCommand});
}

void DualModeController::HciBleEnergyInfo(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_ENERGY_INFO_OCF, {kUnknownHciCommand});
}

void DualModeController::HciBleExtendedScanParams(
    const std::vector<uint8_t>& args UNUSED_ATTR) {
  SendCommandComplete(HCI_BLE_EXTENDED_SCAN_PARAMS_OCF, {kUnknownHciCommand});
}

DualModeController::Properties::Properties(const std::string& file_name)
    : local_supported_commands_size_(64), local_name_size_(248) {
  std::string properties_raw;
  if (!base::ReadFileToString(base::FilePath(file_name), &properties_raw))
    LOG_INFO(LOG_TAG, "Error reading controller properties from file.");

  scoped_ptr<base::Value> properties_value_ptr =
      base::JSONReader::Read(properties_raw);
  if (properties_value_ptr.get() == nullptr)
    LOG_INFO(LOG_TAG,
             "Error controller properties may consist of ill-formed JSON.");

  // Get the underlying base::Value object, which is of type
  // base::Value::TYPE_DICTIONARY, and read it into member variables.
  base::Value& properties_dictionary = *(properties_value_ptr.get());
  base::JSONValueConverter<DualModeController::Properties> converter;

  if (!converter.Convert(properties_dictionary, this))
    LOG_INFO(LOG_TAG,
             "Error converting JSON properties into Properties object.");
}

const std::vector<uint8_t> DualModeController::Properties::GetLeBufferSize() {
  return std::vector<uint8_t>(
      {kSuccessStatus,
       static_cast<uint8_t>(le_acl_data_packet_length_),
       static_cast<uint8_t>(le_acl_data_packet_length_ >> 8),
       num_le_acl_data_packets_});
}

const std::vector<uint8_t>
DualModeController::Properties::GetLeLocalSupportedFeatures() {
  std::vector<uint8_t> success_local_supported_features = {
      kSuccessStatus, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};

  return success_local_supported_features;
}

const std::vector<uint8_t>
DualModeController::Properties::GetLeSupportedStates() {
  std::vector<uint8_t> success_supported_states = {
      kSuccessStatus, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  return success_supported_states;
}

const std::vector<uint8_t>
DualModeController::Properties::GetLeWhiteListSize() {
  return std::vector<uint8_t>({kSuccessStatus, le_white_list_size_});
}

const std::vector<uint8_t> DualModeController::Properties::GetLeRand() {
  std::vector<uint8_t> success_rand_val;

  success_rand_val.push_back(kSuccessStatus);

  for (uint8_t i = 0; i < 8; ++i)
    success_rand_val.push_back(static_cast<uint8_t>(rand()));

  return success_rand_val;
}

const std::vector<uint8_t> DualModeController::Properties::GetLeVendorCap() {
  std::vector<uint8_t> success_vendor_cap = {kSuccessStatus,
                                             0x05,
                                             0x01,
                                             0x00,
                                             0x04,
                                             0x80,
                                             0x01,
                                             0x10,
                                             0x01,
                                             0x60,
                                             0x00,
                                             0x0a,
                                             0x00,
                                             0x01,
                                             0x01};

  return success_vendor_cap;
}

const std::vector<uint8_t> DualModeController::Properties::GetBufferSize() {
  return std::vector<uint8_t>(
      {kSuccessStatus,
       static_cast<uint8_t>(acl_data_packet_size_),
       static_cast<uint8_t>(acl_data_packet_size_ >> 8),
       sco_data_packet_size_,
       static_cast<uint8_t>(num_acl_data_packets_),
       static_cast<uint8_t>(num_acl_data_packets_ >> 8),
       static_cast<uint8_t>(num_sco_data_packets_),
       static_cast<uint8_t>(num_sco_data_packets_ >> 8)});
}

const std::vector<uint8_t>
DualModeController::Properties::GetLocalVersionInformation() {
  return std::vector<uint8_t>({kSuccessStatus,
                               version_,
                               static_cast<uint8_t>(revision_),
                               static_cast<uint8_t>(revision_ >> 8),
                               lmp_pal_version_,
                               static_cast<uint8_t>(manufacturer_name_),
                               static_cast<uint8_t>(manufacturer_name_ >> 8),
                               static_cast<uint8_t>(lmp_pal_subversion_),
                               static_cast<uint8_t>(lmp_pal_subversion_ >> 8)});
}

const std::vector<uint8_t> DualModeController::Properties::GetBdAddress() {
  return bd_address_;
}

const std::vector<uint8_t>
DualModeController::Properties::GetLocalExtendedFeatures(uint8_t page_number) {
  uint8_t maximum_page_number = 1;
  if (page_number == 0)
    return std::vector<uint8_t>({kSuccessStatus,
                                 page_number,
                                 maximum_page_number,
                                 0xFF,
                                 0xFF,
                                 0xFF,
                                 0xFF,
                                 0xFF,
                                 0xFF,
                                 0xFF,
                                 0xFF});
  else
    return std::vector<uint8_t>({kSuccessStatus,
                                 page_number,
                                 maximum_page_number,
                                 0x07,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00});
}

const std::vector<uint8_t>
DualModeController::Properties::GetLocalSupportedCommands() {
  std::vector<uint8_t> local_supported_commands;
  local_supported_commands.push_back(kSuccessStatus);
  for (uint8_t i = 0; i < local_supported_commands_size_; ++i)
    local_supported_commands.push_back(0xFF);
  return local_supported_commands;
}

const std::vector<uint8_t> DualModeController::Properties::GetLocalName() {
  std::vector<uint8_t> local_name;
  local_name.push_back(kSuccessStatus);
  for (uint8_t i = 0; i < local_name_size_; ++i)
    local_name.push_back(0xFF);
  return local_name;
}

// static
void DualModeController::Properties::RegisterJSONConverter(
    base::JSONValueConverter<DualModeController::Properties>* converter) {
// TODO(dennischeng): Use RegisterIntField() here?
#define REGISTER_UINT8_T(field_name, field) \
  converter->RegisterCustomField<uint8_t>(  \
      field_name, &DualModeController::Properties::field, &ParseUint8t);
#define REGISTER_UINT16_T(field_name, field) \
  converter->RegisterCustomField<uint16_t>(  \
      field_name, &DualModeController::Properties::field, &ParseUint16t);
  REGISTER_UINT16_T("AclDataPacketSize", acl_data_packet_size_);
  REGISTER_UINT8_T("ScoDataPacketSize", sco_data_packet_size_);
  REGISTER_UINT16_T("NumAclDataPackets", num_acl_data_packets_);
  REGISTER_UINT16_T("NumScoDataPackets", num_sco_data_packets_);
  REGISTER_UINT8_T("Version", version_);
  REGISTER_UINT16_T("Revision", revision_);
  REGISTER_UINT8_T("LmpPalVersion", lmp_pal_version_);
  REGISTER_UINT16_T("ManufacturerName", manufacturer_name_);
  REGISTER_UINT16_T("LmpPalSubversion", lmp_pal_subversion_);
#undef REGISTER_UINT8_T
#undef REGISTER_UINT16_T
}

}  // namespace test_vendor_lib
