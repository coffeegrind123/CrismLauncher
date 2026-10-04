// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;

/** Download mirror choices (BMCLAPI for game files, MCIM for Modrinth and CurseForge), shared by
 *  the Services settings page and the setup wizard. See net/DownloadMirror.h. */
class MirrorSettingsWidget : public QWidget {
    Q_OBJECT

   public:
    explicit MirrorSettingsWidget(QWidget* parent = nullptr);

    void loadSettings();
    void applySettings();
    void retranslateUi();

   private:
    void fillComboBoxes();

    QGroupBox* m_gameFilesGroup;
    QLabel* m_gameFilesNote;
    QComboBox* m_gameFilesMode;
    QLineEdit* m_gameFilesURL;

    QGroupBox* m_modsGroup;
    QLabel* m_modsNote;
    QLabel* m_modrinthLabel;
    QComboBox* m_modrinthMirror;
    QLabel* m_curseForgeLabel;
    QComboBox* m_curseForgeMirror;
};
