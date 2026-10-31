module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

module ngine.jobs;

import ngine.core;
import ngine.memory;
import ngine.profile;

namespace ngine::jobs {

namespace detail {

// Arbeitsbereich eines Threads. Eigene Cache-Zeilen, damit Threads sich nicht ausbremsen.
struct alignas(64) Slot {
    WorkStealingDeque<Job*> deque;
    memory::ObjectPool<Job> pool{256, memory::Category::Jobs};
    // Freie Jobs dieses Slots, nur vom eigenen Thread benutzt.
    Job* free_list = nullptr;
    // Jobs, die andere Threads zurückgeben. Lock-freier Stapel; der eigene Thread holt ihn
    // immer komplett ab, deshalb gibt es kein ABA-Problem.
    alignas(64) std::atomic<Job*> returned{nullptr};
};

} // namespace detail

namespace {

// Welcher Thread zu welchem JobSystem gehört.
thread_local const JobSystem* tls_system = nullptr;
thread_local u32 tls_slot = 0;

// Wie oft ein Worker ohne Arbeit weitersucht, bevor er schläft.
constexpr int spin_count = 64;

} // namespace

JobSystem::JobSystem(const JobSystemDesc& desc) {
    const u32 cores = std::max(1u, std::thread::hardware_concurrency());
    worker_count_ = desc.worker_count.value_or(std::max(1u, cores - 1));
    external_slot_ = worker_count_ + 1;

    slots_.reserve(external_slot_ + 1);
    for (u32 i = 0; i <= external_slot_; ++i) {
        slots_.push_back(std::make_unique<detail::Slot>());
    }

    previous_system_ = tls_system;
    previous_slot_ = tls_slot;
    tls_system = this;
    tls_slot = 0;

    workers_.reserve(worker_count_);
    for (u32 i = 1; i <= worker_count_; ++i) {
        workers_.emplace_back([this, i] { worker_main(i); });
    }
    log::debug("Job-System: {} Worker-Threads", worker_count_);
}

JobSystem::~JobSystem() {
    // Der erzeugende Thread hilft beim Abarbeiten; ohne Worker erledigt er alles allein.
    while (detail::Job* job = find_job(current_slot())) {
        execute(job, current_slot());
    }
    stop_.store(true, std::memory_order_seq_cst);
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    epoch_.notify_all();
    workers_.clear();

    // Alle Jobs zurück in ihre Pools, damit diese nichts als belegt melden.
    for (auto& slot : slots_) {
        detail::Job* job = slot->free_list;
        while (job != nullptr) {
            detail::Job* next = job->next_free;
            slot->pool.destroy(job);
            job = next;
        }
        job = slot->returned.exchange(nullptr, std::memory_order_acquire);
        while (job != nullptr) {
            detail::Job* next = job->next_free;
            slot->pool.destroy(job);
            job = next;
        }
    }

    if (tls_system == this) {
        tls_system = previous_system_;
        tls_slot = previous_slot_;
    }
}

u32 JobSystem::current_slot() const noexcept {
    return tls_system == this ? tls_slot : external_slot_;
}

detail::Job* JobSystem::allocate_job() {
    const u32 index = current_slot();
    detail::Slot& slot = *slots_[index];
    // Fremde Threads teilen sich einen Slot und brauchen deshalb eine Sperre.
    std::unique_lock lock{external_mutex_, std::defer_lock};
    if (index == external_slot_) {
        lock.lock();
    }

    if (slot.free_list == nullptr) {
        slot.free_list = slot.returned.exchange(nullptr, std::memory_order_acquire);
    }
    detail::Job* job = slot.free_list;
    if (job != nullptr) {
        slot.free_list = job->next_free;
    } else {
        job = slot.pool.create();
        job->owner = index;
    }
    return job;
}

void JobSystem::free_job(detail::Job* job, u32 slot) noexcept {
    if (job->owner == slot && slot != external_slot_) {
        job->next_free = slots_[slot]->free_list;
        slots_[slot]->free_list = job;
        return;
    }
    std::atomic<detail::Job*>& returned = slots_[job->owner]->returned;
    detail::Job* head = returned.load(std::memory_order_relaxed);
    do {
        job->next_free = head;
    } while (!returned.compare_exchange_weak(head, job, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void JobSystem::submit(detail::Job* job) {
    const u32 slot = current_slot();
    if (has_deque(slot)) {
        slots_[slot]->deque.push(job);
    } else {
        const std::scoped_lock lock{injection_mutex_};
        injection_.push_back(job);
        injection_size_.fetch_add(1, std::memory_order_release);
    }
    // Zusammen mit sleeping_ im Worker (beides seq_cst) geht kein Wecksignal verloren.
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_seq_cst) > 0) {
        epoch_.notify_one();
    }
}

void JobSystem::execute(detail::Job* job, u32 slot) noexcept {
    {
        NGINE_PROFILE_ZONE("Job");
        job->function();
    }
    job->function.reset();
    Counter* counter = job->counter;
    free_job(job, slot);
    // Danach darf der Job den Zähler nicht mehr anfassen: Der Wartende kann ihn sofort zerstören.
    if (counter != nullptr) {
        counter->value_.fetch_sub(1, std::memory_order_release);
    }
}

detail::Job* JobSystem::find_job(u32 slot) {
    if (has_deque(slot)) {
        if (detail::Job* job = slots_[slot]->deque.pop()) {
            return job;
        }
    }
    if (injection_size_.load(std::memory_order_acquire) > 0) {
        const std::scoped_lock lock{injection_mutex_};
        if (!injection_.empty()) {
            detail::Job* job = injection_.front();
            injection_.pop_front();
            injection_size_.fetch_sub(1, std::memory_order_relaxed);
            return job;
        }
    }
    // Reihum bei den anderen stehlen, beginnend beim Nachbarn, damit nicht alle dasselbe Opfer
    // wählen.
    const u32 deques = worker_count_ + 1;
    for (u32 i = 1; i <= deques; ++i) {
        const u32 victim = (slot + i) % deques;
        if (victim == slot) {
            continue;
        }
        if (detail::Job* job = slots_[victim]->deque.steal()) {
            return job;
        }
    }
    return nullptr;
}

void JobSystem::wait(const Counter& counter) {
    const u32 slot = current_slot();
    int idle = 0;
    while (!counter.done()) {
        if (detail::Job* job = find_job(slot)) {
            execute(job, slot);
            idle = 0;
        } else if (++idle > spin_count) {
            // Die fehlenden Jobs laufen woanders; Rechenzeit abgeben statt heiß zu warten.
            std::this_thread::yield();
        }
    }
}

void JobSystem::worker_main(u32 slot) {
    tls_system = this;
    tls_slot = slot;
    profile::set_thread_name(std::format("Worker {}", slot).c_str());

    int idle = 0;
    while (true) {
        if (detail::Job* job = find_job(slot)) {
            execute(job, slot);
            idle = 0;
            continue;
        }
        if (++idle < spin_count) {
            continue;
        }

        // Schlafen. Erst als schlafend melden, dann den Stand merken und nochmals suchen:
        // So kann zwischen Suche und Warten keine neue Arbeit unbemerkt ankommen.
        sleeping_.fetch_add(1, std::memory_order_seq_cst);
        const u32 epoch = epoch_.load(std::memory_order_seq_cst);
        if (detail::Job* job = find_job(slot)) {
            sleeping_.fetch_sub(1, std::memory_order_seq_cst);
            execute(job, slot);
            idle = 0;
            continue;
        }
        if (stop_.load(std::memory_order_seq_cst)) {
            sleeping_.fetch_sub(1, std::memory_order_seq_cst);
            break;
        }
        epoch_.wait(epoch, std::memory_order_seq_cst);
        sleeping_.fetch_sub(1, std::memory_order_seq_cst);
        idle = 0;
    }
}

} // namespace ngine::jobs
