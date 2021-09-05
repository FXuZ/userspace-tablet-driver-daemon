/*
xp-pen-userland
Copyright (C) 2021 - Aren Villanueva <https://github.com/kurikaesu/>

This program is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <algorithm>
#include "xp_pen_handler.h"

xp_pen_handler::xp_pen_handler() {
    std::cout << "xp_pen_handler initialized" << std::endl;

    handledProducts.push_back(0x091b);
}

xp_pen_handler::~xp_pen_handler() {
    for (auto deviceInterfaces : deviceInterfaces) {
        cleanupDevice(deviceInterfaces);
    }
}

int xp_pen_handler::getVendorId() {
    return 0x28bd;
}

std::vector<int> xp_pen_handler::getProductIds() {
    return handledProducts;
}

bool xp_pen_handler::handleProductAttach(libusb_device* device, const libusb_device_descriptor descriptor) {
    std::cout << "xp_pen_handler" << std::endl;
    libusb_device_handle* handle = NULL;
    device_interface_pair* interfacePair = NULL;
    switch (descriptor.idProduct) {
        case 0x091b:
            std::cout << "Got known device" << std::endl;
            interfacePair = claimDevice(device, handle, descriptor);
            deviceInterfaces.push_back(interfacePair);
            deviceInterfaceMap[device] =interfacePair;

            return true;

        default:
            std::cout << "Unknown device" << std::endl;

            break;
    }

    return false;
}

void xp_pen_handler::handleProductDetach(libusb_device *device, struct libusb_device_descriptor descriptor) {
    for (auto deviceObj : deviceInterfaceMap) {
        if (deviceObj.first == device) {
            std::cout << "Handling device detach" << std::endl;
            cleanupDevice(deviceObj.second);
            libusb_close(deviceObj.second->deviceHandle);

            auto deviceInterfacesIterator = std::find(deviceInterfaces.begin(), deviceInterfaces.end(), deviceObj.second);
            if (deviceInterfacesIterator != deviceInterfaces.end()) {
                deviceInterfaces.erase(deviceInterfacesIterator);
            }

            auto deviceMapIterator = std::find(deviceInterfaceMap.begin(), deviceInterfaceMap.end(), deviceObj);
            if (deviceMapIterator != deviceInterfaceMap.end()) {
                deviceInterfaceMap.erase(deviceMapIterator);
            }

            break;
        }
    }
}

device_interface_pair* xp_pen_handler::claimDevice(libusb_device *device, libusb_device_handle *handle, const libusb_device_descriptor descriptor) {
    device_interface_pair* deviceInterface = new device_interface_pair();
    int err;

    struct libusb_config_descriptor* configDescriptor;
    err = libusb_get_config_descriptor(device, 0, &configDescriptor);
    if (err != LIBUSB_SUCCESS) {
        std::cout << "Could not get config descriptor" << std::endl;
    }

    if ((err = libusb_open(device, &handle)) == LIBUSB_SUCCESS) {
        deviceInterface->deviceHandle = handle;
        unsigned char interfaceCount = configDescriptor->bNumInterfaces;

        for (unsigned char interface_number = 0; interface_number < interfaceCount; ++interface_number) {
            err = libusb_detach_kernel_driver(handle, interface_number);
            if (LIBUSB_SUCCESS == err) {
                std::cout << "Detached interface from kernel " << interface_number << std::endl;
                deviceInterface->detachedInterfaces.push_back(interface_number);
            }

            if (libusb_claim_interface(handle, interface_number) == LIBUSB_SUCCESS) {
                std::cout << "Claimed interface " << interface_number << std::endl;
                deviceInterface->claimedInterfaces.push_back(interface_number);

                unsigned char interface_target = interface_number;
                /*
                if (configDescriptor->interface[interface_number].num_altsetting > 0) {
                    std::cout << "Interface " << interface_number << " has " << configDescriptor->interface[interface_number].num_altsetting << " alt settings" << std::endl;

                    // Try to set alt setting
                    if (libusb_set_interface_alt_setting(handle, interface_number, configDescriptor->interface[interface_number].altsetting[0].bAlternateSetting) != LIBUSB_SUCCESS) {
                        std::cout << "Could not set alt setting on interface " << interface_number << std::endl;
                    }

                    interface_target = configDescriptor->interface[interface_number].altsetting[0].bInterfaceNumber;
                    std::cout << "Interface target set to " << (int)interface_target << std::endl;
                }
                 */

                if (!setupReportProtocol(handle, interface_target) ||
                    !setupInfiniteIdle(handle, interface_target)) {
                    continue;
                }

                sendInitKey(handle, interface_target);
                setupTransfers(handle, interface_target, descriptor.bMaxPacketSize0);

                std::cout << "Setup completed on interface " << interface_number << std::endl;
            }
        }
    } else {
        std::cout << "libusb_open returned error " << err << std::endl;
    }

    return deviceInterface;
}

void xp_pen_handler::cleanupDevice(device_interface_pair *pair) {
    for (auto interface: pair->claimedInterfaces) {
        libusb_release_interface(pair->deviceHandle, interface);
        std::cout << "Releasing interface " << interface << std::endl;
    }

    for (auto interface: pair->detachedInterfaces) {
        libusb_attach_kernel_driver(pair->deviceHandle, interface);
        std::cout << "Reattaching to kernel interface " << interface << std::endl;
    }
}

void xp_pen_handler::sendInitKey(libusb_device_handle *handle, int interface_number) {
    unsigned char key[] = {0x02, 0xb0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int sentBytes;
    int ret = libusb_interrupt_transfer(handle, interface_number | LIBUSB_ENDPOINT_OUT, key, sizeof(key), &sentBytes, 1000);
    if (ret != LIBUSB_SUCCESS) {
        std::cout << "Failed to send key on interface " << interface_number << " ret: " << ret << " errno: " << errno << std::endl;
        return;
    }

    if (sentBytes != sizeof(key)) {
        std::cout << "Didn't send all of the key on interface " << interface_number << " only sent " << sentBytes << std::endl;
        return;
    }
}

bool xp_pen_handler::setupTransfers(libusb_device_handle *handle, unsigned char interface_number, int maxPacketSize) {
    std::cout << "Setting up transfers with max packet size of " << maxPacketSize << std::endl;
    struct libusb_transfer* transfer = libusb_alloc_transfer(0);
    if (transfer == NULL) {
        std::cout << "Could not allocate a transfer for interface " << interface_number << std::endl;
        return false;
    }

    transfer->user_data = NULL;
    unsigned char* buff = new unsigned char[maxPacketSize];
    libusb_fill_interrupt_transfer(transfer,
                                   handle, interface_number | LIBUSB_ENDPOINT_IN,
                                   buff, maxPacketSize,
                                   transferCallback, NULL,
                                   1000);

    transfer->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
    int ret = libusb_submit_transfer(transfer);
    if (ret != LIBUSB_SUCCESS) {
        std::cout << "Could not submit transfer on interface " << interface_number << " ret: " << ret << " errno: " << errno << std::endl;
        return false;
    }

    std::cout << "Set up transfer for interface " << interface_number << std::endl;

    return true;
}

void xp_pen_handler::transferCallback(struct libusb_transfer *transfer) {
    int err;
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        for (int i = 0 ; i < transfer->actual_length; ++i) {
            std::cout << std::hex << transfer->buffer[i];
        }

        std::cout << std::endl;

        // Resubmit the transfer
        err = libusb_submit_transfer(transfer);
        if (err != LIBUSB_SUCCESS) {
            std::cout << "Could not resubmit my transfer" << std::endl;
        }

        break;

    default:
        break;
    }
}
