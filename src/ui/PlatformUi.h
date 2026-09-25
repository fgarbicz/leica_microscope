#pragma once
// The few places where the application has to do something different per
// operating system: revealing a file in the file manager, and preparing access
// to the camera (a driver on Windows, a udev rule on Linux, nothing on macOS).
// Everything else in the UI is identical on all three platforms.

#include <QString>

class QWidget;

namespace lm {

// Opens the system file manager with `path` selected. Falls back to opening the
// containing folder where selecting is not supported.
void revealInFileManager(const QString &path);

// Whether this platform needs a one-off step before the Leica camera can be
// opened. False on macOS: a vendor-class device needs no driver there.
bool cameraAccessSetupAvailable();

// Menu/button text for that step, e.g. "Install / repair camera driver…".
QString cameraAccessSetupLabel();

// Runs it (elevated where needed) and returns a short status message for the
// status bar, or an empty string when the user cancelled.
QString setUpCameraAccess(QWidget *parent);

// Menu wording that must match what the user's desktop calls things:
// "Show in Explorer" / "Show in Finder" / "Show in file manager", and the
// recycle bin / Trash.
QString revealActionText();
QString trashName();

// One line naming the platform and the USB backend, for the About box.
QString platformDescription();

} // namespace lm
