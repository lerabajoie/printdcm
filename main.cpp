/*
 * Copyright (C) 2014 Irkutsk Diagnostic Center.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "product.h"
#include "printscp.h"
#include "storescp.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#define HAVE_CONFIG_H
#include <dcmtk/config/osconfig.h> /* make sure OS specific configuration is included first */
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>

#define DEFAULT_SPOOL_INTERVAL 1800

static void cleanChildren()
{
#ifdef HAVE_WAITPID
    int stat_loc;
#elif HAVE_WAIT3
    struct rusage rusage;
#if defined(__NeXT__)
    /* some systems need a union wait as argument to wait3 */
    union wait status;
#else
    int        status;
#endif
#endif

#if defined(HAVE_WAITPID) || defined(HAVE_WAIT3)
    int child = 1;
    int options = WNOHANG;
    while (child > 0)
    {
#ifdef HAVE_WAITPID
        child = (int)(waitpid(-1, &stat_loc, options));
#elif defined(HAVE_WAIT3)
        child = wait3(&status, options, &rusage);
#endif
        if (child < 0)
        {
            if (errno != ECHILD)
            {
                qDebug() << "wait for child failed: " << errno;
            }
        }
    }
#endif
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(PRODUCT_SHORT_NAME);
    app.setOrganizationName(ORGANIZATION_DOMAIN);

    QSettings settings;
    auto logLevel = settings.value("log-level").toString();
    if (!logLevel.isEmpty())
    {
        auto level = log4cplus::getLogLevelManager().fromString(logLevel.toUtf8().constData());
        log4cplus::Logger::getRoot().setLogLevel(level);
    }

    auto debug = settings.value("debug").toBool();
    if (debug)
    {
        log4cplus::Logger log = log4cplus::Logger::getInstance("dcmtk.dcmpstat.dump");
        log.setLogLevel(OFLogger::DEBUG_LOG_LEVEL);
    }

    auto port = settings.value("port", DEFAULT_LISTEN_PORT).toInt();
    auto tout = settings.value("timeout", DEFAULT_TIMEOUT).toInt();
    T_ASC_Network *net;
    OFCondition cond = ASC_initializeNetwork(NET_ACCEPTOR, port, tout, &net);
    if (cond.bad())
    {
        qDebug() << "cannot initialise network" << QString::fromLocal8Bit(cond.text());
        return 1;
    }

#if defined(HAVE_SETUID) && defined(HAVE_GETUID)
    /* return to normal uid so that we can't do too much damage in case
     * things go very wrong.   Only relevant if the program is setuid root,
     * and run by another user.  Running as root user may be
     * potentially disasterous if this program screws up badly.
     */
    auto err = setuid(getuid());
    if (err)
    {
        qDebug() << "setuid failed" << errno << "this may dangerous";
    }
#endif

#ifdef HAVE_FORK
    int listen_timeout=1;
#else
    int listen_timeout=1000;
#endif

    Q_FOREVER
    {
        // Use new print SCP object for each association
        //
        PrintSCP printSCP;

        do
        {
           cleanChildren();
        }
        while (!ASC_associationWaiting(net, listen_timeout));

        // Ready to accept an association
        //
        auto ass = printSCP.negotiateAssociation(net);

        if (DVPSJ_error == ass)
        {
            // Association has already been deleted,
            // we just wait for the next client to connect.
        }
        else if (DVPSJ_terminate == ass)
        {
            // Our mission is over
            //
            ASC_dropNetwork(&net);
            break;
        }
        else if (DVPSJ_success == ass)
        {
#if defined(HAVE_FORK) && !defined(QT_DEBUG)
            // A new client just been connected.
            //
            auto pid = fork();
            if (pid < 0)
            {
                qDebug() << "fork() failed, err" << errno;
                printSCP.dropAssociations();
            }
            else if (pid == 0)
            {
                // Do the real work.
                //
                printSCP.handleClient();
                break;
            }
#else
            printSCP.handleClient();
#endif
        }

        // Retry failed prints
        //
        auto spoolPath = settings.value("spool-path").toString();
        if (!spoolPath.isEmpty())
        {
            if (QDateTime::currentDateTime() > settings.value("next-spool-ts").toDateTime())
            {
                auto spoolInterval = settings.value("spool-interval-in-seconds", DEFAULT_SPOOL_INTERVAL).toInt();
                settings.setValue("next-spool-ts", QDateTime::currentDateTime().addSecs(spoolInterval));

                // Retry failed web queries
                //
                Q_FOREACH (auto file, QDir(spoolPath).entryInfoList(QDir::Files))
                {
                    DcmFileFormat dcmFF;
                    auto filePath = file.absoluteFilePath();
                    cond = dcmFF.loadFile(filePath.toLocal8Bit());
                    if (cond.bad())
                    {
                        qDebug() << "Failed to load " << filePath << ": " << QString::fromLocal8Bit(cond.text());
                        continue;
                    }

                    const char* printer = nullptr;
                    dcmFF.getDataset()->findAndGetString(DCM_RETIRED_PrintQueueID, printer);
                    PrintSCP printSCP(nullptr, QString::fromUtf8(printer));

                    if (printSCP.webQuery(dcmFF.getDataset()))
                    {
                        foreach (auto server, settings.value("storage-servers").toStringList())
                        {
                            StoreSCP sscp(server);
                            const char* SOPInstanceUID = nullptr;
                            dcmFF.getDataset()->findAndGetString(DCM_SOPInstanceUID, SOPInstanceUID);

                            cond = sscp.sendToServer(dcmFF.getDataset(), SOPInstanceUID);
                            if (cond.bad())
                            {
                                // The Web query secceded, but store failed.
                                // Move the file down to the queue.
                                // At this point, we will copy the file many times,
                                // for each failed store server.
                                //
                                auto newName = spoolPath.append(QDir::separator())
                                        .append(server).append(QDir::separator())
                                        .append(file.fileName());

                                if (!QFile::copy(filePath, newName))
                                {
                                    qDebug() << "Failed to copy file " << filePath
                                             << "to" << newName << ": " << strerror(errno);
                                }
                            }
                        }

                        if (!QFile::remove(filePath))
                        {
                            qDebug() << "Failed to remove file " << filePath << ": " << strerror(errno);
                        }
                    }
                }

                foreach (auto server, settings.value("storage-servers").toStringList())
                {
                    StoreSCP sscp(server);
                    Q_FOREACH (auto file, QDir(spoolPath.append(QDir::separator()).append(server)).entryInfoList(QDir::Files))
                    {
                        DcmFileFormat dcmFF;
                        auto filePath = file.absoluteFilePath();
                        cond = dcmFF.loadFile(filePath.toLocal8Bit());
                        if (cond.bad())
                        {
                            qDebug() << "Failed to load " << filePath << ": " << QString::fromLocal8Bit(cond.text());
                            continue;
                        }

                        const char* SOPInstanceUID = nullptr;
                        dcmFF.getDataset()->findAndGetString(DCM_SOPInstanceUID, SOPInstanceUID);

                        cond = sscp.sendToServer(dcmFF.getDataset(), SOPInstanceUID);
                        if (cond.good())
                        {
                            if (!QFile::remove(filePath))
                            {
                                qDebug() << "Failed to remove file " << filePath << ": " << strerror(errno);
                            }
                        }
                    }
                }
            }
        }

    }

    cleanChildren();
    return 0;
}
