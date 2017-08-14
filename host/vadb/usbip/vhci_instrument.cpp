/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <fstream>
#include <sstream>
#include <glog/logging.h>
#include "common/libs/fs/shared_select.h"

#include "common/libs/fs/shared_fd.h"
#include "host/vadb/usbip/vhci_instrument.h"

namespace vadb {
namespace usbip {
namespace {
// Device ID is specified as a concatenated pair of BUS and DEVICE id.
// Since we only export one device and our server doesn't care much about
// its number, we use the default value of BUS=1 and DEVICE=1.
// This can be set to something else and should still work, as long as
// numbers are valid in USB sense.
constexpr uint32_t kDefaultDeviceID = (1 << 16) | 1;
constexpr uint32_t kDefaultDeviceSpeed = 2;
// Subsystem and device type where VHCI driver is located.
// These values can usually be found after loading vhci-hcd module here:
// /sys/devices/platform/vhci_hcd/modalias
constexpr char kVHCISubsystem[] = "platform";
constexpr char kVHCIDevType[] = "vhci_hcd";

// Port status values deducted from /sys/devices/platform/vhci_hcd/status
enum {
  // kVHCIPortFree indicates the port is not currently in use.
  kVHCIStatusPortFree = 4
};
}  // anonymous namespace

VHCIInstrument::VHCIInstrument(const std::string& name)
    : udev_(nullptr, [](udev* u) { udev_unref(u); }),
      vhci_device_(nullptr,
                   [](udev_device* device) { udev_device_unref(device); }),
      name_(name) {}

bool VHCIInstrument::Init() {
  wake_event_ = avd::SharedFD::Event(0, 0);

  udev_.reset(udev_new());
  CHECK(udev_) << "Could not create libudev context.";

  vhci_device_.reset(udev_device_new_from_subsystem_sysname(udev_.get(),
                                                            kVHCISubsystem,
                                                            kVHCIDevType));
  if (!vhci_device_) {
    LOG(ERROR) << "VHCI not available. Is the driver loaded?";
    LOG(ERROR) << "Try: sudo modprobe vhci_hcd";
    LOG(ERROR) << "The driver is part of linux-image-extra-`uname -r` package";
    return false;
  }

  syspath_ = udev_device_get_syspath(vhci_device_.get());

  if (!FindFreePort()) {
    LOG(ERROR) << "It appears all your VHCI ports are currently occupied.";
    LOG(ERROR) << "New VHCI device cannot be registered unless one of the "
               << "ports is freed.";
    return false;
  }

  attach_thread_.reset(new std::thread([this]() {
    AttachThread(); }));
  return true;
}

bool VHCIInstrument::FindFreePort() {
  std::ifstream stat(syspath_ + "/status");
  int port;
  int status;
  std::string everything_else;

  if (!stat.is_open()) {
    LOG(ERROR) << "Could not open usb-ip status file.";
    return false;
  }

  // Skip past the header line.
  std::getline(stat, everything_else);

  while (stat.rdstate() == std::ios_base::goodbit) {
    stat >> port >> status;
    std::getline(stat, everything_else);
    if (status == kVHCIStatusPortFree) {
      port_ = port;
      LOG(INFO) << "Using VHCI port " << port_;
      return true;
    }
  }
  return false;
}

void VHCIInstrument::TriggerAttach() {
  uint64_t count = 1;
  wake_event_->Write(&count, sizeof(count));
}

void VHCIInstrument::AttachThread() {
  avd::SharedFDSet rset;
  // If we're attempting connection, make sure to re-try every second until
  // we're successful.
  timeval period = {1, 0};
  // Trigger attach upon start.
  bool want_attach = true;

  while (true) {
    rset.Zero();
    rset.Set(wake_event_);
    // Wait until poked.
    if (0 != avd::Select(&rset, nullptr, nullptr,
                         (want_attach ? &period : nullptr))) {
      uint64_t ignore;
      wake_event_->Read(&ignore, sizeof(ignore));
      LOG(INFO) << "Attach triggered.";
      want_attach = true;
    }

    // Make an attempt to re-attach. If successful, clear pending attach flag.
    if (Attach()) {
      want_attach = false;
    } else {
      LOG(WARNING) << "Attach failed.";
      sleep(1);
    }
  }
}

bool VHCIInstrument::Attach() {
  avd::SharedFD socket =
      avd::SharedFD::SocketLocalClient(name_.c_str(), true, SOCK_STREAM);
  if (!socket->IsOpen()) return false;
  int dup_fd = socket->UNMANAGED_Dup();

  std::stringstream result;
  result << port_ << ' ' << dup_fd << ' ' << kDefaultDeviceID << ' ' <<
         kDefaultDeviceSpeed;
  std::ofstream attach(syspath_ + "/attach");

  if (!attach.is_open()) {
    LOG(WARNING) << "Could not open VHCI attach file.";
    return false;
  }
  attach << result.str();

  // It is unclear whether duplicate FD should remain open or not. There are
  // cases supporting both assumptions, likely related to kernel version.
  // Kernel 4.10 is having problems communicating with USB/IP server if the
  // socket is closed after it's passed to kernel. It is a clear indication that
  // the kernel requires the socket to be kept open.
  return attach.rdstate() == std::ios_base::goodbit;
}

}  // namespace usbip
}  // namespace vadb