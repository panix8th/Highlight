#pragma once

namespace hl {

enum class Cue { Saved, Bookmark, Error, RecordStart, RecordStop };

/* Plays a short synthesized cue (no asset files). volume: 0..100 */
void play_cue(Cue cue, int volume);

} // namespace hl
