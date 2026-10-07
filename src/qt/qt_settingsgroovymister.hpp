#ifndef QT_SETTINGSGROOVYMISTER_HPP
#define QT_SETTINGSGROOVYMISTER_HPP

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

/* "MiSTer" settings page: where the emulated machine's video and audio go when they are
   not going to a host window.
 *
 * Built in code rather than from a .ui file. The page is a short list of plain controls
 * with no device tables to populate, and the two that matter - the monitor preset and what
 * to do about interlacing - need their explanation next to them, which is easier to keep
 * truthful here than in generated XML.
 */
class SettingsGroovyMiSTer : public QWidget {
    Q_OBJECT

public:
    explicit SettingsGroovyMiSTer(QWidget *parent = nullptr);
    ~SettingsGroovyMiSTer() override = default;

    void save(int soft);

private:
    void refreshEnabledState();

    QCheckBox *chkEnabled   = nullptr;
    QLineEdit *editHost     = nullptr;
    QComboBox *cboMonitor   = nullptr;
    QComboBox *cboInterlace = nullptr;
    QComboBox *cboCodec     = nullptr;
    QSpinBox  *spinNear     = nullptr;
    QCheckBox *chkAudio     = nullptr;
    QSpinBox  *spinMtu      = nullptr;
};

#endif /*QT_SETTINGSGROOVYMISTER_HPP*/
