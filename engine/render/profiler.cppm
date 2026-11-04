module;

#include <string_view>
#include <vector>

export module ngine.render:profiler;

import ngine.core;
import ngine.rhi;

export namespace ngine::render {

// GPU-Zonen in Tracy (CORE-07): Zeitstempel vor und nach jeder Zone, abgeholt, wenn derselbe Platz
// unter den Frames im Flug wieder an der Reihe ist (Frame::timestamps), und über ngine.profile
// gemeldet. Der Render-Graph setzt eine Zone um jeden Pass. Ohne Tracy oder ohne Zeitstempel auf
// der GPU tut der Profiler nichts.
class GpuProfiler {
public:
    // Gleicht die Zeitachsen einmal ab (wartet dafür kurz auf die GPU).
    explicit GpuProfiler(rhi::Device& device);

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    // Zu Beginn jedes Frames, vor dem Aufzeichnen: meldet die Zeiten des letzten Frames mit
    // demselben Platz.
    void begin_frame(const rhi::Frame& frame);
    // Zonen dürfen nicht innerhalb von begin_rendering() beginnen oder enden.
    void begin_zone(rhi::CommandList& commands, std::string_view name);
    void end_zone(rhi::CommandList& commands);

private:
    [[nodiscard]] u16 query(u32 index) const noexcept;

    bool active_ = false;
    u8 context_ = 0;
    u32 slot_ = 0;
    u32 next_ = 0;
    // Pro offener Zone: ob sie Zeitstempel bekommen hat.
    std::vector<bool> open_;
    bool in_frame_ = false;
};

} // namespace ngine::render
