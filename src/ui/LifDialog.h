#pragma once
// Choosing one image out of a Leica .lif, which usually holds a whole session.

#include "io/LifFile.h"

#include <QDialog>
#include <QList>

class QListWidget;
class QLabel;

namespace lm {

class LifDialog : public QDialog {
    Q_OBJECT
public:
    LifDialog(const QString &path, const QList<LifEntry> &entries, QWidget *parent = nullptr);

    // The chosen image, valid once exec() returned Accepted.
    const LifEntry &selected() const { return m_entries[m_index]; }
    int selectedIndex() const { return m_index; }

    // Shows the dialog for `path` and loads the image the user picked. Returns
    // false when the file cannot be read or the user cancelled; `error` is set
    // only for a real failure, so an empty error means "cancelled".
    static bool openFrom(const QString &path, LoadedImage &out, QWidget *parent, QString *error);

private:
    void updateDetails();

    QString m_path;
    QList<LifEntry> m_entries;
    QListWidget *m_list;
    QLabel *m_details;
    int m_index = 0;
};

} // namespace lm
