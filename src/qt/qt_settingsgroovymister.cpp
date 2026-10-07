#include "qt_settingsgroovymister.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

extern "C" {
#include <86box/86box.h>
#include <86box/groovy_mister.h>
}

#include <cstring>

namespace {

/* switchres presets worth offering for a PC.
 *
 * The full list switchres compiles in is much longer, but most of it is arcade monitor
 * models that have no bearing on VGA timings. These are the bands a PC's modes actually
 * land in. The data string is what goes in the config file.
 */
struct MonitorPreset {
    const char *key;
    const char *label;
};

const MonitorPreset monitor_presets[] = {
    { "arcade_15",       QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "15 kHz arcade / consumer CRT")        },
    { "arcade_15_25",    QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "15/25 kHz arcade monitor")            },
    { "arcade_15_25_31", QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "15/25/31 kHz tri-sync arcade monitor")},
    { "arcade_31",       QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "31 kHz arcade monitor")               },
    { "generic_15",      QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "15 kHz generic")                      },
    { "ntsc",            QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "NTSC television")                     },
    { "pal",             QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "PAL television")                      },
    { "vesa_480",        QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "VGA PC monitor (31 kHz, 640x480)")    },
    { "vesa_600",        QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "SVGA PC monitor (800x600)")           },
    { "vesa_768",        QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "XGA PC monitor (1024x768)")           },
    { "pc_31_120",       QT_TRANSLATE_NOOP("SettingsGroovyMiSTer", "Multisync PC monitor (31-120 kHz)")   },
};

} // namespace

SettingsGroovyMiSTer::SettingsGroovyMiSTer(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    auto *form  = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    chkEnabled = new QCheckBox(tr("Send video and audio to a MiSTer"), this);
    chkEnabled->setChecked(groovy_mister_enabled != 0);
    outer->addWidget(chkEnabled);

    auto *blurb = new QLabel(tr("The emulated machine's display is streamed over the network to a "
                                "MiSTer running the GroovyNLC core, which drives a real CRT from "
                                "the video mode the machine programmed."),
                             this);
    blurb->setWordWrap(true);
    outer->addWidget(blurb);
    outer->addSpacing(8);

    editHost = new QLineEdit(QString::fromUtf8(groovy_mister_host), this);
    editHost->setPlaceholderText(tr("e.g. 192.168.1.10"));
    form->addRow(tr("MiSTer &address:"), editHost);

    cboMonitor = new QComboBox(this);
    for (const auto &preset : monitor_presets)
        cboMonitor->addItem(tr(preset.label), QString::fromUtf8(preset.key));
    {
        const int at = cboMonitor->findData(QString::fromUtf8(groovy_mister_monitor));
        cboMonitor->setCurrentIndex(at >= 0 ? at : 0);
    }
    form->addRow(tr("&Monitor:"), cboMonitor);

    cboMonitor->setToolTip(tr("What the display can scan. This decides how a PC video mode "
                              "reaches it: a 640x480 70 Hz VGA mode is 31 kHz, which a 15 kHz "
                              "CRT cannot show at all and a PC monitor shows natively."));
    auto *monitorNote = new QLabel(tr("What the display can scan."), this);
    form->addRow(monitorNote);

    cboInterlace = new QComboBox(this);
    cboInterlace->addItem(tr("Interlace them (15 kHz displays)"), GROOVY_MISTER_INTERLACE_SPLIT);
    cboInterlace->addItem(tr("Leave them progressive"), GROOVY_MISTER_INTERLACE_NEVER);
    cboInterlace->setCurrentIndex(
        groovy_mister_interlace == GROOVY_MISTER_INTERLACE_NEVER ? 1 : 0);
    form->addRow(tr("Modes the monitor cannot scan &progressively:"), cboInterlace);

    cboInterlace->setToolTip(
        tr("Interlacing sends the whole frame and lets the MiSTer split it into fields, so a "
           "31 kHz PC mode can be shown on a 15 kHz CRT. Leaving them progressive refuses those "
           "modes instead, which is what you want on a monitor that can scan them natively."));
    auto *interlaceNote = new QLabel(tr("The MiSTer splits the frame into fields."), this);
    form->addRow(interlaceNote);

    cboCodec = new QComboBox(this);
    cboCodec->addItem(tr("NLC (near-lossless, recommended)"), 7);
    cboCodec->addItem(tr("Uncompressed"), 0);
    cboCodec->setCurrentIndex(groovy_mister_codec == 0 ? 1 : 0);
    form->addRow(tr("&Compression:"), cboCodec);

    spinNear = new QSpinBox(this);
    spinNear->setRange(0, 3);
    spinNear->setValue(groovy_mister_near_level);
    spinNear->setSpecialValueText(tr("0 (lossless)"));
    form->addRow(tr("NLC &quantisation:"), spinNear);

    chkAudio = new QCheckBox(tr("Send the emulated machine's audio too"), this);
    chkAudio->setChecked(groovy_mister_audio != 0);
    form->addRow(chkAudio);

    spinMtu = new QSpinBox(this);
    spinMtu->setRange(576, 9000);
    spinMtu->setSingleStep(4);
    spinMtu->setValue(groovy_mister_mtu);
    form->addRow(tr("Network M&TU:"), spinMtu);

    auto *mtuNote = new QLabel(tr("1500 unless every hop to the MiSTer carries jumbo frames."), this);
    form->addRow(mtuNote);

    outer->addLayout(form);
    outer->addStretch(1);

    connect(chkEnabled, &QCheckBox::toggled, this, &SettingsGroovyMiSTer::refreshEnabledState);
    refreshEnabledState();
}

/* Everything below the checkbox only means something when the output is on. */
void
SettingsGroovyMiSTer::refreshEnabledState()
{
    const bool on = chkEnabled->isChecked();

    editHost->setEnabled(on);
    cboMonitor->setEnabled(on);
    cboInterlace->setEnabled(on);
    cboCodec->setEnabled(on);
    spinNear->setEnabled(on);
    chkAudio->setEnabled(on);
    spinMtu->setEnabled(on);
}

void
SettingsGroovyMiSTer::save(int soft)
{
    (void) soft;

    groovy_mister_enabled = chkEnabled->isChecked() ? 1 : 0;

    const QByteArray host = editHost->text().trimmed().toUtf8();
    strncpy(groovy_mister_host, host.constData(), sizeof(groovy_mister_host) - 1);
    groovy_mister_host[sizeof(groovy_mister_host) - 1] = '\0';

    const QByteArray monitor = cboMonitor->currentData().toString().toUtf8();
    strncpy(groovy_mister_monitor, monitor.constData(), sizeof(groovy_mister_monitor) - 1);
    groovy_mister_monitor[sizeof(groovy_mister_monitor) - 1] = '\0';

    groovy_mister_interlace  = cboInterlace->currentData().toInt();
    groovy_mister_codec      = cboCodec->currentData().toInt();
    groovy_mister_near_level = spinNear->value();
    groovy_mister_audio      = chkAudio->isChecked() ? 1 : 0;
    groovy_mister_mtu        = spinMtu->value();
}
