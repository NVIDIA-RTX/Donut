/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace donut::engine
{

class ThreadPoolTask
{
public:
    // Execute the task.
    virtual void Run() = 0;
};

class ThreadPool
{
public:
    ThreadPool(uint32_t numThreads = 0);
    ~ThreadPool();

    // Enqueues a task for execution in the thread pool.
    // If any thread is available, the task immediately starts executing.
    void AddTask(std::shared_ptr<ThreadPoolTask> const& task);

    // Enqueues a function for execution in the thread pool.
    // If any thread is available, the function immediately starts executing.
    void AddTask(std::function<void()> func);

    // Waits for all previously added tasks to complete or fail.
    void WaitForTasks();

private:
    static void StaticThreadProc(ThreadPool* self);
    void ThreadProc();

    std::vector<std::thread> m_threads;
    std::queue<std::shared_ptr<ThreadPoolTask>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_forward;
    std::atomic<bool> m_terminate = false;
    std::atomic<int> m_pendingTasks = 0;
};

}