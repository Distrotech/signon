/*
 * This file is part of signon
 *
 * Copyright (C) 2009-2010 Nokia Corporation.
 *
 * Contact: Aurel Popirtac <ext-aurel.popirtac@nokia.com>
 * Contact: Alberto Mardegan <alberto.mardegan@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <QtDebug>
#include <QDBusConnection>

#include "signondaemon.h"
#include "signond-common.h"
#include "signondaemonadaptor.h"
#include "signonidentity.h"
#include "signonauthsession.h"


using namespace SignOn;

namespace SignonDaemonNS {

    RequestCounter *RequestCounter::m_pInstance = NULL;

    SignonDaemon::SignonDaemon(QObject *parent) : QObject(parent)
    {
        m_pCAMManager = NULL;
    }

    SignonDaemon::~SignonDaemon()
    {
        qDeleteAll(m_storedIdentities);
        qDeleteAll(m_unstoredIdentities);
        SignonAuthSession::stopAllAuthSessions();

        if (m_pCAMManager) {
            m_pCAMManager->closeCredentialsSystem();
            m_pCAMManager->deleteLater();
        }

        delete RequestCounter::instance();
    }

    bool SignonDaemon::init()
    {
        /* DBus Service init */
        QDBusConnection connection = SIGNON_BUS;

        if (!connection.isConnected()) {
            QDBusError err = connection.lastError();
            TRACE() << "Connection cannot be established:" << err.errorString(err.type());
            TRACE() << err.message();
            return false;
        }

        QDBusConnection::RegisterOptions registerOptions = QDBusConnection::ExportAllContents;

#ifndef SIGNON_DISABLE_ACCESS_CONTROL
        (void)new SignonDaemonAdaptor(this);
        registerOptions = QDBusConnection::ExportAdaptors;
#endif

        if (!connection.registerObject(SIGNON_DAEMON_OBJECTPATH, this, registerOptions)) {
            TRACE() << "Object cannot be registered";
            return false;
        }

        if (!connection.registerService(SIGNON_SERVICE)) {
            QDBusError err = connection.lastError();
            TRACE() << "Service cannot be registered: " << err.errorString(err.type());
            return false;
        }

        /*
            Secure storage init
            TODO - make this callable only by external entity
            (e.g. boot script or whichever process has the passphrase)
        */
        if (!initSecureStorage(QByteArray()))
            qFatal("Signond: Cannot initialize credentials secure storage.");

        TRACE() << "Signond SUCCESSFULLY initialized.";

        listDBusInterfaces();

        // TODO - remove this
        QTimer *requestCounterTimer = new QTimer(this);
        requestCounterTimer->setInterval(500000);
        connect(requestCounterTimer,
                SIGNAL(timeout()),
                this,
                SLOT(displayRequestsCount()));
        requestCounterTimer->start();
        //TODO - end

        return true;
    }


    bool SignonDaemon::initSecureStorage(const QByteArray &lockCode)
    {
        Q_UNUSED(lockCode)

        m_pCAMManager = CredentialsAccessManager::instance();
        CAMConfiguration config;
        //Leaving encryption disabled for the moment: dm_mod not loaded by default.
        config.m_useEncryption = false;

        if (!m_pCAMManager->init(config))  {
            qFatal("Signond: Cannot set proper configuration of CAM");
            delete m_pCAMManager;
            m_pCAMManager = NULL;
            return false;
        }

        if (!m_pCAMManager->openCredentialsSystem()) {
            qFatal("Signond: openCredentialsSystem failed with code %d",
                   m_pCAMManager->lastError());
            delete m_pCAMManager;
            m_pCAMManager = NULL;
            return false;
        }
        return true;
    }

    void SignonDaemon::displayRequestsCount()
    {
        TRACE() << "\n\n\nUnstored identities:" << m_unstoredIdentities.count()
                << "\nStored identities:" << m_storedIdentities.count()
                << "\nService requests:" << RequestCounter::instance()->serviceRequests()
                << "\nIdentity requests:" << RequestCounter::instance()->identityRequests()
                << "\n\n";
    }

    void SignonDaemon::unregisterIdentity(SignonIdentity *identity)
    {
        if (m_storedIdentities.contains(identity->id()))
            m_storedIdentities.remove(identity->id());
        else
            m_unstoredIdentities.remove(identity->objectName());

        identity->deleteLater();
    }

    void SignonDaemon::identityStored(SignonIdentity *identity)
    {
        m_unstoredIdentities.remove(identity->objectName());
        m_storedIdentities.insert(identity->id(), identity);
    }

    void SignonDaemon::registerNewIdentity(QDBusObjectPath &objectPath)
    {
        RequestCounter::instance()->addServiceResquest();

        TRACE() << "Registering new identity:";

        SignonIdentity *identity = SignonIdentity::createIdentity(SSO_NEW_IDENTITY, this);

        if (identity == NULL)
        {
            QDBusMessage errReply = message().createErrorReply(
                    SSO_DAEMON_INTERNAL_SERVER_ERR_NAME,
                    SSO_DAEMON_INTERNAL_SERVER_ERR_STR + QLatin1String("Could not create remote Identity object."));
            SIGNON_BUS.send(errReply);
            return;
        }

        m_unstoredIdentities.insert(identity->objectName(), identity);

        objectPath = QDBusObjectPath(identity->objectName());
    }

    void SignonDaemon::registerStoredIdentity(const quint32 id, QDBusObjectPath &objectPath, QList<QVariant> &identityData)
    {
        RequestCounter::instance()->addServiceResquest();

        TRACE() << "Registering identity:" << id;

        //1st check if the existing identity is in cache
        SignonIdentity *identity = m_storedIdentities.value(id, NULL);

        //if not create it
        if (identity == NULL)
            identity = SignonIdentity::createIdentity(id, this);

        if (identity == NULL)
        {
            QDBusMessage errReply = message().createErrorReply(
                    SSO_DAEMON_INTERNAL_SERVER_ERR_NAME,
                    SSO_DAEMON_INTERNAL_SERVER_ERR_STR + QLatin1String("Could not create remote Identity object."));
            SIGNON_BUS.send(errReply);
            return;
        }

        bool ok;
        SignonIdentityInfo info = identity->queryInfo(ok);

        if (info.m_id == 0)
        {
            QDBusMessage errReply = message().createErrorReply(
                                                            SSO_IDENTITY_NOT_FOUND_ERR_NAME,
                                                            SSO_IDENTITY_NOT_FOUND_ERR_STR);
            SIGNON_BUS.send(errReply);
            objectPath = QDBusObjectPath();
            return;
        }

        //cache the identity as stored
        m_storedIdentities.insert(identity->id(), identity);

        identityData = info.toVariantList();

        TRACE() << "DONE REGISTERING IDENTITY";
        objectPath = QDBusObjectPath(identity->objectName());
    }

    QStringList SignonDaemon::queryMethods()
    {
        RequestCounter::instance()->addServiceResquest();

        QDir pluginsDir(SIGNON_PLUGINS_DIR);
         //TODO: in the future remove the sym links comment
         QStringList fileNames = pluginsDir.entryList(
                 QStringList() << QLatin1String("*.so*"), QDir::Files | QDir::NoDotAndDotDot);

         QStringList ret;
         QString fileName;
         foreach(fileName, fileNames)
         {
             if (fileName.startsWith(QLatin1String("lib")))
             {
                 fileName = fileName.mid(3, fileName.indexOf(QLatin1String("plugin")) -3);
                 if ((fileName.length() > 0) && !ret.contains(fileName))
                     ret << fileName;
             }
         }

         return ret;
    }

    QStringList SignonDaemon::queryMechanisms(const QString &method)
    {
        RequestCounter::instance()->addServiceResquest();
        TRACE() << "\n\n\n Querying mechanisms\n\n";

        QStringList mechs = SignonSessionCore::loadedPluginMethods(method);

        if (mechs.size())
            return mechs;

        PluginProxy *plugin = PluginProxy::createNewPluginProxy(method);

        if (!plugin)
        {
            TRACE() << "Could not load plugin of type: " << method;
            QDBusMessage errReply = message().createErrorReply(
                    SSO_DAEMON_METHOD_NOT_KNOWN_ERR_NAME,
                    QString(SSO_DAEMON_METHOD_NOT_KNOWN_ERR_STR
                            + QLatin1String("Method %1 is not known or could not load specific configuration.")).arg(method));
            SIGNON_BUS.send(errReply);
            return QStringList();
        }

        mechs = plugin->mechanisms();
        delete plugin;

        return mechs;
    }


    QList<QVariant> SignonDaemon::queryIdentities(const QMap<QString, QVariant> &filter)
    {
        RequestCounter::instance()->addServiceResquest();

        TRACE() << "\n\n\n Querying identities\n\n";

        CredentialsDB *db = m_pCAMManager->credentialsDB();
        if (!db) {
            qCritical() << Q_FUNC_INFO << m_pCAMManager->lastError();
            return QList<QVariant>();
        }

        QMap<QString, QString> filterLocal;
        QMapIterator<QString, QVariant> it(filter);
        while(it.hasNext())
        {
            it.next();
            filterLocal.insert(it.key(), it.value().toString());
        }

        QList<SignonIdentityInfo> credentials = db->credentials(filterLocal);

        if (db->errorOccurred())
        {
            QDBusMessage errReply = message().createErrorReply(
                    SSO_DAEMON_INTERNAL_SERVER_ERR_NAME,
                    SSO_DAEMON_INTERNAL_SERVER_ERR_STR + QLatin1String("Querying database error occurred."));
            SIGNON_BUS.send(errReply);
            return QList<QVariant>();
        }

        return SignonIdentityInfo::listToVariantList(credentials);
    }

    bool SignonDaemon::clear()
    {
        RequestCounter::instance()->addServiceResquest();
        TRACE() << "\n\n\n Clearing DB\n\n";
        CredentialsDB *db = m_pCAMManager->credentialsDB();
        if (!db) {
            qCritical() << Q_FUNC_INFO << m_pCAMManager->lastError();
            return false;
        }

        if (!db->clear())
        {
            QDBusMessage errReply = message().createErrorReply(
                                                    SSO_DAEMON_INTERNAL_SERVER_ERR_NAME,
                                                    QString(SSO_DAEMON_INTERNAL_SERVER_ERR_STR
                                                            + QLatin1String("Database error occurred.")));
            SIGNON_BUS.send(errReply);
            return false;
        }
        return true;
    }

    QString SignonDaemon::getAuthSessionObjectPath(const quint32 id, const QString type)
    {
        return SignonAuthSession::getAuthSessionObjectPath(id, type, this);
    }

    bool SignonDaemon::setDeviceLockCode(const QByteArray &oldLockCode,
                                         const QByteArray &newLockCode)
    {
        Q_UNUSED(oldLockCode)
        Q_UNUSED(newLockCode)

        // TODO - implement this
        TRACE() << "setDeviceLockCode:   oldLockCode = " << oldLockCode << ", newLockCode = " << newLockCode;
        return false;
    }

    bool SignonDaemon::setSim(const QByteArray& simData,
                              const QByteArray& checkData)
    {
        Q_UNUSED(simData)
        Q_UNUSED(checkData)

        // TODO - implement this
        TRACE() << "setSim:   simData = " << simData << ", checkData = " << checkData;
        return false;
    }

    bool SignonDaemon::remoteLock(const QByteArray &lockCode)
    {
        Q_UNUSED(lockCode)

        // TODO - implement this
        TRACE() << "remoteLock:   lockCode = " << lockCode;
        return false;
    }

    void SignonDaemon::listDBusInterfaces()
    {
        QDBusReply<QStringList> reply = SIGNON_BUS.interface()->registeredServiceNames();
        QStringList list = reply.value();

        QString servicesList = QLatin1String("DBUS registered services: \n");
        servicesList += list.join(QLatin1String("\n"));

        TRACE() << "\n\n" << servicesList.toAscii().data() << "\n";
    }

} //namespace SignonDaemonNS
