/*
 * Copyright (C) 2011 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * Maintainer: sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "accountsworker.h"
#include "user.h"

#include <QFileDialog>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <pwd.h>
#include <unistd.h>
#include <libintl.h>
#include <random>
#include <crypt.h>

using namespace dcc::accounts;

const QString AccountsService("com.deepin.daemon.Accounts");
const QString DisplayManagerService("org.freedesktop.DisplayManager");

AccountsWorker::AccountsWorker(UserModel *userList, QObject *parent)
    : QObject(parent)
    , m_accountsInter(new Accounts(AccountsService, "/com/deepin/daemon/Accounts", QDBusConnection::systemBus(), this))
#ifdef DCC_ENABLE_ADDOMAIN
    , m_notifyInter(new Notifications("org.freedesktop.Notifications", "/org/freedesktop/Notifications", QDBusConnection::sessionBus(), this))
#endif
    , m_dmInter(new DisplayManager(DisplayManagerService, "/org/freedesktop/DisplayManager", QDBusConnection::systemBus(), this))
    , m_userModel(userList)
{
    struct passwd *pws;
    pws = getpwuid(getuid());
    m_currentUserName = QString(pws->pw_name);

    connect(m_accountsInter, &Accounts::UserListChanged, this, &AccountsWorker::onUserListChanged);
    connect(m_accountsInter, &Accounts::UserAdded, this, &AccountsWorker::addUser);
    connect(m_accountsInter, &Accounts::UserDeleted, this, &AccountsWorker::removeUser);

    connect(m_dmInter, &DisplayManager::SessionsChanged, this, &AccountsWorker::updateUserOnlineStatus);

    m_accountsInter->setSync(false);
    m_dmInter->setSync(false);
#ifdef DCC_ENABLE_ADDOMAIN
    m_notifyInter->setSync(false);
#endif
    onUserListChanged(m_accountsInter->userList());
    updateUserOnlineStatus(m_dmInter->sessions());
}

void AccountsWorker::active()
{
    for (auto it(m_userInters.cbegin()); it != m_userInters.cend(); ++it)
    {
        it.key()->setName(it.value()->userName());
        it.key()->setAutoLogin(it.value()->automaticLogin());
        it.key()->setAvatars(it.value()->iconList());
        it.key()->setCurrentAvatar(it.value()->iconFile());
    }
#ifdef DCC_ENABLE_ADDOMAIN
    refreshADDomain();
#endif
}

void AccountsWorker::randomUserIcon(User *user)
{
    QDBusPendingCall call = m_accountsInter->RandUserIcon();
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, [user, call] {
        if (!call.isError()) {
            QDBusReply<QString> reply = call.reply();
            user->setCurrentAvatar(reply.value());
        }
    });
}

void AccountsWorker::createAccount(const User *user)
{
    qDebug() << "create account " << user;
    emit requestFrameAutoHide(false);

    QFutureWatcher<CreationResult*> *watcher = new QFutureWatcher<CreationResult*>(this);
    connect(watcher, &QFutureWatcher<CreationResult*>::finished, [this, watcher] {
        CreationResult *result = watcher->result();
        emit accountCreationFinished(result);
        emit requestFrameAutoHide(true);
    });

    QFuture<CreationResult*> future = QtConcurrent::run(this, &AccountsWorker::createAccountInternal, user);
    watcher->setFuture(future);
}

void AccountsWorker::addNewAvatar(User *user)
{
    AccountsUser *userInter = m_userInters[user];
    Q_ASSERT(userInter);

    emit requestFrameAutoHide(false);
    QFileDialog fd;
    fd.setNameFilter(tr("Images") + "(*.png *.bmp *.jpg *.jpeg)");

    QStringList directory = QStandardPaths::standardLocations(QStandardPaths::PicturesLocation);
    if (!directory.isEmpty())
        fd.setDirectory(directory.first());

    do {
        if (fd.exec() != QFileDialog::Accepted)
            break;

        const QString file = fd.selectedFiles().first();
        userInter->SetIconFile(file).waitForFinished();
    } while (false);

    QTimer::singleShot(100, this, [=] { emit requestFrameAutoHide(true); });
}

void AccountsWorker::setAvatar(User *user, const QString &iconPath)
{
    AccountsUser *ui = m_userInters[user];
    Q_ASSERT(ui);

    ui->SetIconFile(iconPath);
}

void AccountsWorker::setFullname(User *user, const QString &fullname)
{
    AccountsUser *ui = m_userInters[user];
    Q_ASSERT(ui);

    emit requestFrameAutoHide(false);

    QDBusPendingCall call = ui->SetFullName(fullname);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [=] {
        if (!call.isError()) {
            emit accountFullNameChangeFinished();
        }

        emit requestFrameAutoHide(true);
        watcher->deleteLater();
    });
}

void AccountsWorker::deleteAccount(User *user, const bool deleteHome)
{
    emit requestFrameAutoHide(false);
    m_accountsInter->DeleteUser(user->name(), deleteHome).waitForFinished();
    QTimer::singleShot(100, this, [=] { emit requestFrameAutoHide(true); });
}

void AccountsWorker::setAutoLogin(User *user, const bool autoLogin)
{
    AccountsUser *ui = m_userInters[user];
    Q_ASSERT(ui);

    // because this operate need root permission, we must wait for finished and refersh result
    emit requestFrameAutoHide(false);

    QDBusPendingCall call = ui->SetAutomaticLogin(autoLogin);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [=] {
        if (call.isError()) {
            emit user->autoLoginChanged(user->autoLogin());
        }

        emit requestFrameAutoHide(true);
        watcher->deleteLater();
    });
}

void AccountsWorker::onUserListChanged(const QStringList &userList)
{
    for (const auto &path : userList)
        if (!m_userModel->contains(path))
            addUser(path);
}

void AccountsWorker::setPassword(User *user, const QString &oldpwd, const QString &passwd)
{
    /* freedesktop accounts dbus way.
    AccountsUser *userInter = m_userInters[user];
    Q_ASSERT(userInter);

    emit requestFrameAutoHide(false);
    userInter->SetPassword(passwd).waitForFinished();
    QTimer::singleShot(100, this, [=] { emit requestFrameAutoHide(true); });
    */

    // passwd command way
    QProcess *cmd = new QProcess;
    cmd->setProgram("passwd");
    cmd->setArguments(QStringList(user->name()));

    connect(cmd, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), cmd, [cmd] (int exitCode, QProcess::ExitStatus exitStatus) {
        qDebug() << "passwd command exit status and exit code:" << exitCode << exitStatus;
        qWarning() << cmd->errorString() << QString(cmd->readAllStandardOutput()) << QString(cmd->readAllStandardError());
        cmd->deleteLater();
    });
    cmd->start();

    if (!cmd->waitForStarted()) {
        qWarning() << "failed to start passwd command";
        return;
    }

    cmd->write(QString("%1\n%2\n%3").arg(oldpwd).arg(passwd).arg(passwd).toLatin1());
    cmd->closeWriteChannel();
}

void AccountsWorker::deleteUserIcon(User *user, const QString &iconPath)
{
    AccountsUser *userInter = m_userInters[user];
    Q_ASSERT(userInter);

    userInter->DeleteIconFile(iconPath);
}

void AccountsWorker::addUser(const QString &userPath)
{
    AccountsUser *userInter = new AccountsUser(AccountsService, userPath, QDBusConnection::systemBus(), this);
    userInter->setSync(false);

    User *user = new User(this);

    connect(userInter, &AccountsUser::UserNameChanged, [=](const QString &name) {
        user->setName(name);
        user->setOnline(m_onlineUsers.contains(name));
        user->setIsCurrentUser(name == m_currentUserName);
    });

    connect(userInter, &AccountsUser::AutomaticLoginChanged, user, &User::setAutoLogin);
    connect(userInter, &AccountsUser::IconListChanged, user, &User::setAvatars);
    connect(userInter, &AccountsUser::IconFileChanged, user, &User::setCurrentAvatar);
    connect(userInter, &AccountsUser::FullNameChanged, user, &User::setFullname);
    connect(userInter, &AccountsUser::NoPasswdLoginChanged, user, &User::setNopasswdLogin);

    user->setName(userInter->userName());
    user->setFullname(userInter->fullName());
    user->setAutoLogin(userInter->automaticLogin());
    user->setAvatars(userInter->iconList());
    user->setCurrentAvatar(userInter->iconFile());
    user->setNopasswdLogin(userInter->noPasswdLogin());

    m_userInters[user] = userInter;
    m_userModel->addUser(userPath, user);
}

void AccountsWorker::removeUser(const QString &userPath)
{
    for (AccountsUser *userInter : m_userInters.values())
    {
        if (userInter->path() == userPath)
        {
            User *user = m_userInters.key(userInter);

            m_userInters.remove(user);
            m_userModel->removeUser(userPath);

            return;
        }
    }
}

void AccountsWorker::setNopasswdLogin(User *user, const bool nopasswdLogin)
{
    AccountsUser *userInter = m_userInters[user];
    Q_ASSERT(userInter);

    emit requestFrameAutoHide(false);

    QDBusPendingCall call = userInter->EnableNoPasswdLogin(nopasswdLogin);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [=] {
        if (call.isError()) {
            emit user->nopasswdLoginChanged(user->nopasswdLogin());
        }

        emit requestFrameAutoHide(true);
        watcher->deleteLater();
    });
}

#ifdef DCC_ENABLE_ADDOMAIN
void AccountsWorker::refreshADDomain()
{
    QProcess *process = new QProcess(this);
    process->start("/opt/pbis/bin/enum-users");

    connect(process, &QProcess::readyReadStandardOutput, this, [=] {
        QRegularExpression re("Name:\\s+(\\w+)");
        QRegularExpressionMatch match = re.match(process->readAll());
        m_userModel->setIsJoinADDomain(match.hasMatch());
    });

    connect(process, static_cast<void (QProcess::*)(int)>(&QProcess::finished), process, &QProcess::deleteLater);
}
#endif

#ifdef DCC_ENABLE_ADDOMAIN
void AccountsWorker::ADDomainHandle(const QString &server, const QString &admin, const QString &password)
{
    QProcess *process = new QProcess(this);

    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [=] (int exitCode, QProcess::ExitStatus exitStatus){

        const bool isJoin = m_userModel->isJoinADDomain();
        QString message;

        if (exitStatus == QProcess::NormalExit && !exitCode) {
            message = isJoin ? tr("Your host was removed from the domain server successfully.")
                             : tr("Your host joins the domain server successfully.");
        } else {
            message = isJoin ? tr("Your host failed to leave the domain server.")
                             : tr("Your host failed to join the domain server.");
        }

        m_notifyInter->Notify("", QDateTime::currentMSecsSinceEpoch(), "dde", tr("AD domain settings"), message, QStringList(), QVariantMap(), 0);

        refreshADDomain();

        process->deleteLater();
    });

    if (m_userModel->isJoinADDomain()) {
        process->start("pkexec", QStringList() << "/opt/pbis/bin/domainjoin-cli" << "leave" << admin << password);
    } else {
        process->start("pkexec", QStringList() << "/opt/pbis/bin/domainjoin-cli" << "join" << server << admin << password);
    }

    process->waitForFinished();
}
#endif

void AccountsWorker::updateUserOnlineStatus(const QList<QDBusObjectPath> paths)
{
    m_onlineUsers.clear();

    for (const QDBusObjectPath &path : paths) {
        Session tmpSession(DisplayManagerService, path.path(), QDBusConnection::systemBus());
        m_onlineUsers << tmpSession.userName();
    }

    for (User *user : m_userModel->userList()) {
        const bool online = m_onlineUsers.contains(user->name());
        user->setOnline(online);
    }
}

CreationResult *AccountsWorker::createAccountInternal(const User *user)
{
    CreationResult *result = new CreationResult;

    // validate username
    QDBusPendingReply<bool, QString, int> reply = m_accountsInter->IsUsernameValid(user->name());
    reply.waitForFinished();
    if (reply.isError()) {
        result->setType(CreationResult::UserNameError);
        result->setMessage(reply.error().message());

        return result;
    }
    bool validation = reply.argumentAt(0).toBool();
    if (!validation) {
        result->setType(CreationResult::UserNameError);
        result->setMessage(dgettext("dde-daemon", reply.argumentAt(1).toString().toUtf8().data()));
        return result;
    }

    // validate password
    if (user->password() != user->repeatPassword()) {
        result->setType(CreationResult::PasswordMatchError);
        result->setMessage(tr("Password not match."));
        return result;
    }

    QDBusObjectPath path = m_accountsInter->CreateUser(user->name(), user->name(), 1);
    const QString userPath = path.path();
    if (userPath.isEmpty() || userPath.isNull()) {
        result->setType(CreationResult::UnknownError);
        result->setMessage("no method call result on CreateUser");
        return result;
    }

    AccountsUser *userDBus = new AccountsUser("com.deepin.daemon.Accounts", userPath, QDBusConnection::systemBus(), this);
    if (!userDBus->isValid()) {
        result->setType(CreationResult::UnknownError);
        result->setMessage("user dbus is still not valid.");

        return result;
    }

    //TODO(hualet): better to check all the call results.
    bool sifResult = !userDBus->SetIconFile(user->currentAvatar()).isError();
    bool spResult = !userDBus->SetPassword(cryptUserPassword(user->password())).isError();

    if (!sifResult || !spResult) {
        result->setType(CreationResult::UnknownError);
        if (!sifResult) result->setMessage("set icon file for new created user failed.");
        if (!spResult) result->setMessage("set password for new created user failed");

        return result;
    }

    return result;
}

QString AccountsWorker::cryptUserPassword(const QString &password)
{
    /*
        NOTE(kirigaya): Password is a combination of salt and crypt function.
        slat is begin with $6$, 16 byte of random values, at the end of $.
        crypt function will return encrypted values.
     */

    const QString seedchars("./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    char salt[] = "$6$................$";

    std::random_device r;
    std::default_random_engine e1(r());
    std::uniform_int_distribution<int> uniform_dist(0, seedchars.size());

    // Random access to a character in a restricted list
    for (int i = 0; i != 16; i++) {
        salt[3 + i] = seedchars.at(uniform_dist(e1)).toLatin1();
    }

#ifdef QT_DEBUG
    qDebug() << crypt(password.toUtf8().data(), salt);
#endif

    return crypt(password.toUtf8().data(), salt);
}
