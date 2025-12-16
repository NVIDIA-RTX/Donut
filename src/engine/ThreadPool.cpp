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

#include <donut/engine/ThreadPool.h>
#include <cassert>

namespace donut::engine
{

// A simple task wrapper for a function object
class ThreadPoolFunctionTask : public ThreadPoolTask
{
public:
    ThreadPoolFunctionTask(std::function<void()>&& func)
        : m_func(std::move(func))
    { }

    void Run() override
    {
        return m_func();
    }

private:
    std::function<void()> m_func;
};

ThreadPool::ThreadPool(uint32_t numThreads)
{
    if (numThreads == 0)
        numThreads = std::thread::hardware_concurrency();

    m_threads.resize(numThreads);
    for (uint32_t i = 0; i < numThreads; ++i)
    {
        m_threads[i] = std::thread(StaticThreadProc, this);
    }
}

ThreadPool::~ThreadPool()
{
    WaitForTasks();

    m_terminate.store(true);

    m_forward.notify_all();

    for (std::thread& thread : m_threads)
        thread.join();
}

void ThreadPool::AddTask(std::shared_ptr<ThreadPoolTask> const& task)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    
    m_tasks.push(task);
    ++m_pendingTasks;

    m_forward.notify_one();
}

void ThreadPool::AddTask(std::function<void()> func)
{
    std::shared_ptr<ThreadPoolFunctionTask> task = std::make_shared<ThreadPoolFunctionTask>(std::move(func));
    AddTask(task);
}

void ThreadPool::WaitForTasks()
{
    while(m_pendingTasks.load() != 0)
        std::this_thread::yield();
}

void ThreadPool::StaticThreadProc(ThreadPool* self)
{
    self->ThreadProc();
}

void ThreadPool::ThreadProc()
{
    while(!m_terminate.load())
    {
        std::shared_ptr<ThreadPoolTask> task;
        
        // Wait until a task is available or termination is requested
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_forward.wait(lock, [this] { return !m_tasks.empty() || m_terminate.load(); });

            if (!m_tasks.empty())
            {
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
        }

        if (task)
        {
            try
            {
                task->Run();
            }
            catch(...)
            {
                // Ignore task exceptions
            }
            --m_pendingTasks;
        }
    }
}

}