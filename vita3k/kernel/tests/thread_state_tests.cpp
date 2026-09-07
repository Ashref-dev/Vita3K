// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

namespace {

int run_primitive_timeout_with_lifecycle_wait() {
    KernelState kernel;
    MemState mem;
    constexpr SceUID thread_id = 1;
    const auto thread = std::make_shared<ThreadState>(thread_id, kernel, mem);
    {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.threads.emplace(thread_id, thread);
    }
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        thread->status = ThreadStatus::run;
    }

    const SceUID semaphore_id = semaphore_create(kernel, "kernel-test", "timeout", thread_id, 0, 0, 1);
    if (semaphore_id < 0)
        return 1;

    std::atomic_bool lifecycle_wait_started = false;
    std::thread lifecycle_wait([&] {
        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        lifecycle_wait_started.store(true, std::memory_order_release);
        thread->status_cond.wait(thread_lock, [&] {
            return thread->status == ThreadStatus::dormant;
        });
    });

    while (!lifecycle_wait_started.load(std::memory_order_acquire))
        std::this_thread::yield();
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
    }

    SceUInt32 timeout = 1'000;
    const SceInt32 wait_result = semaphore_wait(kernel, "kernel-test", thread_id, semaphore_id, 1, &timeout);

    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        thread->status = ThreadStatus::dormant;
        thread->status_cond.notify_all();
    }
    lifecycle_wait.join();

    return wait_result == SCE_KERNEL_ERROR_WAIT_TIMEOUT ? 0 : 1;
}

int run_lifecycle_wake_cleans_primitive_queue() {
    KernelState kernel;
    MemState mem;
    constexpr SceUID thread_id = 1;
    const auto thread = std::make_shared<ThreadState>(thread_id, kernel, mem);
    {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.threads.emplace(thread_id, thread);
    }
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        thread->status = ThreadStatus::run;
    }

    const SceUID semaphore_id = semaphore_create(kernel, "kernel-test", "lifecycle", thread_id, 0, 0, 1);
    if (semaphore_id < 0)
        return 1;
    const auto semaphore = kernel.semaphores.at(semaphore_id);

    SceInt32 wait_result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    SceUInt32 timeout = 100'000;
    std::thread primitive_wait([&] {
        wait_result = semaphore_wait(kernel, "kernel-test", thread_id, semaphore_id, 1, &timeout);
    });

    while (true) {
        const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
        if (!semaphore->waiting_threads->empty())
            break;
        std::this_thread::yield();
    }

    thread->exit_delete();
    primitive_wait.join();

    const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
    return wait_result == SCE_KERNEL_OK && semaphore->waiting_threads->empty() ? 0 : 1;
}

int run_stale_semaphore_waiter_preserves_signal() {
    KernelState kernel;
    MemState mem;
    constexpr SceUID thread_id = 1;
    const auto thread = std::make_shared<ThreadState>(thread_id, kernel, mem);
    {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.threads.emplace(thread_id, thread);
    }
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        thread->status = ThreadStatus::run;
    }

    const SceUID semaphore_id = semaphore_create(kernel, "kernel-test", "stale", thread_id, 0, 0, 1);
    const auto semaphore = kernel.semaphores.at(semaphore_id);
    WaitingThreadData data{};
    data.thread = thread;
    data.priority = thread->priority;
    data.signal = 1;
    {
        const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
        semaphore->waiting_threads->push(data);
    }

    if (semaphore_signal(kernel, "kernel-test", thread_id, semaphore_id, 1) != SCE_KERNEL_OK)
        return 1;
    const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
    return semaphore->val == 1 && semaphore->waiting_threads->empty() ? 0 : 1;
}

ThreadStatePtr make_thread(KernelState &kernel, MemState &mem, SceUID thread_id, ThreadStatus status) {
    auto thread = std::make_shared<ThreadState>(thread_id, kernel, mem);
    thread->status = status;
    kernel.threads.emplace(thread_id, thread);
    return thread;
}

WaitingThreadData make_waiter(const ThreadStatePtr &thread) {
    WaitingThreadData data{};
    data.thread = thread;
    data.priority = thread->priority;
    return data;
}

} // namespace

TEST(ThreadStateTest, primitive_timeout_does_not_conflict_with_lifecycle_wait) {
    ASSERT_EXIT(
        std::_Exit(run_primitive_timeout_with_lifecycle_wait()),
        testing::ExitedWithCode(0), "");
}

TEST(ThreadStateTest, lifecycle_wake_removes_thread_from_primitive_queue) {
    ASSERT_EXIT(
        std::_Exit(run_lifecycle_wake_cleans_primitive_queue()),
        testing::ExitedWithCode(0), "");
}

TEST(ThreadStateTest, stale_semaphore_waiter_does_not_consume_signal) {
    ASSERT_EXIT(
        std::_Exit(run_stale_semaphore_waiter_preserves_signal()),
        testing::ExitedWithCode(0), "");
}

TEST(ThreadStateTest, condvar_signal_any_wakes_one_thread) {
    KernelState kernel;
    MemState mem;
    const auto first = make_thread(kernel, mem, 1, ThreadStatus::wait);
    const auto second = make_thread(kernel, mem, 2, ThreadStatus::wait);

    SceUID mutex_id = 0;
    ASSERT_EQ(mutex_create(&mutex_id, kernel, mem, "kernel-test", "mutex", first->id, 0, 0, {}, SyncWeight::Heavy), SCE_KERNEL_OK);
    SceUID condvar_id = 0;
    ASSERT_EQ(condvar_create(&condvar_id, kernel, "kernel-test", "condvar", first->id, 0, mutex_id, SyncWeight::Heavy), SCE_KERNEL_OK);
    const auto condvar = kernel.condvars.at(condvar_id);
    {
        const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
        condvar->waiting_threads->push(WaitingThreadData{ .thread = first, .priority = first->priority });
        condvar->waiting_threads->push(WaitingThreadData{ .thread = second, .priority = second->priority });
    }

    EXPECT_EQ(condvar_signal(kernel, "kernel-test", first->id, condvar_id, Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Heavy), SCE_KERNEL_OK);
    const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
    EXPECT_EQ(condvar->waiting_threads->size(), 1);
    EXPECT_NE(first->status == ThreadStatus::run, second->status == ThreadStatus::run);
}

TEST(ThreadStateTest, condvar_signal_any_skips_stale_waiters) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);
    const auto waiting = make_thread(kernel, mem, 2, ThreadStatus::wait);

    SceUID mutex_id = 0;
    ASSERT_EQ(mutex_create(&mutex_id, kernel, mem, "kernel-test", "mutex", stale->id, 0, 0, {}, SyncWeight::Heavy), SCE_KERNEL_OK);
    SceUID condvar_id = 0;
    ASSERT_EQ(condvar_create(&condvar_id, kernel, "kernel-test", "condvar", stale->id, 0, mutex_id, SyncWeight::Heavy), SCE_KERNEL_OK);
    const auto condvar = kernel.condvars.at(condvar_id);
    {
        const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
        condvar->waiting_threads->push(WaitingThreadData{ .thread = stale, .priority = stale->priority });
        condvar->waiting_threads->push(WaitingThreadData{ .thread = waiting, .priority = waiting->priority });
    }

    EXPECT_EQ(condvar_signal(kernel, "kernel-test", stale->id, condvar_id, Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Heavy), SCE_KERNEL_OK);
    const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
    EXPECT_TRUE(condvar->waiting_threads->empty());
    EXPECT_EQ(waiting->status, ThreadStatus::run);
}

TEST(ThreadStateTest, condvar_signal_specific_removes_stale_waiter) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);

    SceUID mutex_id = 0;
    ASSERT_EQ(mutex_create(&mutex_id, kernel, mem, "kernel-test", "mutex", stale->id, 0, 0, {}, SyncWeight::Heavy), SCE_KERNEL_OK);
    SceUID condvar_id = 0;
    ASSERT_EQ(condvar_create(&condvar_id, kernel, "kernel-test", "condvar", stale->id, 0, mutex_id, SyncWeight::Heavy), SCE_KERNEL_OK);
    const auto condvar = kernel.condvars.at(condvar_id);
    {
        const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
        condvar->waiting_threads->push(WaitingThreadData{ .thread = stale, .priority = stale->priority });
    }

    EXPECT_EQ(condvar_signal(kernel, "kernel-test", stale->id, condvar_id, Condvar::SignalTarget(Condvar::SignalTarget::Type::Specific, stale->id), SyncWeight::Heavy), SCE_KERNEL_OK);
    const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
    EXPECT_TRUE(condvar->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_mutex_waiter_does_not_receive_ownership) {
    KernelState kernel;
    MemState mem;
    const auto owner = make_thread(kernel, mem, 1, ThreadStatus::run);
    const auto stale = make_thread(kernel, mem, 2, ThreadStatus::run);

    SceUID mutex_id = 0;
    ASSERT_EQ(mutex_create(&mutex_id, kernel, mem, "kernel-test", "mutex", owner->id, 0, 1, {}, SyncWeight::Heavy), SCE_KERNEL_OK);
    const auto mutex = kernel.mutexes.at(mutex_id);
    {
        const std::lock_guard<std::mutex> mutex_lock(mutex->mutex);
        auto data = make_waiter(stale);
        data.lock_count = 1;
        mutex->waiting_threads->push(data);
    }

    EXPECT_EQ(mutex_unlock(kernel, "kernel-test", owner->id, mutex_id, 1, SyncWeight::Heavy), SCE_KERNEL_OK);

    const std::lock_guard<std::mutex> mutex_lock(mutex->mutex);
    EXPECT_EQ(mutex->owner, nullptr);
    EXPECT_EQ(mutex->lock_count, 0);
    EXPECT_TRUE(mutex->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_rwlock_waiter_does_not_receive_ownership) {
    KernelState kernel;
    MemState mem;
    const auto owner = make_thread(kernel, mem, 1, ThreadStatus::run);
    const auto stale = make_thread(kernel, mem, 2, ThreadStatus::run);

    const SceUID lock_id = rwlock_create(kernel, mem, "kernel-test", "rwlock", owner->id, 0);
    ASSERT_GE(lock_id, 0);
    ASSERT_EQ(rwlock_lock(kernel, mem, "kernel-test", owner->id, lock_id, nullptr, true), SCE_KERNEL_OK);
    const auto rwlock = kernel.rwlocks.at(lock_id);
    {
        const std::lock_guard<std::mutex> rwlock_lock(rwlock->mutex);
        auto data = make_waiter(stale);
        data.is_write = true;
        rwlock->waiting_threads->push(data);
    }

    EXPECT_EQ(rwlock_unlock(kernel, mem, "kernel-test", owner->id, lock_id, true), SCE_KERNEL_OK);

    const std::lock_guard<std::mutex> rwlock_lock(rwlock->mutex);
    EXPECT_TRUE(rwlock->owners.empty());
    EXPECT_EQ(rwlock->state, RWLockState::Unlocked);
    EXPECT_TRUE(rwlock->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_simple_event_waiter_does_not_consume_pattern) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);

    const SceUID event_id = simple_event_create(kernel, mem, "kernel-test", "event", stale->id, SCE_KERNEL_EVENT_ATTR_AUTO_RESET, 0);
    ASSERT_GE(event_id, 0);
    const auto event = kernel.simple_events.at(event_id);
    {
        const std::lock_guard<std::mutex> event_lock(event->mutex);
        auto data = make_waiter(stale);
        data.pattern = 1;
        event->waiting_threads->push(data);
    }

    EXPECT_EQ(simple_event_setorpulse(kernel, "kernel-test", stale->id, event_id, 1, 0, true), SCE_KERNEL_OK);

    const std::lock_guard<std::mutex> event_lock(event->mutex);
    EXPECT_EQ(event->pattern, 1u);
    EXPECT_TRUE(event->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_eventflag_waiter_does_not_clear_flags) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);

    const SceUID event_id = eventflag_create(kernel, "kernel-test", stale->id, "evf", 0x1000, 0);
    ASSERT_GE(event_id, 0);
    const auto event = kernel.eventflags.at(event_id);
    bool was_canceled = false;
    {
        const std::lock_guard<std::mutex> event_lock(event->mutex);
        auto data = make_waiter(stale);
        data.wait = SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR;
        data.flags = 1;
        data.outBits = nullptr;
        data.was_canceled = &was_canceled;
        event->waiting_threads->push(data);
    }

    EXPECT_EQ(eventflag_set(kernel, "kernel-test", stale->id, event_id, 1), 0);

    const std::lock_guard<std::mutex> event_lock(event->mutex);
    EXPECT_EQ(event->flags, 1);
    EXPECT_TRUE(event->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_eventflag_waiter_is_not_counted_by_cancel) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);

    const SceUID event_id = eventflag_create(kernel, "kernel-test", stale->id, "evf", 0x1000, 0);
    ASSERT_GE(event_id, 0);
    const auto event = kernel.eventflags.at(event_id);
    bool was_canceled = false;
    {
        const std::lock_guard<std::mutex> event_lock(event->mutex);
        auto data = make_waiter(stale);
        data.wait = SCE_EVENT_WAITOR;
        data.flags = 1;
        data.outBits = nullptr;
        data.was_canceled = &was_canceled;
        event->waiting_threads->push(data);
    }

    SceUInt32 waiting_count = 42;
    EXPECT_EQ(eventflag_cancel(kernel, "kernel-test", stale->id, event_id, 0, &waiting_count), SCE_KERNEL_OK);

    EXPECT_EQ(waiting_count, 0u);
    EXPECT_FALSE(was_canceled);
    const std::lock_guard<std::mutex> event_lock(event->mutex);
    EXPECT_TRUE(event->waiting_threads->empty());
}

TEST(ThreadStateTest, stale_semaphore_waiter_is_not_counted_by_cancel) {
    KernelState kernel;
    MemState mem;
    const auto stale = make_thread(kernel, mem, 1, ThreadStatus::run);

    const SceUID semaphore_id = semaphore_create(kernel, "kernel-test", "stale-cancel", stale->id, 0, 0, 1);
    ASSERT_GE(semaphore_id, 0);
    const auto semaphore = kernel.semaphores.at(semaphore_id);
    bool was_canceled = false;
    {
        const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
        auto data = make_waiter(stale);
        data.signal = 1;
        data.was_canceled = &was_canceled;
        semaphore->waiting_threads->push(data);
    }

    SceUInt32 waiting_count = 42;
    EXPECT_EQ(semaphore_cancel(kernel, "kernel-test", stale->id, semaphore_id, 0, &waiting_count), SCE_KERNEL_OK);

    EXPECT_EQ(waiting_count, 0u);
    EXPECT_FALSE(was_canceled);
    const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
    EXPECT_TRUE(semaphore->waiting_threads->empty());
}

TEST(ThreadStateTest, condvar_timeout_reacquires_associated_mutex) {
    KernelState kernel;
    MemState mem;
    const auto thread = make_thread(kernel, mem, 1, ThreadStatus::run);

    SceUID mutex_id = 0;
    ASSERT_EQ(mutex_create(&mutex_id, kernel, mem, "kernel-test", "mutex", thread->id, 0, 1, {}, SyncWeight::Heavy), SCE_KERNEL_OK);
    SceUID condvar_id = 0;
    ASSERT_EQ(condvar_create(&condvar_id, kernel, "kernel-test", "condvar", thread->id, 0, mutex_id, SyncWeight::Heavy), SCE_KERNEL_OK);
    SceUInt32 timeout = 1'000;
    EXPECT_EQ(condvar_wait(kernel, mem, "kernel-test", thread->id, condvar_id, &timeout, SyncWeight::Heavy), SCE_KERNEL_ERROR_WAIT_TIMEOUT);

    const auto mutex = kernel.mutexes.at(mutex_id);
    const std::lock_guard<std::mutex> mutex_lock(mutex->mutex);
    EXPECT_EQ(mutex->owner, thread);
    EXPECT_EQ(mutex->lock_count, 1);
}
