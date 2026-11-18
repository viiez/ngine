// Skelett-Animation (R4.6, ANIM-01, ANIM-02, ADR 0050): Skelette und Clips aus glTF (Rig),
// Abtasten und Mischen von Posen, Zustandsautomat als Asset (.animgraph), Komponente Animator und
// AnimationSystem, das Posen in Jobs rechnet. Ohne Fenster und GPU; der Renderer verformt die Netze
// mit den Paletten, die das System liefert.
export module ngine.animation;

export import :rig;
export import :graph;
export import :machine;
export import :components;
export import :system;
