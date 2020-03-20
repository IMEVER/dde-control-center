#include "manualrestore.h"
#include "backupandrestoremodel.h"

#include <QVBoxLayout>
#include <QLabel>
#include <DBackgroundGroup>
#include <QPushButton>
#include <QDir>
#include <QProcess>
#include <QRadioButton>
#include <QCheckBox>
#include <QProcess>
#include <QFile>
#include <QSettings>
#include <QDebug>
#include <QSharedPointer>
#include <DDialog>
#include <DDBusSender>
#include <QCryptographicHash>

using namespace DCC_NAMESPACE::systeminfo;

class RestoreItem : public QWidget {
    Q_OBJECT
public:
    explicit RestoreItem(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_layout(new QVBoxLayout)
        , m_radioBtn(new QRadioButton)
        , m_content(nullptr)
    {
        m_radioBtn->setCheckable(true);
        m_radioBtn->setMinimumSize(24, 24);

        QVBoxLayout* mainLayout = new QVBoxLayout;
        mainLayout->setMargin(0);
        mainLayout->setSpacing(0);

        mainLayout->addWidget(m_radioBtn, 0, Qt::AlignVCenter | Qt::AlignLeft);
        mainLayout->addLayout(m_layout);

        m_layout->setMargin(0);
        m_layout->setSpacing(5);

        setLayout(mainLayout);
    }

    QRadioButton* radioButton() const {
        return m_radioBtn;
    }

    void setContent(QWidget* content) {
        if (m_content) {
            m_layout->removeWidget(m_content);
            m_content->hide();
            m_content->setParent(nullptr);
            disconnect(m_radioBtn, &QRadioButton::toggled, m_content, &QWidget::setEnabled);
        }

        m_content = content;
        connect(m_radioBtn, &QRadioButton::toggled, m_content, &QWidget::setEnabled);
        content->setEnabled(m_radioBtn->isChecked());

        m_layout->addWidget(m_content);
    }

    void setTitle(const QString& title) {
        m_radioBtn->setText(title);
    }

    bool checked() const {
        return m_radioBtn->isChecked();
    }

    void setChecked(bool check) {
        m_radioBtn->setChecked(check);
        m_content->setEnabled(check);
    }

private:
    QVBoxLayout* m_layout;
    QRadioButton* m_radioBtn;
    QWidget* m_content;
};

ManualRestore::ManualRestore(BackupAndRestoreModel* model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_saveUserDataCheckBox(new QCheckBox)
    , m_directoryChooseWidget(new DFileChooserEdit)
    , m_tipsLabel(new QLabel)
    , m_backupBtn(new QPushButton(tr("Restore")))
    , m_actionType(ActionType::RestoreSystem)
{
    m_tipsLabel->setWordWrap(true);

    QVBoxLayout* mainLayout = new QVBoxLayout;
    mainLayout->setMargin(0);
    mainLayout->setSpacing(5);

    auto createBgGroup = [=](QLayout* layout) -> DBackgroundGroup* {
        DBackgroundGroup* bgGroup = new DBackgroundGroup;
        bgGroup->setLayout(layout);
        bgGroup->setBackgroundRole(QPalette::Window);
        bgGroup->setItemSpacing(1);
        return bgGroup;
    };

    m_systemRestore = new RestoreItem;
    m_manualRestore = new RestoreItem;

    // restore system
    {
        QHBoxLayout* chooseLayout = new QHBoxLayout;
        chooseLayout->setMargin(0);
        chooseLayout->setSpacing(0);
        chooseLayout->addSpacing(5);
        chooseLayout->addWidget(m_saveUserDataCheckBox);
        m_saveUserDataCheckBox->setMinimumSize(24, 24);
        m_saveUserDataCheckBox->setText(tr("Save User Data"));

        m_systemRestore->setTitle(tr("Reset All Settings"));

        QWidget* bgWidget = new QWidget;
        bgWidget->setLayout(chooseLayout);

        m_systemRestore->setContent(bgWidget);

        QVBoxLayout* bgLayout = new QVBoxLayout;
        bgLayout->addWidget(m_systemRestore);
        mainLayout->addWidget(createBgGroup(bgLayout));
    }

    // restore system end

    // manual restore
    {
        QHBoxLayout* chooseLayout = new QHBoxLayout;
        chooseLayout->setMargin(0);
        chooseLayout->setSpacing(0);
        chooseLayout->addSpacing(5);
        chooseLayout->addWidget(new QLabel(tr("Select restore directory")), 0, Qt::AlignVCenter);
        chooseLayout->addSpacing(5);
        chooseLayout->addWidget(m_directoryChooseWidget, 0, Qt::AlignVCenter);

        QWidget* bgWidget = new QWidget;
        bgWidget->setLayout(chooseLayout);

        m_manualRestore->setContent(bgWidget);
        m_manualRestore->setTitle(tr("Manual Restore"));

        QVBoxLayout* bgLayout = new QVBoxLayout;
        bgLayout->addWidget(m_manualRestore);
        mainLayout->addWidget(createBgGroup(bgLayout));
    }

    // manual restore end

    mainLayout->addWidget(m_tipsLabel);
    mainLayout->addStretch();
    mainLayout->addWidget(m_backupBtn);

    setLayout(mainLayout);

    m_tipsLabel->hide();

    QFileDialog* fileDialog = new QFileDialog(this);
    fileDialog->setFileMode(QFileDialog::Directory);
    m_directoryChooseWidget->setFileDialog(fileDialog);

    m_systemRestore->radioButton()->setChecked(true);

    connect(m_backupBtn, &QPushButton::clicked, this, &ManualRestore::restore, Qt::QueuedConnection);
    connect(m_systemRestore->radioButton(), &QRadioButton::toggled, this, &ManualRestore::onItemChecked);
    connect(m_manualRestore->radioButton(), &QRadioButton::toggled, this, &ManualRestore::onItemChecked);
    connect(model, &BackupAndRestoreModel::restoreButtonEnabledChanged, m_backupBtn, &QPushButton::setEnabled);
    connect(model, &BackupAndRestoreModel::manualRestoreCheckFailedChanged, this, &ManualRestore::onManualRestoreCheckFailed);

    m_backupBtn->setEnabled(model->restoreButtonEnabled());
    m_directoryChooseWidget->lineEdit()->setText(model->restoreDirectory());
    m_saveUserDataCheckBox->setChecked(model->formatData());
    onManualRestoreCheckFailed(model->manualRestoreCheckFailed());
}

void ManualRestore::onItemChecked()
{
    QRadioButton* button = qobject_cast<QRadioButton*>(sender());

    const bool check = button == m_systemRestore->radioButton();

    auto setCheck = [&](RestoreItem* item, bool c) {
        item->radioButton()->blockSignals(true);
        item->setChecked(c);
        item->radioButton()->blockSignals(false);
    };

    setCheck(m_systemRestore, check);
    setCheck(m_manualRestore, !check);

    m_actionType = check ? ActionType::RestoreSystem : ActionType::ManualRestore;
}

void ManualRestore::onManualRestoreCheckFailed(bool failed)
{
    m_tipsLabel->setText(tr("Backup file is invalid."));
    m_tipsLabel->setVisible(failed);
}

void ManualRestore::restore()
{
    if (m_actionType == ActionType::RestoreSystem) {
        const bool formatData = !m_saveUserDataCheckBox->isChecked();

        DDialog dialog;
        QString message{ tr(
                             "This will reset all system settings to their defaults. Your data, username and "
                             "password will not be deleted, please confirm and continue") };

        if (formatData) {
            message =
                tr("This will reinstall the system and clear all user data. It is risky, "
                   "please confirm and continue");
        }

        dialog.setMessage(message);
        dialog.addButton(tr("Cancel"));

        {
            int result = dialog.addButton(tr("Confirm"), true, DDialog::ButtonWarning);
            if (dialog.exec() != result) {
                return;
            }
        }

        DDialog reboot;

        if (formatData) {
            reboot.setMessage(tr("You should reboot the computer to erase all content and settings, reboot now?"));
        }
        else {
            reboot.setMessage(tr("You should reboot the computer to reset all settings, reboot now?"));
        }

        reboot.addButton(tr("Cancel"));
        {
            int result = reboot.addButton(tr("Confirm"), true, DDialog::ButtonWarning);
            if (reboot.exec() != result) {
                return;
            }
        }

        Q_EMIT requestSystemRestore(formatData);
    }

    if (m_actionType == ActionType::ManualRestore) {
        m_tipsLabel->hide();

        // TODO(justforlxz): 判断内容的有效性
        const QString& selectPath = m_directoryChooseWidget->lineEdit()->text();

        if (selectPath.isEmpty()) {
            return;
        }

        Q_EMIT requestManualRestore(selectPath);
    }
}

#include "manualrestore.moc"