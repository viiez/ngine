// Replikation der Welt vom Server zu den Clients (NET-04, ADR 0024). Der Server schickt
// regelmäßig einen Schnappschuss ausgewählter Komponenten aller Entities mit Replicated; der
// Client spiegelt ihn in seine eigene Welt. Seit R5.1 dazu die Eingabe der Spieler und welche
// Figur ein Client steuert (:input).
export module ngine.replication;

export import :snapshot;
export import :session;
export import :input;
