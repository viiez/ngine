// Job-System der Engine: verteilt kleine Aufgaben über Work-Stealing auf alle Kerne.
// Abhängigkeiten gibt es über Zähler (Counter) und Graphen (TaskGraph).
export module ngine.jobs;

export import :deque;
export import :function;
export import :system;
export import :graph;
