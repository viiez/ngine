// CPU-Speicher der Engine: Allokatoren auf Basis von std::pmr::memory_resource und
// Budget-Tracking pro Kategorie. GPU-Speicher verwaltet die RHI (VMA).
export module ngine.memory;

export import :align;
export import :budget;
export import :linear;
export import :frame;
export import :pool;
