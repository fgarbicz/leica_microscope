#pragma once
// The few places where the application has to do something different per
// operating system: revealing a file in the file manager, and preparing access
// to the camera (a driver on Windows, a udev rule on Linux, nothing on macOS).
// Everything else in the UI is identical on all three platforms.

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace lm {

// Opens the system file manager with `path` selected. Falls back to opening the
// containing folder where selecting is not supported.
void revealInFileManager(const QString &path);

// QFileDialog::getSaveFileName() that always returns a name with an extension.
// Windows and macOS add the selected filter's extension to a name typed
// without one; the Qt and GTK dialogs on Linux do not, and a file without an
// extension is then written in the default format and opened by nothing. A
// name that already ends in an extension of one of the filters is kept as it
// is; otherwise the selected filter's first extension is appended (and a file
// of that name is only replaced after asking). Empty when cancelled.
QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir, const QString &filter,
                        QString *selectedFilter = nullptr);

// Opens an image in the desktop's default image viewer (Photos on Windows,
// Preview on macOS, the desktop's viewer on Linux) in a window of its own, e.g.
// as a reference on a second screen. Explains the problem when that fails.
bool openInImageViewer(const QString &path, QWidget *parent);

// Whether this platform needs a one-off step before the Leica camera can be
// opened. False on macOS: a vendor-class device needs no driver there.
bool cameraAccessSetupAvailable();

// Menu/button text for that step, e.g. "Install / repair camera driver…".
QString cameraAccessSetupLabel();

// Runs it (elevated where needed). The installer runs in the background behind
// a busy dialog, so this returns before it has finished; `done` is called at the
// end with a short status message for the status bar, or an empty string when
// the user cancelled.
void setUpCameraAccess(QWidget *parent, std::function<void(const QString &)> done = {});

// Menu wording that must match what the user's desktop calls things:
// "Show in Explorer" / "Show in Finder" / "Show in file manager", and the
// recycle bin / Trash.
QString revealActionText();
QString trashName();

// Moves an image to the Trash / recycle bin together with its sidecars (.json
// metadata, .annotations.json). The sidecars only go when the image did. False
// when the image itself could not be moved.
bool moveImageToTrash(const QString &path);

// The files that belong to an image: its .json metadata and .annotations.json.
QStringList sidecarsOf(const QString &imagePath);

// Renames an image and its sidecars as one step: either all of them are
// renamed, or none is (a failure half-way is rolled back), so an image never
// ends up separated from its metadata or annotations. A change of case only is
// allowed on case-insensitive file systems. The error is for the user.
bool renameImage(const QString &from, const QString &to, QString *error);

// Why `name` cannot be a file name on any of the three platforms, or empty.
QString invalidFileName(const QString &name);

// One line naming the platform and the USB backend, for the About box.
QString platformDescription();

} // namespace lm
