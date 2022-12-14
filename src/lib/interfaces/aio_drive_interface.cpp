/************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 * Author/Developer(s): Rishabh Mittal
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
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <fmt/format.h>
#include <linux/fs.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#endif

#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include <folly/Exception.h>
#include "iomgr_config.hpp"
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include <sisl/logging/logging.h>

// TODO: Remove this once the problem is fixed in flip
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <sisl/flip/flip.hpp>
#if defined __clang__ or defined __GNUC__
#pragma GCC diagnostic pop
#endif

#include "include/iomgr.hpp"
#include "include/aio_drive_interface.hpp"
#include "include/iomgr_config.hpp"

namespace iomgr {
#ifdef __APPLE__

ssize_t preadv(int fd, const iovec* iov, int iovcnt, off_t offset) {
    lseek(fd, offset, SEEK_SET);
    return ::readv(fd, iov, iovcnt);
}

ssize_t pwritev(int fd, const iovec* iov, int iovcnt, off_t offset) {
    lseek(fd, offset, SEEK_SET);
    return ::writev(fd, iov, iovcnt);
}

#endif
using namespace std;

thread_local aio_thread_context* AioDriveInterface::t_aio_ctx;
std::vector< int > AioDriveInterface::s_poll_interval_table;

AioDriveInterface::AioDriveInterface(const io_interface_comp_cb_t& cb) : KernelDriveInterface(cb) {
    init_poll_interval_table();
}

AioDriveInterface::~AioDriveInterface() {}

io_device_ptr AioDriveInterface::open_dev(const std::string& devname, drive_type dev_type, int oflags) {
    std::lock_guard lock{m_open_mtx};
    LOGMSG_ASSERT(((dev_type == drive_type::block_nvme) || (dev_type == drive_type::block_hdd) ||
                   (dev_type == drive_type::file_on_hdd) || (dev_type == drive_type::file_on_nvme)),
                  "Unexpected dev type to open {}", dev_type);
    init_write_zero_buf(devname, dev_type);

    auto fd = ::open(devname.c_str(), oflags, 0640);
    if (fd == -1) {
        folly::throwSystemError(fmt::format("Unable to open the device={} dev_type={}, errno={} strerror={}", devname,
                                            dev_type, errno, strerror(errno)));
        return nullptr;
    }

    auto iodev = alloc_io_device(backing_dev_t(fd), 9 /* pri */, thread_regex::all_io);
    iodev->devname = devname;
    iodev->creator = iomanager.am_i_io_reactor() ? iomanager.iothread_self() : nullptr;
    iodev->dtype = dev_type;

    // We don't need to add the device to each thread, because each AioInterface thread context add an
    // event fd and read/write use this device fd to control with iocb.
    LOGINFO("Device={} of type={} opened with flags={} successfully, fd={}", devname, dev_type, oflags, fd);
    return iodev;
}

void AioDriveInterface::close_dev(const io_device_ptr& iodev) {
    // TODO: This is where we would wait for any outstanding io's to complete

    IOInterface::close_dev(iodev);

    // AIO base devices are not added to any poll list, so it can be closed as is.
    ::close(iodev->fd());
    iodev->clear();
}

void AioDriveInterface::init_iface_thread_ctx([[maybe_unused]] const io_thread_t& thr) {
    // TODO: Ideally we need to move this aio_thread_context from thread_local to IOInterface::thread_local
    // so that we can support multiple AioDriveInterfaces within the thread if needed.
    t_aio_ctx = new aio_thread_context();
    t_aio_ctx->ev_fd = eventfd(0, EFD_NONBLOCK);
    t_aio_ctx->ev_io_dev =
        iomanager.generic_interface()->make_io_device(backing_dev_t(t_aio_ctx->ev_fd), EPOLLIN, 0, nullptr, true,
                                                      bind_this(AioDriveInterface::on_event_notification, 3));

    int err = io_setup(MAX_OUTSTANDING_IO, &t_aio_ctx->ioctx);
    if (err) {
        LOGCRITICAL("io_setup failed with ret status {} errno {}", err, errno);
        folly::throwSystemError(fmt::format("io_setup failed with ret status {} errno {}", err, errno));
    }

    t_aio_ctx->iocb_info_prealloc(MAX_OUTSTANDING_IO);
    t_aio_ctx->poll_cb_idx =
        iomanager.this_reactor()->register_poll_interval_cb(bind_this(AioDriveInterface::handle_completions, 0));
}

void AioDriveInterface::clear_iface_thread_ctx([[maybe_unused]] const io_thread_t& thr) {
    iomanager.this_reactor()->unregister_poll_interval_cb(t_aio_ctx->poll_cb_idx);

    int err = io_destroy(t_aio_ctx->ioctx);
    if (err) { LOGERROR("io_destroy failed with ret status={} errno={}", err, errno); }

    iomanager.generic_interface()->remove_io_device(t_aio_ctx->ev_io_dev);
    close(t_aio_ctx->ev_fd);
    delete t_aio_ctx;
}

void AioDriveInterface::on_event_notification(IODevice* iodev, [[maybe_unused]] void* cookie,
                                              [[maybe_unused]] int event) {
    assert(iodev->fd() == t_aio_ctx->ev_fd);

    uint64_t temp = 0;
    [[maybe_unused]] auto rsize = read(t_aio_ctx->ev_fd, &temp, sizeof(uint64_t));

    LOGTRACEMOD(iomgr, "Received completion on fd = {} ev_fd = {}", iodev->fd(), t_aio_ctx->ev_fd);
    handle_completions();
}

void AioDriveInterface::handle_completions() {
    auto& tmetrics = iomanager.this_thread_metrics();

    const int nevents = io_getevents(t_aio_ctx->ioctx, 0, MAX_COMPLETIONS, t_aio_ctx->events, NULL);
    ++tmetrics.io_callbacks;

    if (nevents == 0) {
        return;
    } else if (nevents < 0) {
        /* TODO: Handle error by reporting failures */
        LOGERROR("process_completions nevents is less then zero {}", nevents);
        COUNTER_INCREMENT(m_metrics, completion_errors, 1);
    } else {
        tmetrics.aio_events_in_callback += nevents;
    }

    for (int i = 0; i < nevents; ++i) {
        auto& e = t_aio_ctx->events[i];

        auto ret = static_cast< int64_t >(e.res);
        auto iocb = (struct iocb*)e.obj;
        auto info = (iocb_info_t*)iocb;

        LOGTRACEMOD(iomgr, "Event[{}]: Result {} res2={}", i, e.res, e.res2);
#ifdef _PRERELEASE
        auto flip_resubmit_cnt = flip::Flip::instance().get_test_flip< int >("read_write_resubmit_io");
        if (flip_resubmit_cnt != boost::none && info->resubmit_cnt < (uint32_t)flip_resubmit_cnt.get()) { e.res = 0; }
#endif
        if (ret < 0) {
            COUNTER_INCREMENT(m_metrics, completion_errors, 1);
            LOGDFATAL("Error in completion of aio, result: {} info: {}", ret, info->to_string());
        } else if ((e.res != info->size) || e.res2) {
            COUNTER_INCREMENT(m_metrics, completion_errors, 1);
            LOGERROR("io is not completed properly. size read/written {} info {} error {}", e.res, info->to_string(),
                     e.res2);
            if (e.res2 == 0) { e.res2 = EIO; }
            if (resubmit_iocb_on_err(iocb)) { continue; }
        }

        auto user_cookie = (uint8_t*)iocb->data;
        t_aio_ctx->dec_submitted_aio();
        t_aio_ctx->free_iocb(iocb);
        retry_io();
        if (m_comp_cb) { m_comp_cb(e.res2, user_cookie); }
    }
}

bool AioDriveInterface::resubmit_iocb_on_err(struct iocb* iocb) {
    auto info = (iocb_info_t*)iocb;
    if (info->resubmit_cnt > IM_DYNAMIC_CONFIG(max_resubmit_cnt)) { return false; }
    ++info->resubmit_cnt;
    t_aio_ctx->prep_iocb_for_resubmit(iocb);
    auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
    COUNTER_INCREMENT(m_metrics, resubmit_io_on_err, 1);
    if (ret != 1) { handle_io_failure(iocb); }
    return true;
}

void AioDriveInterface::async_write(IODevice* iodev, const char* data, uint32_t size, uint64_t offset, uint8_t* cookie,
                                    bool part_of_batch) {
    if (!t_aio_ctx->can_submit_aio()) {
        auto iocb = t_aio_ctx->prep_iocb(false, iodev->fd(), false, data, size, offset, cookie);
        push_retry_list(iocb, true /* no_slot */);
        return;
    }

    if (part_of_batch && t_aio_ctx->can_be_batched(0)) {
        t_aio_ctx->prep_iocb(true /* batch_io */, iodev->fd(), false /* is_read */, data, size, offset, cookie);
    } else {
        auto iocb = t_aio_ctx->prep_iocb(false, iodev->fd(), false, data, size, offset, cookie);
        auto& metrics = iomanager.this_thread_metrics();
        ++metrics.iface_io_batch_count;
        ++metrics.iface_io_actual_count;

        auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
        t_aio_ctx->inc_submitted_aio(ret);
        if (ret != 1) {
            handle_io_failure(iocb);
            return;
        }
    }
}

void AioDriveInterface::async_read(IODevice* iodev, char* data, uint32_t size, uint64_t offset, uint8_t* cookie,
                                   bool part_of_batch) {
    if (!t_aio_ctx->can_submit_aio()) {
        auto iocb = t_aio_ctx->prep_iocb(false, iodev->fd(), true, data, size, offset, cookie);
        push_retry_list(iocb, true /* no_slot */);
        return;
    }
    if (part_of_batch && t_aio_ctx->can_be_batched(0)) {
        t_aio_ctx->prep_iocb(true /* batch_io */, iodev->fd(), true /* is_read */, data, size, offset, cookie);
    } else {
        auto iocb = t_aio_ctx->prep_iocb(false, iodev->fd(), true, data, size, offset, cookie);
        auto& metrics = iomanager.this_thread_metrics();
        ++metrics.iface_io_batch_count;
        ++metrics.iface_io_actual_count;

        auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
        t_aio_ctx->inc_submitted_aio(ret);
        if (ret != 1) {
            handle_io_failure(iocb);
            return;
        }
    }
}

void AioDriveInterface::async_writev(IODevice* iodev, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset,
                                     uint8_t* cookie, bool part_of_batch) {
    if (!t_aio_ctx->can_submit_aio()
#ifdef _PRERELEASE
        || flip::Flip::instance().test_flip("io_write_iocb_empty_flip")
#endif
    ) {
        auto iocb = t_aio_ctx->prep_iocb_v(false, iodev->fd(), false, iov, iovcnt, size, offset, cookie);
        push_retry_list(iocb, true /* no_slot */);
        return;
    }
    if (part_of_batch && t_aio_ctx->can_be_batched(iovcnt)) {
        t_aio_ctx->prep_iocb_v(true /* batch_io */, iodev->fd(), false /* is_read */, iov, iovcnt, size, offset,
                               cookie);
    } else {
        auto iocb = t_aio_ctx->prep_iocb_v(false, iodev->fd(), false, iov, iovcnt, size, offset, cookie);
        auto& metrics = iomanager.this_thread_metrics();
        ++metrics.iface_io_batch_count;
        ++metrics.iface_io_actual_count;

        auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
        t_aio_ctx->inc_submitted_aio(ret);
        if (ret != 1) {
            handle_io_failure(iocb);
            return;
        }
    }
}

void AioDriveInterface::async_readv(IODevice* iodev, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset,
                                    uint8_t* cookie, bool part_of_batch) {
    if (!t_aio_ctx->can_submit_aio()
#ifdef _PRERELEASE
        || flip::Flip::instance().test_flip("io_read_iocb_empty_flip")
#endif
    ) {
        auto iocb = t_aio_ctx->prep_iocb_v(false, iodev->fd(), true, iov, iovcnt, size, offset, cookie);
        push_retry_list(iocb, true /* no_slot */);
        return;
    }

    if (part_of_batch && t_aio_ctx->can_be_batched(iovcnt)) {
        t_aio_ctx->prep_iocb_v(true /* batch_io */, iodev->fd(), true /* is_read */, iov, iovcnt, size, offset, cookie);
    } else {
        auto iocb = t_aio_ctx->prep_iocb_v(false, iodev->fd(), true, iov, iovcnt, size, offset, cookie);
        auto& metrics = iomanager.this_thread_metrics();
        ++metrics.iface_io_batch_count;
        ++metrics.iface_io_actual_count;

        auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
        t_aio_ctx->inc_submitted_aio(ret);
        if (ret != 1) {
            handle_io_failure(iocb);
            return;
        }
    }
}

void AioDriveInterface::async_unmap(IODevice* iodev, uint32_t size, uint64_t offset, uint8_t* cookie,
                                    bool part_of_batch) {}

void AioDriveInterface::submit_batch() {
    auto ibatch = t_aio_ctx->move_cur_batch();
    LOGTRACEMOD(iomgr, "submit pending batch n_iocbs={}", ibatch.n_iocbs);
    if (ibatch.n_iocbs == 0) { return; } // No batch to submit

    auto& metrics = iomanager.this_thread_metrics();
    ++metrics.iface_io_batch_count;

    auto n_issued = io_submit(t_aio_ctx->ioctx, ibatch.n_iocbs, ibatch.get_iocb_list());
    if (n_issued < 0) { n_issued = 0; }
    metrics.iface_io_actual_count += n_issued;
    t_aio_ctx->inc_submitted_aio(n_issued);

    // For those which we are not able to issue, convert that to sync io
    auto n_iocbs = ibatch.n_iocbs;
    for (auto i = n_issued; i < n_iocbs; ++i) {
        auto info = ibatch.iocb_info[i];
        auto iocb = (struct iocb*)info;
        handle_io_failure(iocb);
    }
}

void AioDriveInterface::retry_io() {
    struct iocb* iocb = nullptr;
    while ((t_aio_ctx->can_submit_aio()) && (iocb = t_aio_ctx->pop_retry_list()) != nullptr) {
        COUNTER_DECREMENT(m_metrics, retry_list_size, 1);
        auto ret = io_submit(t_aio_ctx->ioctx, 1, &iocb);
        t_aio_ctx->inc_submitted_aio(ret);
        if (ret != 1 && handle_io_failure(iocb)) { break; }
    }
}

void AioDriveInterface::push_retry_list(struct iocb* iocb, const bool no_slot) {
    auto info = (iocb_info_t*)iocb;
    COUNTER_INCREMENT_IF_ELSE(m_metrics, no_slot, queued_aio_slots_full, retry_io_eagain_error, 1);
    COUNTER_INCREMENT(m_metrics, retry_list_size, 1);
    LOGDEBUGMOD(iomgr, "adding io into retry list: {}", info->to_string());
    t_aio_ctx->push_retry_list(iocb);
    if (!t_aio_ctx->timer_set) {
        t_aio_ctx->timer_set = true;
        iomanager.schedule_thread_timer(IM_DYNAMIC_CONFIG(aio->retry_timeout), false, nullptr, [this](void* cookie) {
            t_aio_ctx->timer_set = false;
            retry_io();
        });
    }
}

bool AioDriveInterface::handle_io_failure(struct iocb* iocb) {
    auto info = (iocb_info_t*)iocb;
    bool ret = true;

    if (errno == EAGAIN) {
        push_retry_list(iocb, false /* no_slot */);
    } else {
        LOGERROR("io submit fail: io info: {}, errno: {}", info->to_string(), errno);
        COUNTER_INCREMENT_IF_ELSE(m_metrics, info->is_read, read_io_submission_errors, write_io_submission_errors, 1);
        ret = false;
        t_aio_ctx->free_iocb(iocb);
        if (m_comp_cb) m_comp_cb(errno, (uint8_t*)iocb->data);
    }
    return ret;
}

void AioDriveInterface::init_poll_interval_table() {
    s_poll_interval_table.clear();
    s_poll_interval_table.push_back(IM_DYNAMIC_CONFIG(poll.force_wakeup_by_time_ms));
    auto tight_loop_after_ios{IM_DYNAMIC_CONFIG(poll.tight_loop_after_io_max)};

    // TODO: For now we are not putting the decay factor, but eventually timeout
    // will start high for lower outstanding ios and decay towards max ios
    int t{s_poll_interval_table[0]};
    for (uint32_t i{1}; i < tight_loop_after_ios; ++i) {
        s_poll_interval_table.push_back(t);
    }
    s_poll_interval_table.push_back(0); // Tight loop from here on
}

/////////////////////////// aio_thread_context /////////////////////////////////////////////////
void aio_thread_context::dec_submitted_aio() {
    --submitted_aio;
    iomanager.this_reactor()->set_poll_interval((submitted_aio >= AioDriveInterface::s_poll_interval_table.size())
                                                    ? 0
                                                    : AioDriveInterface::s_poll_interval_table[submitted_aio]);
}

void aio_thread_context::inc_submitted_aio(int count) {
    if (count < 0) { return; }
    submitted_aio += count;

    iomanager.this_reactor()->set_poll_interval(submitted_aio >= AioDriveInterface::s_poll_interval_table.size()
                                                    ? 0
                                                    : AioDriveInterface::s_poll_interval_table[submitted_aio]);
}
} // namespace iomgr
