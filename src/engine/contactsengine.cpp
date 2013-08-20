/*
 * Copyright (C) 2013 Jolla Ltd. <andrew.den.exter@jollamobile.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "contactsengine.h"

#include "contactsdatabase.h"
#include "contactnotifier.h"
#include "contactreader.h"
#include "contactwriter.h"

#include "qtcontacts-extensions.h"
#include "qtcontacts-extensions_impl.h"

#include <QCoreApplication>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QUuid>

// ---- for schema modification ------
#include <QtContacts/QContactFamily>
#include <QtContacts/QContactGeoLocation>
#include <QtContacts/QContactFavorite>
#include <QtContacts/QContactAddress>
#include <QtContacts/QContactEmailAddress>
#include <QtContacts/QContactPhoneNumber>
#include <QtContacts/QContactRingtone>
#include <QtContacts/QContactPresence>
#include <QtContacts/QContactGlobalPresence>
#include <QtContacts/QContactName>
// -----------------------------------

#include <QtDebug>

class Job
{
public:
    Job()
    {
    }

    virtual ~Job()
    {
    }

    virtual QContactAbstractRequest *request() = 0;
    virtual void clear() = 0;

    virtual void execute(const ContactsEngine &engine, QSqlDatabase &database, ContactReader *reader, ContactWriter *&writer) = 0;
    virtual void update(QMutex *) {}
    virtual void updateState(QContactAbstractRequest::State state) = 0;
    virtual void setError(QContactManager::Error) {}

    virtual void contactsAvailable(const QList<QContact> &) {}
    virtual void contactIdsAvailable(const QList<QContactIdType> &) {}

    virtual QString description() const = 0;
    virtual QContactManager::Error error() const = 0;
};

template <typename T>
class TemplateJob : public Job
{
public:
    TemplateJob(T *request)
        : m_request(request)
        , m_error(QContactManager::NoError)
    {
    }

    QContactAbstractRequest *request()
    {
        return m_request;
    }

    void clear()
    {
        m_request = 0;
    }

    QContactManager::Error error() const
    {
        return m_error;
    }

    void setError(QContactManager::Error error)
    {
        m_error = error;
    }

protected:
    T *m_request;
    QContactManager::Error m_error;
};

class ContactSaveJob : public TemplateJob<QContactSaveRequest>
{
public:
    ContactSaveJob(QContactSaveRequest *request)
        : TemplateJob(request)
        , m_contacts(request->contacts())
#ifdef USING_QTPIM
        , m_definitionMask(request->typeMask())
#else
        , m_definitionMask(request->definitionMask())
#endif
    {
    }

    void execute(const ContactsEngine &engine, QSqlDatabase &database, ContactReader *reader, ContactWriter *&writer)
    {
        if (!writer)
            writer = new ContactWriter(engine, database, reader);
        m_error = writer->save(&m_contacts, m_definitionMask, 0, &m_errorMap, false, false);
    }

    void updateState(QContactAbstractRequest::State state)
    {
         QContactManagerEngine::updateContactSaveRequest(
                     m_request, m_contacts, m_error, m_errorMap, state);
    }

    QString description() const
    {
        QString s(QLatin1String("Save"));
        foreach (const QContact &c, m_contacts) {
            s.append(' ').append(ContactId::toString(c));
        }
        return s;
    }

private:
    QList<QContact> m_contacts;
    ContactWriter::DetailList m_definitionMask;
    QMap<int, QContactManager::Error> m_errorMap;
};

class ContactRemoveJob : public TemplateJob<QContactRemoveRequest>
{
public:
    ContactRemoveJob(QContactRemoveRequest *request)
        : TemplateJob(request)
        , m_contactIds(request->contactIds())
    {
    }

    void execute(const ContactsEngine &engine, QSqlDatabase &database, ContactReader *reader, ContactWriter *&writer)
    {
        if (!writer)
            writer = new ContactWriter(engine, database, reader);
        m_errorMap.clear();
        m_error = writer->remove(m_contactIds, &m_errorMap, false);
    }

    void updateState(QContactAbstractRequest::State state)
    {
        QContactManagerEngine::updateContactRemoveRequest(
                m_request,
                m_error,
                m_errorMap,
                state);
    }

    QString description() const
    {
        QString s(QLatin1String("Remove"));
        foreach (const QContactIdType &id, m_contactIds) {
            s.append(' ').append(ContactId::toString(id));
        }
        return s;
    }

private:
    QList<QContactIdType> m_contactIds;
    QMap<int, QContactManager::Error> m_errorMap;
};

class ContactFetchJob : public TemplateJob<QContactFetchRequest>
{
public:
    ContactFetchJob(QContactFetchRequest *request)
        : TemplateJob(request)
        , m_filter(request->filter())
        , m_fetchHint(request->fetchHint())
        , m_sorting(request->sorting())
    {
    }

    void execute(const ContactsEngine &, QSqlDatabase &, ContactReader *reader, ContactWriter *&)
    {
        QList<QContact> contacts;
        m_error = reader->readContacts(
                QLatin1String("AsynchronousFilter"),
                &contacts,
                m_filter,
                m_sorting,
                m_fetchHint);
    }

    void update(QMutex *mutex)
    {
        QList<QContact> contacts;      {
            QMutexLocker locker(mutex);
            contacts = m_contacts;
        }
        QContactManagerEngine::updateContactFetchRequest(
                m_request,
                contacts,
                QContactManager::NoError,
                QContactAbstractRequest::ActiveState);
    }

    void updateState(QContactAbstractRequest::State state)
    {
        QContactManagerEngine::updateContactFetchRequest(m_request, m_contacts, m_error, state);
    }

    void contactsAvailable(const QList<QContact> &contacts)
    {
        m_contacts = contacts;
    }

    QString description() const
    {
        QString s(QLatin1String("Fetch"));
        return s;
    }

private:
    QContactFilter m_filter;
    QContactFetchHint m_fetchHint;
    QList<QContactSortOrder> m_sorting;
    QList<QContact> m_contacts;
};

#ifdef USING_QTPIM
class IdFetchJob : public TemplateJob<QContactIdFetchRequest>
#else
class IdFetchJob : public TemplateJob<QContactLocalIdFetchRequest>
#endif
{
public:
#ifdef USING_QTPIM
    IdFetchJob(QContactIdFetchRequest *request)
#else
    IdFetchJob(QContactLocalIdFetchRequest *request)
#endif
        : TemplateJob(request)
        , m_filter(request->filter())
        , m_sorting(request->sorting())
    {
    }

    void execute(const ContactsEngine &, QSqlDatabase &, ContactReader *reader, ContactWriter *&)
    {
        QList<QContactIdType> contactIds;
        m_error = reader->readContactIds(&contactIds, m_filter, m_sorting);
    }

    void update(QMutex *mutex)
    {
        QList<QContactIdType> contactIds;
        {
            QMutexLocker locker(mutex);
            contactIds = m_contactIds;
        }
#ifdef USING_QTPIM
        QContactManagerEngine::updateContactIdFetchRequest(
#else
        QContactManagerEngine::updateContactLocalIdFetchRequest(
#endif
                m_request,
                contactIds,
                QContactManager::NoError,
                QContactAbstractRequest::ActiveState);
    }

    void updateState(QContactAbstractRequest::State state)
    {
#ifdef USING_QTPIM
        QContactManagerEngine::updateContactIdFetchRequest(
#else
        QContactManagerEngine::updateContactLocalIdFetchRequest(
#endif
                m_request, m_contactIds, m_error, state);
    }

    void contactIdsAvailable(const QList<QContactIdType> &contactIds)
    {
        m_contactIds = contactIds;
    }

    QString description() const
    {
        QString s(QLatin1String("Fetch IDs"));
        return s;
    }

private:
    QContactFilter m_filter;
    QList<QContactSortOrder> m_sorting;
    QList<QContactIdType> m_contactIds;
};

class ContactFetchByIdJob : public TemplateJob<QContactFetchByIdRequest>
{
public:
    ContactFetchByIdJob(QContactFetchByIdRequest *request)
        : TemplateJob(request)
#ifdef USING_QTPIM
        , m_contactIds(request->contactIds())
#else
        , m_contactIds(request->localIds())
#endif
        , m_fetchHint(request->fetchHint())
    {
    }

    void execute(const ContactsEngine &, QSqlDatabase &, ContactReader *reader, ContactWriter *&)
    {
        QList<QContact> contacts;
        m_error = reader->readContacts(
                QLatin1String("AsynchronousIds"),
                &contacts,
                m_contactIds,
                m_fetchHint);
    }

    void update(QMutex *mutex)
    {
        QList<QContact> contacts;
        {
            QMutexLocker locker(mutex);
            contacts = m_contacts;
        }
#ifdef USING_QTPIM
        QContactManagerEngine::updateContactFetchByIdRequest(
#else
        QContactManagerEngineV2::updateContactFetchByIdRequest(
#endif
                m_request,
                contacts,
                QContactManager::NoError,
                QMap<int, QContactManager::Error>(),
                QContactAbstractRequest::ActiveState);
    }

    void updateState(QContactAbstractRequest::State state)
    {
#ifdef USING_QTPIM
        QContactManagerEngine::updateContactFetchByIdRequest(
#else
        QContactManagerEngineV2::updateContactFetchByIdRequest(
#endif
                m_request,
                m_contacts,
                m_error,
                QMap<int, QContactManager::Error>(),
                state);
    }

    void contactsAvailable(const QList<QContact> &contacts)
    {
        m_contacts = contacts;
    }

    QString description() const
    {
        QString s(QLatin1String("FetchByID"));
        foreach (const QContactIdType &id, m_contactIds) {
            s.append(' ').append(ContactId::toString(id));
        }
        return s;
    }

private:
    QList<QContactIdType> m_contactIds;
    QContactFetchHint m_fetchHint;
    QList<QContact> m_contacts;
};


class RelationshipSaveJob : public TemplateJob<QContactRelationshipSaveRequest>
{
public:
    RelationshipSaveJob(QContactRelationshipSaveRequest *request)
        : TemplateJob(request)
        , m_relationships(request->relationships())
    {
    }

    void execute(const ContactsEngine &engine, QSqlDatabase &database, ContactReader *reader, ContactWriter *&writer)
    {
        if (!writer)
            writer = new ContactWriter(engine, database, reader);
        m_error = writer->save(m_relationships, &m_errorMap, false);
    }

    void updateState(QContactAbstractRequest::State state)
    {
         QContactManagerEngine::updateRelationshipSaveRequest(
                     m_request, m_relationships, m_error, m_errorMap, state);
    }

    QString description() const
    {
        QString s(QLatin1String("Relationship Save"));
        return s;
    }

private:
    QList<QContactRelationship> m_relationships;
    QMap<int, QContactManager::Error> m_errorMap;
};

class RelationshipRemoveJob : public TemplateJob<QContactRelationshipRemoveRequest>
{
public:
    RelationshipRemoveJob(QContactRelationshipRemoveRequest *request)
        : TemplateJob(request)
        , m_relationships(request->relationships())
    {
    }

    void execute(const ContactsEngine &engine, QSqlDatabase &database, ContactReader *reader, ContactWriter *&writer)
    {
        if (!writer)
            writer = new ContactWriter(engine, database, reader);
        m_error = writer->remove(m_relationships, &m_errorMap, false);
    }

    void updateState(QContactAbstractRequest::State state)
    {
        QContactManagerEngine::updateRelationshipRemoveRequest(
                m_request, m_error, m_errorMap, state);
    }

    QString description() const
    {
        QString s(QLatin1String("Relationship Remove"));
        return s;
    }

private:
    QList<QContactRelationship> m_relationships;
    QMap<int, QContactManager::Error> m_errorMap;
};

class RelationshipFetchJob : public TemplateJob<QContactRelationshipFetchRequest>
{
public:
    RelationshipFetchJob(QContactRelationshipFetchRequest *request)
        : TemplateJob(request)
        , m_type(request->relationshipType())
#ifdef USING_QTPIM
        , m_first(request->first().id())
        , m_second(request->second().id())
#else
        , m_first(request->first())
        , m_second(request->second())
#endif
    {
    }

    void execute(const ContactsEngine &, QSqlDatabase &, ContactReader *reader, ContactWriter *&)
    {
        m_error = reader->readRelationships(
                &m_relationships,
                m_type,
                m_first,
                m_second);
    }

    void updateState(QContactAbstractRequest::State state)
    {
        QContactManagerEngine::updateRelationshipFetchRequest(
                m_request, m_relationships, m_error, state);
    }

    QString description() const
    {
        QString s(QLatin1String("Relationship Fetch"));
        return s;
    }

private:
    QString m_type;
    QContactId m_first;
    QContactId m_second;
    QList<QContactRelationship> m_relationships;
};

class JobThread : public QThread
{
public:
    JobThread(ContactsEngine *engine, const QString &databaseUuid)
        : m_currentJob(0)
        , m_engine(engine)
        , m_updatePending(false)
        , m_running(true)
        , m_databaseUuid(databaseUuid)
    {
        start(QThread::IdlePriority);
    }

    ~JobThread()
    {
        {
            QMutexLocker locker(&m_mutex);
            m_running = false;
            m_wait.wakeOne();
        }
        wait();
    }

    void run();

    void enqueue(Job *job)
    {
        QMutexLocker locker(&m_mutex);
        m_pendingJobs.append(job);
        m_wait.wakeOne();
    }

    bool requestDestroyed(QContactAbstractRequest *request)
    {
        QMutexLocker locker(&m_mutex);
        for (QList<Job*>::iterator it = m_pendingJobs.begin(); it != m_pendingJobs.end(); it++) {
            if ((*it)->request() == request) {
                delete *it;
                m_pendingJobs.erase(it);
                return true;
            }
        }

        if (m_currentJob && m_currentJob->request() == request) {
            m_currentJob->clear();
            return false;
        }

        for (QList<Job*>::iterator it = m_finishedJobs.begin(); it != m_finishedJobs.end(); it++) {
            if ((*it)->request() == request) {
                delete *it;
                m_finishedJobs.erase(it);
                return false;
            }
        }

        for (QList<Job*>::iterator it = m_cancelledJobs.begin(); it != m_cancelledJobs.end(); it++) {
            if ((*it)->request() == request) {
                delete *it;
                m_cancelledJobs.erase(it);
                return false;
            }
        }
        return false;
    }

    bool cancelRequest(QContactAbstractRequest *request)
    {
        QMutexLocker locker(&m_mutex);
        for (QList<Job*>::iterator it = m_pendingJobs.begin(); it != m_pendingJobs.end(); it++) {
            if ((*it)->request() == request) {
                m_cancelledJobs.append(*it);
                m_pendingJobs.erase(it);
                return true;
            }
        }
        return false;
    }

    bool waitForFinished(QContactAbstractRequest *request, const int msecs)
    {
        long timeout = msecs <= 0
                ? LONG_MAX
                : msecs;

        Job *finishedJob = 0;
        {
            QMutexLocker locker(&m_mutex);
            for (;;) {
                bool pendingJob = false;
                if (m_currentJob && m_currentJob->request() == request) {
                    qDebug() << "Wait for current job" << timeout;
                    // wait for the current job to updateState.
                    if (!m_finishedWait.wait(&m_mutex, timeout))
                        return false;
                } else for (int i = 0; i < m_pendingJobs.size(); i++) {
                    Job *job = m_pendingJobs[i];
                    if (job->request() == request) {
                        // If the job is pending, move it to the front of the queue and wait for
                        // the current job to end.
                        QElapsedTimer timer;
                        timer.start();
                        m_pendingJobs.move(i, 0);
                        if (!m_finishedWait.wait(&m_mutex, timeout))
                            return false;
                        timeout -= timer.elapsed();
                        if (timeout <= 0)
                            return false;
                        pendingJob = true;

                        break;
                    }
                }
                // Job is either finished, cancelled, or there is no job.
                if (!pendingJob)
                    break;
            }

            for (QList<Job*>::iterator it = m_finishedJobs.begin(); it != m_finishedJobs.end(); it++) {
                if ((*it)->request() == request) {
                    finishedJob = *it;
                    m_finishedJobs.erase(it);
                    break;
                }
            }
        }
        if (finishedJob) {
            finishedJob->updateState(QContactAbstractRequest::FinishedState);
            delete finishedJob;
            return true;
        } else for (QList<Job*>::iterator it = m_cancelledJobs.begin(); it != m_cancelledJobs.end(); it++) {
            if ((*it)->request() == request) {
                (*it)->updateState(QContactAbstractRequest::CanceledState);
                delete *it;
                m_cancelledJobs.erase(it);
                return true;
            }
        }
        return false;
    }

    void postUpdate()
    {
        if (!m_updatePending) {
            m_updatePending = true;
            QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        }
    }

    void contactsAvailable(const QList<QContact> &contacts)
    {
        QMutexLocker locker(&m_mutex);
        m_currentJob->contactsAvailable(contacts);
        postUpdate();
    }

    void contactIdsAvailable(const QList<QContactIdType> &contactIds)
    {
        QMutexLocker locker(&m_mutex);
        m_currentJob->contactIdsAvailable(contactIds);
        postUpdate();
    }

    bool event(QEvent *event)
    {
        if (event->type() == QEvent::UpdateRequest) {
            QList<Job*> finishedJobs;
            QList<Job*> cancelledJobs;
            Job *currentJob;
            {
                QMutexLocker locker(&m_mutex);
                finishedJobs = m_finishedJobs;
                cancelledJobs = m_cancelledJobs;
                m_finishedJobs.clear();
                m_cancelledJobs.clear();

                currentJob = m_currentJob;
                m_updatePending = false;
            }

            while (!finishedJobs.isEmpty()) {
                Job *job = finishedJobs.takeFirst();
                job->updateState(QContactAbstractRequest::FinishedState);
                delete job;
            }

            while (!cancelledJobs.isEmpty()) {
                Job *job = cancelledJobs.takeFirst();
                job->updateState(QContactAbstractRequest::CanceledState);
                delete job;
            }

            if (currentJob)
                currentJob->update(&m_mutex);
            return true;
        } else {
            return QThread::event(event);
        }
    }

private:
    QMutex m_mutex;
    QWaitCondition m_wait;
    QWaitCondition m_finishedWait;
    QList<Job*> m_pendingJobs;
    QList<Job*> m_finishedJobs;
    QList<Job*> m_cancelledJobs;
    Job *m_currentJob;
    ContactsEngine *m_engine;
    bool m_updatePending;
    bool m_running;
    QString m_databaseUuid;
};

class JobContactReader : public ContactReader
{
public:
    JobContactReader(const QSqlDatabase &database, JobThread *thread)
        : ContactReader(database)
        , m_thread(thread)
    {
    }

    void contactsAvailable(const QList<QContact> &contacts)
    {
        m_thread->contactsAvailable(contacts);
    }

    void contactIdsAvailable(const QList<QContactIdType> &contactIds)
    {
        m_thread->contactIdsAvailable(contactIds);
    }

private:
    JobThread *m_thread;
};

void JobThread::run()
{
    QSqlDatabase database = ContactsDatabase::open(QString(QLatin1String("qtcontacts-sqlite-job-%1")).arg(m_databaseUuid));
    if (!database.isOpen()) {
        while (m_running) {
            if (m_pendingJobs.isEmpty()) {
                m_wait.wait(&m_mutex);
            } else {
                m_currentJob = m_pendingJobs.takeFirst();
                m_currentJob->setError(QContactManager::UnspecifiedError);
                m_finishedJobs.append(m_currentJob);
                m_currentJob = 0;
                postUpdate();
                m_finishedWait.wakeOne();
            }
        }

        return;
    }

    JobContactReader reader(database, this);
    ContactWriter *writer = 0;

    QMutexLocker locker(&m_mutex);

    while (m_running) {
        if (m_pendingJobs.isEmpty()) {
            m_wait.wait(&m_mutex);
        } else {
            m_currentJob = m_pendingJobs.takeFirst();
            locker.unlock();
            QElapsedTimer timer;
            timer.start();
            m_currentJob->execute(*m_engine, database, &reader, writer);
            qDebug() << "Job executed in" << timer.elapsed() << ":" << m_currentJob->description() << ":" << m_currentJob->error();
            locker.relock();
            m_finishedJobs.append(m_currentJob);
            m_currentJob = 0;
            postUpdate();
            m_finishedWait.wakeOne();
        }
    }
}

ContactsEngine::ContactsEngine(const QString &name)
    : m_name(name)
    , m_synchronousReader(0)
    , m_synchronousWriter(0)
    , m_jobThread(0)
{
#ifdef USING_QTPIM
    static bool registered = qRegisterMetaType<QList<int> >("QList<int>");
    Q_UNUSED(registered)
#endif
}

ContactsEngine::~ContactsEngine()
{
    delete m_synchronousWriter;
    delete m_synchronousReader;
    delete m_jobThread;
}

QString ContactsEngine::databaseUuid()
{
    if (m_databaseUuid.isEmpty()) {
        m_databaseUuid = QUuid::createUuid().toString();
    }

    return m_databaseUuid;
}

QContactManager::Error ContactsEngine::open()
{
    m_database = ContactsDatabase::open(QString(QLatin1String("qtcontacts-sqlite-%1")).arg(databaseUuid()));
    if (m_database.isOpen()) {
        ContactNotifier::initialize();
        ContactNotifier::connect("contactsAdded", "au", this, SLOT(_q_contactsAdded(QVector<quint32>)));
        ContactNotifier::connect("contactsChanged", "au", this, SLOT(_q_contactsChanged(QVector<quint32>)));
        ContactNotifier::connect("contactsRemoved", "au", this, SLOT(_q_contactsRemoved(QVector<quint32>)));
        ContactNotifier::connect("selfContactIdChanged", "uu", this, SLOT(_q_selfContactIdChanged(quint32,quint32)));
        ContactNotifier::connect("relationshipsAdded", "au", this, SLOT(_q_relationshipsAdded(QVector<quint32>)));
        ContactNotifier::connect("relationshipsRemoved", "au", this, SLOT(_q_relationshipsRemoved(QVector<quint32>)));
        return QContactManager::NoError;
    } else {
        qWarning() << "Unable to open database";
        return QContactManager::UnspecifiedError;
    }
}

QString ContactsEngine::managerName() const
{
    return m_name;
}

int ContactsEngine::managerVersion() const
{
    return 1;
}

QList<QContactIdType> ContactsEngine::contactIds(
            const QContactFilter &filter,
            const QList<QContactSortOrder> &sortOrders,
            QContactManager::Error* error) const
{
    if (!m_synchronousReader)
        m_synchronousReader = new ContactReader(m_database);

    QList<QContactIdType> contactIds;

    QContactManager::Error err = m_synchronousReader->readContactIds(&contactIds, filter, sortOrders);
    if (error)
        *error = err;
    return contactIds;
}

QList<QContact> ContactsEngine::contacts(
            const QContactFilter &filter,
            const QList<QContactSortOrder> &sortOrders,
            const QContactFetchHint &fetchHint,
            QContactManager::Error* error) const
{
    if (!m_synchronousReader)
        m_synchronousReader = new ContactReader(m_database);

    QList<QContact> contacts;

    QContactManager::Error err = m_synchronousReader->readContacts(
                QLatin1String("SynchronousFilter"),
                &contacts,
                filter,
                sortOrders,
                fetchHint);
    if (error)
        *error = err;
    return contacts;
}

QList<QContact> ContactsEngine::contacts(
        const QContactFilter &filter,
        const QList<QContactSortOrder> &sortOrders,
        const QContactFetchHint &fetchHint,
        QMap<int, QContactManager::Error> *errorMap,
        QContactManager::Error *error) const
{
    Q_UNUSED(errorMap);

    return contacts(filter, sortOrders, fetchHint, error);
}

QList<QContact> ContactsEngine::contacts(
            const QList<QContactIdType> &localIds,
            const QContactFetchHint &fetchHint,
            QMap<int, QContactManager::Error> *errorMap,
            QContactManager::Error *error) const
{
    Q_UNUSED(errorMap);
    if (!m_synchronousReader)
        m_synchronousReader = new ContactReader(m_database);

    QList<QContact> contacts;

    QContactManager::Error err = m_synchronousReader->readContacts(
                QLatin1String("SynchronousIds"),
                &contacts,
                localIds,
                fetchHint);
    if (error)
        *error = err;
    return contacts;
}

QContact ContactsEngine::contact(
        const QContactIdType &contactId,
        const QContactFetchHint &fetchHint,
        QContactManager::Error* error) const
{
    QMap<int, QContactManager::Error> errorMap;

    QList<QContact> contacts = ContactsEngine::contacts(
                QList<QContactIdType>() << contactId, fetchHint, &errorMap, error);
    return !contacts.isEmpty()
            ? contacts.first()
            : QContact();
}

bool ContactsEngine::saveContacts(
            QList<QContact> *contacts,
            QMap<int, QContactManager::Error> *errorMap,
            QContactManager::Error *error)
{
    return saveContacts(contacts, ContactWriter::DetailList(), errorMap, error);
}

bool ContactsEngine::saveContacts(
            QList<QContact> *contacts,
            const ContactWriter::DetailList &definitionMask,
            QMap<int, QContactManager::Error> *errorMap,
            QContactManager::Error *error)
{
    if (!m_synchronousWriter) {
        if (!m_synchronousReader) {
            m_synchronousReader = new ContactReader(m_database);
        }
        m_synchronousWriter = new ContactWriter(*this, m_database, m_synchronousReader);
    }

    QContactManager::Error err = m_synchronousWriter->save(contacts, definitionMask, 0, errorMap, false, false);

    if (error)
        *error = err;
    return err == QContactManager::NoError;
}

bool ContactsEngine::removeContact(const QContactIdType &contactId, QContactManager::Error* error)
{
    QMap<int, QContactManager::Error> errorMap;

    return removeContacts(QList<QContactIdType>() << contactId, &errorMap, error);
}

bool ContactsEngine::removeContacts(
            const QList<QContactIdType> &contactIds,
            QMap<int, QContactManager::Error> *errorMap,
            QContactManager::Error* error)
{
    if (!m_synchronousWriter) {
        if (!m_synchronousReader) {
            m_synchronousReader = new ContactReader(m_database);
        }
        m_synchronousWriter = new ContactWriter(*this, m_database, m_synchronousReader);
    }

    QContactManager::Error err = m_synchronousWriter->remove(contactIds, errorMap, false);
    if (error)
        *error = err;
    return err == QContactManager::NoError;
}

QContactIdType ContactsEngine::selfContactId(QContactManager::Error* error) const
{
    if (!m_synchronousReader)
        m_synchronousReader = new ContactReader(m_database);

    QContactIdType contactId;
    QContactManager::Error err = m_synchronousReader->getIdentity(
            ContactsDatabase::SelfContactId, &contactId);
    if (error)
        *error = err;
    return contactId;
}

bool ContactsEngine::setSelfContactId(
        const QContactIdType&, QContactManager::Error* error)
{
    *error = QContactManager::NotSupportedError;
    return false;
}

QList<QContactRelationship> ContactsEngine::relationships(
        const QString &relationshipType,
#ifdef USING_QTPIM
        const QContact &participant,
#else
        const QContactId &participantId,
#endif
        QContactRelationship::Role role,
        QContactManager::Error *error) const
{
    if (!m_synchronousReader)
        m_synchronousReader = new ContactReader(m_database);

#ifdef USING_QTPIM
    QContactId first = ContactId::apiId(participant);
#else
    QContactId first = participantId;
#endif
    QContactId second;

    if (role == QContactRelationship::Second)
        qSwap(first, second);

    QList<QContactRelationship> relationships;
    QContactManager::Error err = m_synchronousReader->readRelationships(
                &relationships, relationshipType, first, second);
    if (error)
        *error = err;
    return relationships;
}

bool ContactsEngine::saveRelationships(
        QList<QContactRelationship> *relationships,
        QMap<int, QContactManager::Error> *errorMap,
        QContactManager::Error *error)
{
    if (!m_synchronousWriter) {
        if (!m_synchronousReader) {
            m_synchronousReader = new ContactReader(m_database);
        }
        m_synchronousWriter = new ContactWriter(*this, m_database, m_synchronousReader);
    }

    QContactManager::Error err = m_synchronousWriter->save(*relationships, errorMap, false);
    if (error)
        *error = err;

    if (err == QContactManager::NoError) {
#ifndef USING_QTPIM
        // update id of relationships to include the manager uri where applicable.
        for (int i = 0; relationships && i < relationships->size(); ++i) {
            QContactRelationship curr = relationships->at(i);
            if (curr.first().managerUri().isEmpty()) {
                QContactId firstId = curr.first();
                firstId.setManagerUri(QLatin1String("org.nemomobile.contacts.sqlite"));
                curr.setFirst(firstId);
            }
            if (curr.second().managerUri().isEmpty()) {
                QContactId secondId = curr.second();
                secondId.setManagerUri(QLatin1String("org.nemomobile.contacts.sqlite"));
                curr.setSecond(secondId);
            }
            relationships->replace(i, curr);
        }
#endif

        return true;
    }

    return false;
}

bool ContactsEngine::removeRelationships(
        const QList<QContactRelationship> &relationships,
        QMap<int, QContactManager::Error> *errorMap,
        QContactManager::Error *error)
{
    if (!m_synchronousWriter) {
        if (!m_synchronousReader) {
            m_synchronousReader = new ContactReader(m_database);
        }
        m_synchronousWriter = new ContactWriter(*this, m_database, m_synchronousReader);
    }

    QContactManager::Error err = m_synchronousWriter->remove(relationships, errorMap, false);
    if (error)
        *error = err;
    return err == QContactManager::NoError;
}

void ContactsEngine::requestDestroyed(QContactAbstractRequest* req)
{
    if (m_jobThread)
        m_jobThread->requestDestroyed(req);
}

bool ContactsEngine::startRequest(QContactAbstractRequest* request)
{
    Job *job = 0;

    switch (request->type()) {
    case QContactAbstractRequest::ContactSaveRequest:
        job = new ContactSaveJob(qobject_cast<QContactSaveRequest *>(request));
        break;
    case QContactAbstractRequest::ContactRemoveRequest:
        job = new ContactRemoveJob(qobject_cast<QContactRemoveRequest *>(request));
        break;
    case QContactAbstractRequest::ContactFetchRequest:
        job = new ContactFetchJob(qobject_cast<QContactFetchRequest *>(request));
        break;
#ifdef USING_QTPIM
    case QContactAbstractRequest::ContactIdFetchRequest:
        job = new IdFetchJob(qobject_cast<QContactIdFetchRequest *>(request));
        break;
#else
    case QContactAbstractRequest::ContactLocalIdFetchRequest:
        job = new IdFetchJob(qobject_cast<QContactLocalIdFetchRequest *>(request));
        break;
#endif
    case QContactAbstractRequest::ContactFetchByIdRequest:
        job = new ContactFetchByIdJob(qobject_cast<QContactFetchByIdRequest *>(request));
        break;
    case QContactAbstractRequest::RelationshipFetchRequest:
        job = new RelationshipFetchJob(qobject_cast<QContactRelationshipFetchRequest *>(request));
        break;
    case QContactAbstractRequest::RelationshipSaveRequest:
        job = new RelationshipSaveJob(qobject_cast<QContactRelationshipSaveRequest *>(request));
        break;
    case QContactAbstractRequest::RelationshipRemoveRequest:
        job = new RelationshipRemoveJob(qobject_cast<QContactRelationshipRemoveRequest *>(request));
        break;
    default:
        return false;
    }

    if (!m_jobThread)
        m_jobThread = new JobThread(this, databaseUuid());
    job->updateState(QContactAbstractRequest::ActiveState);
    m_jobThread->enqueue(job);

    return true;
}

bool ContactsEngine::cancelRequest(QContactAbstractRequest* req)
{
    if (m_jobThread)
        return m_jobThread->cancelRequest(req);

    return false;
}

bool ContactsEngine::waitForRequestFinished(QContactAbstractRequest* req, int msecs)
{
    if (m_jobThread)
        return m_jobThread->waitForFinished(req, msecs);
    return true;
}

#ifndef USING_QTPIM
QMap<QString, QContactDetailDefinition> ContactsEngine::detailDefinitions(
        const QString& contactType, QContactManager::Error*) const
{
    if (contactType != QContactType::TypeContact) {
        return QMap<QString, QContactDetailDefinition>();
    }

    QMap<QString, QContactDetailDefinition> retn = schemaDefinitions(2).value(QContactType::TypeContact);
    // we don't support some detail types
    retn.remove(QContactFamily::DefinitionName);
    retn.remove(QContactGeoLocation::DefinitionName);
    // we don't support contexts for detail types other than Address, EmailAddress and PhoneNumber
    QStringList keys = retn.keys();
    foreach (const QString &key, keys) {
        if (key == QContactAddress::DefinitionName
                || key == QContactEmailAddress::DefinitionName
                || key == QContactPhoneNumber::DefinitionName) {
            continue;
        }

        QContactDetailDefinition def = retn.value(key);
        def.removeField(QContactDetail::FieldContext);
        retn.insert(key, def);
    }
    // we don't support the Index field of Favorite
    {
        QContactDetailDefinition def = retn.value(QContactFavorite::DefinitionName);
        def.removeField(QContactFavorite::FieldIndex);
        retn.insert(QContactFavorite::DefinitionName, def);
    }
    // we don't support the Event field of Anniversary
    {
        QContactDetailDefinition def = retn.value(QContactAnniversary::DefinitionName);
        def.removeField(QContactAnniversary::FieldEvent);
        retn.insert(QContactAnniversary::DefinitionName, def);
    }
    // we don't support the SubTypes field of Address
    {
        QContactDetailDefinition def = retn.value(QContactAddress::DefinitionName);
        def.removeField(QContactAddress::FieldSubTypes);
        retn.insert(QContactAddress::DefinitionName, def);
    }
    // we don't support the AssistantName field of Organization
    {
        QContactDetailDefinition def = retn.value(QContactOrganization::DefinitionName);
        def.removeField(QContactOrganization::FieldAssistantName);
        retn.insert(QContactOrganization::DefinitionName, def);
    }
    // we don't support the VibrationRingtoneUrl field of Ringtone
    {
        QContactDetailDefinition def = retn.value(QContactRingtone::DefinitionName);
        def.removeField(QContactRingtone::FieldVibrationRingtoneUrl);
        retn.insert(QContactRingtone::DefinitionName, def);
    }
    // we don't support the PresenceStateImageUrl or PresenceStateText fields of Presence / Global Presence
    {
        QContactDetailDefinition def = retn.value(QContactPresence::DefinitionName);
        def.removeField(QContactPresence::FieldPresenceStateImageUrl);
        def.removeField(QContactPresence::FieldPresenceStateText);
        retn.insert(QContactPresence::DefinitionName, def);
    }
    {
        QContactDetailDefinition def = retn.value(QContactGlobalPresence::DefinitionName);
        def.removeField(QContactGlobalPresence::FieldPresenceStateImageUrl);
        def.removeField(QContactGlobalPresence::FieldPresenceStateText);
        retn.insert(QContactGlobalPresence::DefinitionName, def);
    }
    // the Name detail is unique
    {
        QContactDetailDefinition def = retn.value(QContactName::DefinitionName);
        def.setUnique(true);
        retn.insert(QContactName::DefinitionName, def);
    }
    return retn; // XXX TODO: modify the schema definitions with our additions to eg QContactOnlineAccount.
}

bool ContactsEngine::hasFeature(
        QContactManager::ManagerFeature feature, const QString &contactType) const
{
    if (contactType != QContactType::TypeContact)
        return false;

    // note that we also support SelfContact, but we don't support
    // modifying or removing the self contact, thus we report the
    // feature as unsupported.

    switch (feature) {
    case QContactManager::Relationships:
    case QContactManager::ArbitraryRelationshipTypes:
        return true;
    default:
        return false;
    }
}
#endif

#ifdef USING_QTPIM
bool ContactsEngine::isRelationshipTypeSupported(const QString &relationshipType, QContactType::TypeValues contactType) const
#else
bool ContactsEngine::isRelationshipTypeSupported(const QString &relationshipType, const QString &contactType) const
#endif
{
    Q_UNUSED(relationshipType);

    return contactType == QContactType::TypeContact;
}

#ifdef USING_QTPIM
QList<QContactType::TypeValues> ContactsEngine::supportedContactTypes() const
#else
QStringList ContactsEngine::supportedContactTypes() const
#endif
{
#ifdef USING_QTPIM
    return QList<QContactType::TypeValues>() << QContactType::TypeContact;
#else
    return QStringList() << QContactType::TypeContact;
#endif
}

void ContactsEngine::regenerateDisplayLabel(QContact &contact) const
{
    QContactManager::Error displayLabelError = QContactManager::NoError;
    QString label = synthesizedDisplayLabel(contact, &displayLabelError);
    if (displayLabelError != QContactManager::NoError) {
        qWarning() << "Unable to regenerate displayLabel for contact:" << ContactId::toString(contact);
        return;
    }

    setContactDisplayLabel(&contact, label);
}

#ifdef USING_QTPIM
bool ContactsEngine::setContactDisplayLabel(QContact *contact, const QString &label)
{
    QContactDisplayLabel detail(contact->detail<QContactDisplayLabel>());
    detail.setLabel(label);
    return contact->saveDetail(&detail);
}
#endif

QString ContactsEngine::normalizedPhoneNumber(const QString &input)
{
    // TODO: Use a configuration variable to specify max characters:
    static const int maxCharacters = 7;

    return QtContactsSqliteExtensions::minimizePhoneNumber(input, maxCharacters);
}

QString ContactsEngine::synthesizedDisplayLabel(const QContact &contact, QContactManager::Error *error) const
{
    *error = QContactManager::NoError;

    QContactName name = contact.detail<QContactName>();

    // If a custom label has been set, return that
#ifdef USING_QTPIM
    const QString customLabel = name.value<QString>(QContactName__FieldCustomLabel);
    if (!customLabel.isEmpty())
        return customLabel;
#else
    if (!name.customLabel().isEmpty())
        return name.customLabel();
#endif

    QString displayLabel;

    if (!name.firstName().isEmpty())
        displayLabel.append(name.firstName());

    if (!name.lastName().isEmpty()) {
        if (!displayLabel.isEmpty())
            displayLabel.append(" ");
        displayLabel.append(name.lastName());
    }

    if (!displayLabel.isEmpty()) {
        return displayLabel;
    }

    foreach (const QContactNickname& nickname, contact.details<QContactNickname>()) {
        if (!nickname.nickname().isEmpty()) {
            return nickname.nickname();
        }
    }

    foreach (const QContactGlobalPresence& gp, contact.details<QContactGlobalPresence>()) {
        if (!gp.nickname().isEmpty()) {
            return gp.nickname();
        }
    }

    foreach (const QContactOnlineAccount& account, contact.details<QContactOnlineAccount>()) {
        if (!account.accountUri().isEmpty()) {
            return account.accountUri();
        }
    }

    foreach (const QContactEmailAddress& email, contact.details<QContactEmailAddress>()) {
        if (!email.emailAddress().isEmpty()) {
            return email.emailAddress();
        }
    }

    foreach (const QContactPhoneNumber& phone, contact.details<QContactPhoneNumber>()) {
        if (!phone.number().isEmpty())
            return phone.number();
    }

    *error = QContactManager::UnspecifiedError;
    return QString();
}

static QList<QContactIdType> idList(const QVector<quint32> &contactIds)
{
    QList<QContactIdType> ids;
    ids.reserve(contactIds.size());
    foreach (quint32 dbId, contactIds) {
        ids.append(ContactId::apiId(dbId));
    }
    return ids;
}

void ContactsEngine::_q_contactsAdded(const QVector<quint32> &contactIds)
{
    emit contactsAdded(idList(contactIds));
}

void ContactsEngine::_q_contactsChanged(const QVector<quint32> &contactIds)
{
    emit contactsChanged(idList(contactIds));
}

void ContactsEngine::_q_contactsRemoved(const QVector<quint32> &contactIds)
{
    emit contactsRemoved(idList(contactIds));
}

void ContactsEngine::_q_selfContactIdChanged(quint32 oldId, quint32 newId)
{
    emit selfContactIdChanged(ContactId::apiId(oldId), ContactId::apiId(newId));
}

void ContactsEngine::_q_relationshipsAdded(const QVector<quint32> &contactIds)
{
    emit relationshipsAdded(idList(contactIds));
}

void ContactsEngine::_q_relationshipsRemoved(const QVector<quint32> &contactIds)
{
    emit relationshipsRemoved(idList(contactIds));
}
