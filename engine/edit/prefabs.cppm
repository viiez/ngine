module;

#include <expected>
#include <memory>
#include <string>

export module ngine.edit:prefabs;

import ngine.core;
import ngine.math;
import ngine.world;
import :document;

// Prefabs im Editor (WORLD-04, R2.5): einen Teilbaum zur Instanz einer Vorlage machen, eine
// Instanz aufbrechen und neue Instanzen zum Einfügen vorbereiten. Die Datei der Vorlage schreibt
// der Editor selbst (world::make_prefab(), world::write_prefab()); Befehle ändern nur die Welt.
export namespace ngine::edit {

// Macht den Teilbaum unter root zur Instanz der Vorlage prefab_id, die aus genau diesem Teilbaum
// entstanden ist (world::make_prefab()): Knoten in derselben Reihenfolge, ab 1 gezählt. Die Werte
// bleiben, Undo löst die Verknüpfung wieder.
[[nodiscard]] Result<std::unique_ptr<Command>> link_prefab(SceneDocument& document, EntityId root,
                                                           u64 prefab_id);

// Bricht die Instanz unter root auf (world::unpack_instance()); Undo stellt sie wieder her.
[[nodiscard]] Result<std::unique_ptr<Command>> unpack_prefab(SceneDocument& document,
                                                             EntityId root);

// Snapshot einer neuen Instanz von prefab mit freien Kennungen des Dokuments, die Wurzel an
// position unter parent (0: Wurzel); für insert_entities().
[[nodiscard]] Result<EntitySnapshot> prefab_snapshot(SceneDocument& document,
                                                     const world::Prefab& prefab, u64 prefab_id,
                                                     math::DVec3 position, EntityId parent = 0);

} // namespace ngine::edit
