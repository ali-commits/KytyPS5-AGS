#ifndef AGS_ADAPTER_LAUNCH_PLAN_H
#define AGS_ADAPTER_LAUNCH_PLAN_H

// launch.plan (STANDARD.md section 5.4): kyty_emulator next to the adapter, BuildEmulatorArgs with
// the standard and player settings applied, cwd and writable = runtime_root, u59 preset env.
#include "reply.h"

#include <QJsonObject>
#include <QString>

struct GameEntry;

namespace AdapterLaunch {

// BCP-47 tag to console language index (preferenceSchema.h order); -1 when unknown.
[[nodiscard]] int     LanguageIndex(const QString& tag);
[[nodiscard]] QString LanguageTag(int index);
// Nearest frame rate the emulator can export (Streaming::IsCaptureFrameRateSupported).
[[nodiscard]] int NearestCaptureFps(int fps);
// `text` cut to at most `max_bytes` UTF-8 bytes on a character boundary.
[[nodiscard]] QString CutUtf8(const QString& text, int max_bytes);

// Env names AGS owns for the game's session (STANDARD.md section 5.4): the `AGS_` prefix and the
// display, audio and runtime-dir names. A plan that sets one is rejected, so none is emitted.
[[nodiscard]] bool IsReservedEnvName(const QString& name);

[[nodiscard]] Reply Plan(const GameEntry& game, const QJsonObject& params,
                         const QString& runtime_root, const QString& adapter_dir);

} // namespace AdapterLaunch

#endif
