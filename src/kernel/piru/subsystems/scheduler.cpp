#include "scheduler.hpp"

#include <bits/move.h>

#include "kernel.hpp"

namespace piru::subsystems {
    scheduler::scheduler(heap_allocator &allocator) : _allocator(allocator) {

    }

    uint64_t scheduler::Add(callback cb, void *data, uint64_t delayInMs, bool periodic) {
        auto id = GetNewId();

        _tasks.Add(task{
            .id = id,
            .absolute_time = GetCurrentTime() + delayInMs,
            .data = data,
            .cb = cb,
            .periodic = (periodic ? delayInMs : 0)
        });

        BubbleUp(_tasks.Count() - 1);

        return id;
    }

    void scheduler::Remove(uint64_t id) {
        threading::spinlock::scoped_lock lock(_lock, true);
        auto index = _tasks.IndexOf([](const task &t, void* data) {
            return t.id == *(uint64_t*)data;
        }, &id);

        if (index == _tasks.npos) {
            return;
        }

        std::swap(_tasks[index], _tasks[_tasks.Count() - 1]);
        _tasks.Remove(_tasks.Count() - 1);

        if (index < _tasks.Count()) {
            BubbleDown(index);
            BubbleUp(index);
        }
    }

    Result scheduler::Initialize() {
        _tasks.Resize(64);

        return Result::SUCCESS;
    }

    bool scheduler::RunTests() {
        return true; // no tests for this subsystem.
    }

    void scheduler::Run() {
        list<task> toExecute;

        {
            threading::spinlock::scoped_lock lock(_lock, true);
            uint64_t currentTime = GetCurrentTime();

            while (_tasks.Count() > 0 && _tasks[0].absolute_time <= currentTime) {
                task t = _tasks[0];
                std::swap(_tasks[0], _tasks[_tasks.Count() - 1]);
                _tasks.Remove(_tasks.Count() - 1);
                if (_tasks.Count() > 0) {
                    BubbleDown(0);
                }

                toExecute.Add(t);

                currentTime = GetCurrentTime(); // Update just because this mightve taken a while.s
            }

            if (_tasks.Count() > 0) {
                uint64_t nextTriggerTime = _tasks[0].absolute_time;
                uint64_t now = GetCurrentTime();

                if (nextTriggerTime > now) {
                    uint64_t sleepDuration = nextTriggerTime - now;
                    lock.~scoped_lock(); // Unlock before sleeping

                    HardwareSleep(sleepDuration); // Sleep for the duration until the next task
                }
            }
        }

        for (auto& t : toExecute) {
            if (t.cb)
                t.cb(t.absolute_time, t.data);
            if (t.periodic > 0) {
                t.absolute_time = GetCurrentTime() + t.periodic;
                _tasks.Add(t);
                BubbleUp(_tasks.Count() - 1);
            }
        }
    }

    void scheduler::BubbleUp(size_t index) {
        while (index > 0) {
            size_t parent = (index - 1) / 2;

            if (_tasks[index].absolute_time < _tasks[parent].absolute_time) {
                std::swap(_tasks[index], _tasks[parent]);
                index = parent;
            } else {
                break;
            }
        }
    }

    void scheduler::BubbleDown(size_t index) {
        size_t size = _tasks.Count();
        while (true) {
            size_t left = (2 * index) + 1;
            size_t right = (2 * index) + 2;
            size_t smallest = index;

            // Find the child with the smallest absolute_time
            if (left < size && _tasks[left].absolute_time < _tasks[smallest].absolute_time)
                smallest = left;

            if (right < size && _tasks[right].absolute_time < _tasks[smallest].absolute_time)
                smallest = right;

            // If a child is smaller than the current node, swap and continue down
            if (smallest != index) {
                std::swap(_tasks[index], _tasks[smallest]);
                index = smallest;
            } else {
                break; // Heap property is satisfied
            }
        }
    }

    uint64_t scheduler::GetCurrentTime() {
        auto &kernel = kernel::GetInstance();
        return kernel.data.time.GetMs();
    }

    void scheduler::HardwareSleep(uint64_t durationInMs) {
        auto &kernel = kernel::GetInstance();
        kernel.GetCpu().apic.Sleep(durationInMs);
    }
}
