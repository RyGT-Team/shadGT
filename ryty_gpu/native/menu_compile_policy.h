#pragma once
#include <string_view>

namespace Native {
// FIX-SGT-049: both race photography and Scapes enter the photo project and load its gallery.
constexpr bool PhotoEditorAfterProject(std::string_view project, bool was_in_editor) {
    return project == "photo" || (project == "gallery" && was_in_editor);
}

// project_hooks: GT Sport's script-read hook went in, so in_race reflects the game's project.
// FIX-GT7-140: without it (GT7 and other titles) in_race never turns true and the menu wait
// would hold every draw for its new pipeline; shadgt7's rule applies instead: keep going (mode 2)
// never waits, the other modes wait outside races.
// FIX-SGT-049: photo_capture, GT Sport rendering a photo (from its takePhoto command until the
// picture is encoded or saved): every draw and dispatch waits, whatever the mode, as during an icon
// capture. A skipped draw there is saved in the photo for good: with cold shaders the first photo
// came out black (upstream) or with its channels mixed up.
constexpr bool MenuCompileMustWait(unsigned mode, bool in_race, bool capture_hooks,
                                   bool capture_idle, bool project_hooks = true,
                                   bool photo_capture = false) {
    if (photo_capture) return true;
    if (capture_hooks && !capture_idle) return true;
    if (!project_hooks) return mode != 2 && !in_race;
    return !in_race && (mode != 2 || !capture_hooks);
}
} // namespace Native
