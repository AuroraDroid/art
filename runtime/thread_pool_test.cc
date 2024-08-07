/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "thread_pool.h"

#include <string>

#include "base/atomic.h"
#include "common_runtime_test.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"

namespace art HIDDEN {

class CountTask : public Task {
 public:
  explicit CountTask(AtomicInteger* count) : count_(count), verbose_(false) {}

  void Run(Thread* self) override {
    if (verbose_) {
      LOG(INFO) << "Running: " << *self;
    }
    // Simulate doing some work.
    usleep(100);
    // Increment the counter which keeps track of work completed.
    ++*count_;
  }

  void Finalize() override {
    if (verbose_) {
      LOG(INFO) << "Finalizing: " << *Thread::Current();
    }
    delete this;
  }

 private:
  AtomicInteger* const count_;
  const bool verbose_;
};

class ThreadPoolTest : public CommonRuntimeTest {
 public:
  static int32_t num_threads;
};

int32_t ThreadPoolTest::num_threads = 4;

// Check that the thread pool actually runs tasks that you assign it.
TEST_F(ThreadPoolTest, CheckRun) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Thread pool test thread pool", num_threads));
  AtomicInteger count(0);
  static const int32_t num_tasks = num_threads * 4;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool->AddTask(self, new CountTask(&count));
  }
  thread_pool->StartWorkers(self);
  // Wait for tasks to complete.
  thread_pool->Wait(self, true, false);
  // Make sure that we finished all the work.
  EXPECT_EQ(num_tasks, count.load(std::memory_order_seq_cst));
}

TEST_F(ThreadPoolTest, StopStart) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Thread pool test thread pool", num_threads));
  AtomicInteger count(0);
  static const int32_t num_tasks = num_threads * 4;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool->AddTask(self, new CountTask(&count));
  }
  usleep(200);
  // Check that no threads started prematurely.
  EXPECT_EQ(0, count.load(std::memory_order_seq_cst));
  // Signal the threads to start processing tasks.
  thread_pool->StartWorkers(self);
  usleep(200);
  thread_pool->StopWorkers(self);
  AtomicInteger bad_count(0);
  thread_pool->AddTask(self, new CountTask(&bad_count));
  usleep(200);
  // Ensure that the task added after the workers were stopped doesn't get run.
  EXPECT_EQ(0, bad_count.load(std::memory_order_seq_cst));
  // Allow tasks to finish up and delete themselves.
  thread_pool->StartWorkers(self);
  thread_pool->Wait(self, false, false);
}

TEST_F(ThreadPoolTest, StopWait) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Thread pool test thread pool", num_threads));

  AtomicInteger count(0);
  static const int32_t num_tasks = num_threads * 100;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool->AddTask(self, new CountTask(&count));
  }

  // Signal the threads to start processing tasks.
  thread_pool->StartWorkers(self);
  usleep(200);
  thread_pool->StopWorkers(self);

  thread_pool->Wait(self, false, false);  // We should not deadlock here.

  // Drain the task list. Note: we have to restart here, as no tasks will be finished when
  // the pool is stopped.
  thread_pool->StartWorkers(self);
  thread_pool->Wait(self, /* do_work= */ true, false);
}

class TreeTask : public Task {
 public:
  TreeTask(ThreadPool* const thread_pool, AtomicInteger* count, int depth)
      : thread_pool_(thread_pool),
        count_(count),
        depth_(depth) {}

  void Run(Thread* self) override {
    if (depth_ > 1) {
      thread_pool_->AddTask(self, new TreeTask(thread_pool_, count_, depth_ - 1));
      thread_pool_->AddTask(self, new TreeTask(thread_pool_, count_, depth_ - 1));
    }
    // Increment the counter which keeps track of work completed.
    ++*count_;
  }

  void Finalize() override {
    delete this;
  }

 private:
  ThreadPool* const thread_pool_;
  AtomicInteger* const count_;
  const int depth_;
};

// Test that adding new tasks from within a task works.
TEST_F(ThreadPoolTest, RecursiveTest) {
  Thread* self = Thread::Current();
  std::unique_ptr<ThreadPool> thread_pool(
      ThreadPool::Create("Thread pool test thread pool", num_threads));
  AtomicInteger count(0);
  static const int depth = 8;
  thread_pool->AddTask(self, new TreeTask(thread_pool.get(), &count, depth));
  thread_pool->StartWorkers(self);
  thread_pool->Wait(self, true, false);
  EXPECT_EQ((1 << depth) - 1, count.load(std::memory_order_seq_cst));
}

class PeerTask : public Task {
 public:
  PeerTask() {}

  void Run(Thread* self) override {
    ScopedObjectAccess soa(self);
    CHECK(self->GetPeer() != nullptr);
  }

  void Finalize() override {
    delete this;
  }
};

class NoPeerTask : public Task {
 public:
  NoPeerTask() {}

  void Run(Thread* self) override {
    ScopedObjectAccess soa(self);
    CHECK(self->GetPeer() == nullptr);
  }

  void Finalize() override {
    delete this;
  }
};

// Tests for create_peer functionality.
TEST_F(ThreadPoolTest, PeerTest) {
  Thread* self = Thread::Current();
  {
    std::unique_ptr<ThreadPool> thread_pool(
        ThreadPool::Create("Thread pool test thread pool", 1));
    thread_pool->AddTask(self, new NoPeerTask());
    thread_pool->StartWorkers(self);
    thread_pool->Wait(self, false, false);
  }

  {
    // To create peers, the runtime needs to be started.
    self->TransitionFromSuspendedToRunnable();
    bool started = runtime_->Start();
    ASSERT_TRUE(started);

    std::unique_ptr<ThreadPool> thread_pool(
        ThreadPool::Create("Thread pool test thread pool", 1, true));
    thread_pool->AddTask(self, new PeerTask());
    thread_pool->StartWorkers(self);
    thread_pool->Wait(self, false, false);
  }
}

}  // namespace art
