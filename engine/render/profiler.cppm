module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module ngine.render:profiler;

import ngine.core;
import ngine.rhi;

export namespace ngine::render {

// Zeit eines Passes auf der GPU in einem früheren Frame (R4.6, TD-45).
struct PassTime {
    std::string name;
    f64 milliseconds = 0.0;
    // Verschachtelungstiefe der Zone (0 für Pässe des Render-Graphs).
    u32 depth = 0;
};

// GPU-Zonen (CORE-07): Zeitstempel vor und nach jeder Zone, abgeholt, wenn derselbe Platz unter den
// Frames im Flug wieder an der Reihe ist (Frame::timestamps). Der Render-Graph setzt eine Zone um
// jeden Pass. Mit Tracy gehen die Zonen über ngine.profile an Tracy; kann die GPU kalibrierte
// Zeitstempel, gleicht der Profiler die Zeitachsen alle paar Sekunden neu ab (TD-22), sonst nur
// beim Start. Seit R4.6 liefert er die Zeiten auch selbst (pass_times()), z. B. für das
// Statistikfenster des Editors. Ohne Zeitstempel auf der GPU tut der Profiler nichts.
class GpuProfiler {
public:
    // Gleicht die Zeitachsen einmal ab (wartet dafür kurz auf die GPU).
    explicit GpuProfiler(rhi::Device& device);

    // Ob die GPU Zeitstempel schreibt (und der Profiler Zeiten misst).
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    // Ob die Zonen auch an Tracy gehen.
    [[nodiscard]] bool tracy() const noexcept {
        return tracy_;
    }
    // Zeiten der Zonen des letzten fertigen Frames in der Reihenfolge ihres Beginns; leer ohne
    // Zeitstempel oder in den ersten Frames.
    [[nodiscard]] const std::vector<PassTime>& pass_times() const noexcept {
        return times_;
    }
    // Summe der Zonen der obersten Ebene in Millisekunden.
    [[nodiscard]] f64 total_ms() const noexcept;

    // Zu Beginn jedes Frames, vor dem Aufzeichnen: meldet die Zeiten des letzten Frames mit
    // demselben Platz.
    void begin_frame(const rhi::Frame& frame);
    // Zonen dürfen nicht innerhalb von begin_rendering() beginnen oder enden.
    void begin_zone(rhi::CommandList& commands, std::string_view name);
    void end_zone(rhi::CommandList& commands);

private:
    [[nodiscard]] u16 query(u32 index) const noexcept;
    void recalibrate();

    struct Zone {
        std::string name;
        u32 begin = 0;
        u32 end = 0;
        u32 depth = 0;
    };

    bool active_ = false;
    bool tracy_ = false;
    f64 period_ns_ = 0.0;
    u8 context_ = 0;
    rhi::Device* device_ = nullptr;
    // Kalibrierte Zeitstempel: Zeit der CPU (ns) beim letzten Abgleich, sonst nichts.
    std::optional<u64> calibrated_cpu_ns_;
    u32 slot_ = 0;
    u32 next_ = 0;
    // Pro offener Zone: Index in recorded_[slot_] oder ohne Zeitstempel.
    std::vector<std::optional<usize>> open_;
    // Zonen je Platz der Frames im Flug, bis ihre Zeitstempel zurück sind.
    std::vector<std::vector<Zone>> recorded_;
    std::vector<PassTime> times_;
    bool in_frame_ = false;
};

// Zeiten der obersten Ebene, gleichnamige Pässe (Kaskaden, Stufen des Hi-Z) zusammengefasst, in
// der Reihenfolge ihres ersten Auftretens; count zählt die Pässe.
struct PassGroup {
    std::string name;
    f64 milliseconds = 0.0;
    u32 count = 0;
};
[[nodiscard]] std::vector<PassGroup> grouped_pass_times(const GpuProfiler& profiler);

// Zeiten je Pass als eine Zeile für das Log, z. B. „GPU 2.10 ms: Skinning 0.02, Schatten 0.80,
// …“; ohne Zeiten ein Hinweis.
[[nodiscard]] std::string pass_report(const GpuProfiler& profiler);

} // namespace ngine::render
