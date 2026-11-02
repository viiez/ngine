// Replikation der Welt vom Server zu den Clients (NET-04, ADR 0024). Der Server schickt
// regelmäßig einen Schnappschuss ausgewählter Komponenten aller Entities mit Replicated; der
// Client spiegelt ihn in seine eigene Welt.
export module ngine.replication;

export import :snapshot;
export import :session;
