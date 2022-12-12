/************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 * Author/Developer(s): Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 **************************************************************************/
#pragma once
#include <thread>
#include <variant>
#include <functional>
#include <boost/heap/binomial_heap.hpp>

#include <sisl/utility/enum.hpp>
#include <sisl/fds/buffer.hpp>

struct spdk_thread;
struct spdk_bdev_desc;
struct spdk_nvmf_qpair;

namespace iomgr {
////// Forward declarations
class IOReactor;
struct io_thread;
struct IODevice;
struct iomgr_msg;

/////////////////// Types for all IODevice ////////////////////////
typedef std::shared_ptr< IODevice > io_device_ptr;
typedef std::shared_ptr< const IODevice > io_device_const_ptr;
typedef std::function< bool(const io_device_const_ptr&) > iodev_selector_t;
typedef std::function< void(IODevice* iodev, void* cookie, int events) > ev_callback;

/////////////////// Types for all IOReactors ////////////////////////
typedef uint32_t reactor_idx_t;
typedef uint32_t io_thread_idx_t;
typedef uint32_t io_thread_addr_t;

typedef std::function< void(bool) > thread_state_notifier_t;
typedef std::variant< reactor_idx_t, spdk_thread* > backing_thread_t;
typedef std::shared_ptr< io_thread > io_thread_t;

ENUM(thread_regex, uint8_t,
     all_io,            // Represents all io threads
     least_busy_io,     // Represents least busy io thread (including worker + user)
     all_worker,        // Represents all worker io threads (either tloop or iloop)
     least_busy_worker, // Represents least busy worker io thread
     random_worker,     // Represents a random worker io thread
     all_user,          // Represents all user created io threads
     least_busy_user,   // Represents least busy user io thread
     all_tloop          // Represents all tight loop threads (could be either worker or user)
);
typedef uint32_t eal_core_id_t;
typedef std::variant< thread_regex, io_thread_t > thread_specifier;
typedef std::variant< std::thread, eal_core_id_t > sys_thread_id_t;

typedef std::variant< int, spdk_bdev_desc*, spdk_nvmf_qpair* > backing_dev_t;
typedef uint32_t poll_cb_idx_t;
typedef std::function< bool(const io_thread_t&) > can_backoff_cb_t;

/////////////////// Types for all Msghandlers /////////////////////
typedef std::function< void(iomgr_msg*) > msg_handler_t;
typedef void (*spdk_msg_signature_t)(void*);
typedef std::function< void(void) > run_on_closure_t;
typedef std::function< void(io_thread_addr_t) > run_method_t;
typedef uint32_t msg_module_id_t;

struct reschedule_data_t {
    io_device_ptr iodev;
    int event;
};
typedef std::variant< sisl::blob, reschedule_data_t, run_method_t > msg_data_t;

ENUM(wait_type_t, uint8_t, no_wait, sleep, spin, callback);

/////////////////// Types for all IOInterfaces ////////////////////////
class IOInterface;
typedef std::function< void(int64_t res, uint8_t* cookie) > io_interface_comp_cb_t;
typedef std::function< void(void) > listen_sentinel_cb_t;
typedef std::function< void(void) > interface_adder_t;
typedef std::function< void(const std::shared_ptr< IOInterface >&) > interface_cb_t;
typedef uint32_t io_interface_id_t;

ENUM(drive_type, uint8_t,
     file_on_nvme, // Works on top of file system which is hosted in NVMe
     file_on_hdd,  // Works on top of file system which is hosted in HDD
     block_nvme,   // Kernel NVMe block device
     block_hdd,    // Kernel HDD block device
     raw_nvme,     // Raw Nvme device (which can be opened only thru spdk)
     memory,       // Non-persistent memory
     spdk_bdev,    // A SDPK version of bdev
     unknown       // Try to deduce it while loading
)
typedef drive_type iomgr_drive_type;
} // namespace iomgr
