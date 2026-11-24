// Replikation der Welt vom Server zu den Clients (NET-04, ADR 0024, ADR 0052). Der Server schickt
// jedem Client regelmäßig ein Delta gegen dessen letzten bestätigten Stand: alle Entities mit
// Komponenten, die die Reflection als repliziert markiert (:schema), aus der Umgebung seiner Figur
// und mit quantisierten Transforms. Der Client spiegelt das in seine eigene Welt und zeichnet
// Bewegungen interpoliert (:snapshot). Seit R5.1 dazu die Eingabe der Spieler und welche Figur ein
// Client steuert (:input), seit R5.3 Befehle pro festem Schritt und die Vorhersage der eigenen
// Figur
// (:prediction).
export module ngine.replication;

export import :schema;
export import :snapshot;
export import :session;
export import :input;
export import :prediction;
