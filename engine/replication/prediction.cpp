module;

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.input;
import ngine.math;

namespace ngine::replication {

f64 position_error(const PredictedState& a, const PredictedState& b) noexcept {
    return math::length(a.position - b.position);
}

void Predictor::record(Command command, const PredictedState& after) {
    next_ = std::max(next_, command.number + 1);
    entries_.push_back({std::move(command), after});
    while (entries_.size() > std::max<usize>(settings_.history, 1)) {
        entries_.pop_front();
    }
}

bool Predictor::reconcile(u32 number, const PredictedState& server, const SetState& set,
                          const Step& step) {
    if (number <= stats_.acknowledged) {
        // Doppelt oder überholt.
        return false;
    }
    const auto found = std::ranges::find_if(
        entries_, [number](const Entry& entry) { return entry.command.number == number; });
    if (found == entries_.end()) {
        ++stats_.unknown;
        // Älter als alles Gemerkte zählt als bestätigt; einen künftigen Befehl gibt es nicht.
        if (entries_.empty() || number < entries_.front().command.number) {
            stats_.acknowledged = number;
        }
        return false;
    }
    stats_.acknowledged = number;
    const PredictedState& predicted = found->state;
    const f64 error = position_error(predicted, server);
    const f32 velocity_error = math::length(predicted.velocity - server.velocity);
    stats_.last_error = error;
    const bool differs = error > settings_.position_tolerance ||
                         velocity_error > settings_.velocity_tolerance ||
                         predicted.crouched != server.crouched;
    // Bestätigte Befehle braucht niemand mehr.
    const auto after = entries_.erase(entries_.begin(), std::next(found));
    if (!differs) {
        return false;
    }
    set(server);
    for (auto it = after; it != entries_.end(); ++it) {
        it->state = step(it->command);
        ++stats_.replayed;
    }
    ++stats_.corrections;
    stats_.last_correction = error;
    return true;
}

std::vector<Command> Predictor::recent(usize count) const {
    const usize n = std::min(count, entries_.size());
    std::vector<Command> commands;
    commands.reserve(n);
    for (usize i = entries_.size() - n; i < entries_.size(); ++i) {
        commands.push_back(entries_[i].command);
    }
    return commands;
}

std::optional<PredictedState> Predictor::latest() const {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.back().state;
}

std::optional<PredictedState> Predictor::predicted(u32 number) const {
    const auto found = std::ranges::find_if(
        entries_, [number](const Entry& entry) { return entry.command.number == number; });
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->state;
}

PredictionStats Predictor::stats() const noexcept {
    PredictionStats stats = stats_;
    stats.pending = static_cast<u32>(entries_.size());
    return stats;
}

void Predictor::reset() {
    entries_.clear();
    stats_.acknowledged = 0;
}

} // namespace ngine::replication
