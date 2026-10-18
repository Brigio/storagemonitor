// SPDX-License-Identifier: GPL-3.0-or-later

#include "smartmonitor.h"
#include "smarteventmanager.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVariant>

namespace
{

QString locateSmartctl()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("smartctl"));

    if (!path.isEmpty())
        return path;

    const QStringList candidates = {
        QStringLiteral("/usr/bin/smartctl"),
        QStringLiteral("/usr/sbin/smartctl"),
        QStringLiteral("/bin/smartctl"),
        QStringLiteral("/sbin/smartctl")
    };

    for (const QString &candidate : candidates)
    {
        const QFileInfo info(candidate);

        if (info.exists() && info.isFile() && info.isExecutable())
        {
            return candidate;
        }
    }

    return QString();
}

QProcessEnvironment englishEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));

    return env;
}

int jsonTriStateBool(const QJsonValue &value)
{
    if (!value.isBool())
        return -1;

    return value.toBool() ? 1 : 0;
}

quint64 jsonUInt64(const QJsonValue &value, bool *ok = nullptr)
{
    bool localOk = false;
    quint64 result = 0;

    if (value.isDouble())
    {
        const double number = value.toDouble();

        if (number >= 0.0)
        {
            result = static_cast<quint64>(number);
            localOk = true;
        }
    }
    else if (value.isString())
    {
        result = value.toString().toULongLong(&localOk);
    }

    if (ok)
        *ok = localOk;

    return result;
}

quint64 ataRawAttributeValue(const QJsonArray &attributes, int attributeId)
{
    for (const QJsonValue &value : attributes)
    {
        if (!value.isObject())
            continue;

        const QJsonObject attribute = value.toObject();

        if (attribute.value(QStringLiteral("id")).toInt(-1) != attributeId)
        {
            continue;
        }

        const QJsonObject raw = attribute.value(QStringLiteral("raw")).toObject();

        bool ok = false;

        const quint64 result = jsonUInt64(raw.value(QStringLiteral("value")), &ok);

        if (ok)
            return result;
    }

    return 0;
}

} // namespace


SmartMonitor::SmartMonitor(const QSqlDatabase &database, QObject *parent) : QObject(parent)
{
    if (!database.isValid() || !database.isOpen())
    {
        qCritical() << "❌ SmartMonitor: database is not valid/open";
        return;
    }

    m_connectionName = database.connectionName();

    if (m_connectionName.isEmpty())
    {
        qCritical() << "❌ SmartMonitor: database connection name is empty";
        return;
    }

    //Creo Tabella e indici SQLite
    m_ready = ensureSchema();

    if (!m_ready)
        return;

    m_eventManager = new SmartEventManager(database, this);

    if (!m_eventManager->isReady())
    {
        qWarning() << "⚠️ SmartEventManager initialization failed";

        delete m_eventManager;
        m_eventManager = nullptr;
    }
    else
    {
        qInfo() << "✅ SmartEventManager initialized";
    }

    m_smartctlPath = locateSmartctl();

    if (m_smartctlPath.isEmpty())
    {
        qWarning() << "⚠️ smartctl not found; SMART monitoring disabled";
    }
    else
    {
        qInfo() << "✅ smartctl found:" << m_smartctlPath;
    }

    m_timer = new QTimer(this);

    connect(m_timer, &QTimer::timeout, this, &SmartMonitor::scanNow);
}


bool SmartMonitor::isReady() const
{
    return m_ready;
}


bool SmartMonitor::isAvailable() const
{
    return !m_smartctlPath.isEmpty();
}


void SmartMonitor::start(int intervalMs)
{
    if (!m_ready || !isAvailable())
        return;

    if (intervalMs < 60 * 1000)
        intervalMs = 60 * 1000;

    m_timer->start(intervalMs);

    QTimer::singleShot(0, this, &SmartMonitor::scanNow);
}


void SmartMonitor::stop()
{
    if (m_timer)
        m_timer->stop();
}


bool SmartMonitor::ensureSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        return false;
    }

    QSqlQuery query(db);

    const QString createTable = QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS smart_devices (
            id INTEGER PRIMARY KEY AUTOINCREMENT,

            device_path TEXT NOT NULL UNIQUE,
            device_type TEXT,
            protocol TEXT,

            model_name TEXT,
            serial_number TEXT,
            firmware_version TEXT,

            capacity_bytes INTEGER NOT NULL DEFAULT 0,

            present INTEGER NOT NULL DEFAULT 1,
            read_ok INTEGER NOT NULL DEFAULT 0,

            smart_supported INTEGER NOT NULL DEFAULT -1,
            smart_enabled INTEGER NOT NULL DEFAULT -1,
            smart_passed INTEGER NOT NULL DEFAULT -1,

            temperature_celsius INTEGER NOT NULL DEFAULT -1,

            power_on_hours INTEGER NOT NULL DEFAULT 0,
            power_cycle_count INTEGER NOT NULL DEFAULT 0,

            reallocated_sectors INTEGER NOT NULL DEFAULT 0,
            pending_sectors INTEGER NOT NULL DEFAULT 0,
            offline_uncorrectable INTEGER NOT NULL DEFAULT 0,
            crc_errors INTEGER NOT NULL DEFAULT 0,

            nvme_critical_warning INTEGER NOT NULL DEFAULT -1,
            nvme_available_spare INTEGER NOT NULL DEFAULT -1,
            nvme_available_spare_threshold INTEGER NOT NULL DEFAULT -1,
            nvme_percentage_used INTEGER NOT NULL DEFAULT -1,

            nvme_media_errors INTEGER NOT NULL DEFAULT 0,
            nvme_unsafe_shutdowns INTEGER NOT NULL DEFAULT 0,

            smartctl_exit_status INTEGER NOT NULL DEFAULT 0,

            health_state INTEGER NOT NULL DEFAULT 0,
            health_reason TEXT,

            last_error TEXT,

            last_seen INTEGER NOT NULL,
            last_smart_read INTEGER,

            created_at INTEGER DEFAULT (strftime('%s','now')),
            updated_at INTEGER DEFAULT (strftime('%s','now')),

            CHECK(present IN (0,1)),
            CHECK(read_ok IN (0,1)),
            CHECK(smart_supported IN (-1,0,1)),
            CHECK(smart_enabled IN (-1,0,1)),
            CHECK(smart_passed IN (-1,0,1)),
            CHECK(health_state IN (0,1,2,3))
        )
    )");

    if (!query.exec(createTable))
    {
        qCritical() << "❌ Cannot create smart_devices table:" << query.lastError().text();
        return false;
    }

    const QStringList indexes = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_smart_devices_present ON smart_devices(present)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_smart_devices_health ON smart_devices(health_state)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_smart_devices_serial ON smart_devices(serial_number)")
    };

    for (const QString &sql : indexes)
    {
        if (!query.exec(sql))
        {
            qWarning() << "⚠️ Cannot create SMART index:" << query.lastError().text();
        }
    }

    return true;
}


bool SmartMonitor::discoverDevices(QList<DeviceCandidate> &devices, QString &errorMessage) const
{
    devices.clear();
    errorMessage.clear();

    if (!isAvailable())
    {
        errorMessage = QStringLiteral("smartctl is not available");
        return false;
    }

    QProcess process;
    process.setProcessEnvironment(englishEnvironment());

    process.start(m_smartctlPath, QStringList() << QStringLiteral("--scan"));

    if (!process.waitForStarted(5000))
    {
        errorMessage = QStringLiteral("cannot start smartctl --scan");
        return false;
    }

    if (!process.waitForFinished(15000))
    {
        process.kill();
        process.waitForFinished();

        errorMessage = QStringLiteral("smartctl --scan timeout");
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit)
    {
        errorMessage = QStringLiteral("smartctl --scan crashed");
        return false;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput());

    static const QRegularExpression lineRegex(QStringLiteral(R"(^\s*(\S+)(?:\s+-d\s+(\S+))?)"));

    QSet<QString> seen;

    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &line : lines)
    {
        const QRegularExpressionMatch match = lineRegex.match(line.trimmed());

        if (!match.hasMatch())
            continue;

        DeviceCandidate candidate;
        candidate.path = match.captured(1);
        candidate.type = match.captured(2);

        if (candidate.path.isEmpty())
            continue;

        const QString key = candidate.path + QLatin1Char('|') + candidate.type;

        if (seen.contains(key))
            continue;

        seen.insert(key);
        devices.append(candidate);
    }

    return true;
}


bool SmartMonitor::readDevice(const DeviceCandidate &candidate, SmartDeviceState &state) const
{
    state = SmartDeviceState();

    state.devicePath = candidate.path;
    state.deviceType = candidate.type;
    state.present = true;

    QStringList args;
    args << QStringLiteral("-j") << QStringLiteral("-a");

    if (!candidate.type.isEmpty())
    {
        args << QStringLiteral("-d") << candidate.type;
    }

    args << candidate.path;

    QProcess process;
    process.setProcessEnvironment(englishEnvironment());
    process.start(m_smartctlPath, args);

    if (!process.waitForStarted(5000))
    {
        state.lastError = QStringLiteral("cannot start smartctl");
        return false;
    }

    if (!process.waitForFinished(30000))
    {
        process.kill();
        process.waitForFinished();

        state.lastError = QStringLiteral("smartctl timeout");
        return false;
    }

    //distinguere un vero crash di smartctl da un exit status SMART non-zero:
    if (process.exitStatus() != QProcess::NormalExit)
    {
        state.lastError = QStringLiteral("smartctl process crashed");
        return false;
    }

    const QByteArray output = process.readAllStandardOutput();

    if (output.trimmed().isEmpty())
    {
        state.smartctlExitStatus = process.exitCode();
        state.lastError = QStringLiteral("smartctl returned no JSON");
        return false;
    }

    QString parseError;

    if (!parseSmartJson(output, state, parseError))
    {
        state.smartctlExitStatus = process.exitCode();
        state.lastError = parseError;
        return false;
    }

    if (state.smartctlExitStatus == 0 && process.exitCode() != 0)
    {
        state.smartctlExitStatus = process.exitCode();
    }

    state.readOk = true;
    state.lastError.clear();

    evaluateHealth(state);

    return true;
}


bool SmartMonitor::parseSmartJson(const QByteArray &jsonData, SmartDeviceState &state, QString &errorMessage) const
{
    errorMessage.clear();

    QJsonParseError parseError;

    const QJsonDocument document = QJsonDocument::fromJson(jsonData, &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("invalid smartctl JSON: %1") .arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();

    const QJsonObject smartctl = root.value(QStringLiteral("smartctl")).toObject();

    state.smartctlExitStatus = smartctl.value(QStringLiteral("exit_status")).toInt(0);

    const QJsonObject device = root.value(QStringLiteral("device")).toObject();

    if (state.devicePath.isEmpty())
        state.devicePath = device.value(QStringLiteral("name")).toString();

    if (state.deviceType.isEmpty())
        state.deviceType = device.value(QStringLiteral("type")).toString();

    state.protocol = device.value(QStringLiteral("protocol")).toString();

    state.modelName = root.value(QStringLiteral("model_name")).toString();

    if (state.modelName.isEmpty())
        state.modelName = root.value(QStringLiteral("product")).toString();

    state.serialNumber = root.value(QStringLiteral("serial_number")).toString();

    state.firmwareVersion = root.value(QStringLiteral("firmware_version")).toString();

    const QJsonObject capacity = root.value(QStringLiteral("user_capacity")).toObject();

    bool capacityOk = false;
    const quint64 capacityBytes = jsonUInt64(capacity.value(QStringLiteral("bytes")), &capacityOk);

    if (capacityOk)
        state.capacityBytes = capacityBytes;

    const QJsonObject support = root.value(QStringLiteral("smart_support")).toObject();

    if (support.contains(QStringLiteral("available")))
    {
        state.smartSupported = jsonTriStateBool(support.value(QStringLiteral("available")));
    }

    if (support.contains(QStringLiteral("enabled")))
    {
        state.smartEnabled = jsonTriStateBool(support.value(QStringLiteral("enabled")));
    }

    const QJsonObject smartStatus = root.value(QStringLiteral("smart_status")).toObject();

    if (smartStatus.contains(QStringLiteral("passed")))
    {
        state.smartPassed = jsonTriStateBool(smartStatus.value(QStringLiteral("passed")));
    }

    const QJsonObject temperature = root.value(QStringLiteral("temperature")).toObject();

    if (temperature.contains(QStringLiteral("current")))
    {
        state.temperatureCelsius = temperature.value(QStringLiteral("current")).toInt(-1);
    }

    const QJsonObject powerOnTime = root.value(QStringLiteral("power_on_time")).toObject();

    bool hoursOk = false;
    const quint64 hours = jsonUInt64(powerOnTime.value(QStringLiteral("hours")),  &hoursOk);

    if (hoursOk)
        state.powerOnHours = hours;

    bool cyclesOk = false;
    const quint64 cycles = jsonUInt64(root.value(QStringLiteral("power_cycle_count")), &cyclesOk);

    if (cyclesOk)
        state.powerCycleCount = cycles;

    const QJsonArray ataTable =
        root.value(QStringLiteral("ata_smart_attributes"))
            .toObject()
            .value(QStringLiteral("table"))
            .toArray();

    if (!ataTable.isEmpty())
    {
        state.reallocatedSectors = ataRawAttributeValue(ataTable, 5);
        state.pendingSectors = ataRawAttributeValue(ataTable, 197);
        state.offlineUncorrectable = ataRawAttributeValue(ataTable, 198);
        state.crcErrors = ataRawAttributeValue(ataTable, 199);
    }

    const QJsonObject nvme = root.value(QStringLiteral("nvme_smart_health_information_log")).toObject();

    if (!nvme.isEmpty())
    {
        if (state.smartSupported < 0)
            state.smartSupported = 1;

        if (state.smartEnabled < 0)
            state.smartEnabled = 1;

        state.nvmeCriticalWarning = nvme.value(QStringLiteral("critical_warning")).toInt(-1);

        state.nvmeAvailableSpare = nvme.value(QStringLiteral("available_spare")).toInt(-1);

        state.nvmeAvailableSpareThreshold = nvme.value(QStringLiteral("available_spare_threshold")).toInt(-1);

        state.nvmePercentageUsed = nvme.value(QStringLiteral("percentage_used")).toInt(-1);

        bool mediaOk = false;
        state.nvmeMediaErrors = jsonUInt64(nvme.value(QStringLiteral("media_errors")), &mediaOk);

        if (!mediaOk)
            state.nvmeMediaErrors = 0;

        bool unsafeOk = false;
        state.nvmeUnsafeShutdowns = jsonUInt64(nvme.value(QStringLiteral("unsafe_shutdowns")), &unsafeOk);

        if (!unsafeOk)
            state.nvmeUnsafeShutdowns = 0;
    }

    return !state.devicePath.isEmpty();
}

void SmartMonitor::evaluateHealth(SmartDeviceState &state) const
{
    // ============================================================
    // DEFAULT
    // ============================================================

    state.health = SmartHealthState::Unknown;
    state.healthReason = QStringLiteral("SMART status unavailable");

    // ============================================================
    // SMART NON SUPPORTATO
    // ============================================================

    if (state.smartSupported == 0)
    {
        state.temperatureCelsius = -1;
        state.health = SmartHealthState::Unknown;
        state.healthReason = QStringLiteral("SMART is not supported by this device");
        return;
    }

    // ============================================================
    // FASE 1
    // ERROR / CRITICAL
    // ============================================================
    /*
     * NVMe
     * controlliamolo PRIMA di smartPassed.
     * smartctl ricava infatti smart_status.passed anche dal
     * campo NVMe critical_warning
     * In questo modo manteniamo una causa più precisa.
     */
    if (state.nvmeCriticalWarning > 0)
    {
        state.health = SmartHealthState::Error;
        state.healthReason = QStringLiteral("NVMe critical warning is active (0x%1)").arg(state.nvmeCriticalWarning, 0, 16);
        return;
    }

    /*
     * Fallimento SMART complessivo dichiarato dal device.
     */
    if (state.smartPassed == 0)
    {
        state.health = SmartHealthState::Error;
        state.healthReason = QStringLiteral("SMART overall-health self-assessment failed");
        return;
    }

    /*
     * Soglia scelta dal nostro monitor.
     *
     * ATTENZIONE:
     * 65 °C NON è una soglia SMART universale.
     * È una policy del nostro storage-monitor-agent.
     * In futuro potremo usare il limite specifico
     * dichiarato dal device quando disponibile.
     * 'constexpr' può essere calcolato già durante la compilazione
     */
    constexpr int CRITICAL_TEMPERATURE_C = 65;

    if (state.temperatureCelsius >= CRITICAL_TEMPERATURE_C)
    {
        state.health = SmartHealthState::Error;
        state.healthReason = QStringLiteral("Critical temperature: %1°C").arg(state.temperatureCelsius);
        return;
    }

    // ============================================================
    // FASE 2
    // WARNING
    // ============================================================

    QStringList warnings;

    // ------------------------------------------------------------
    // ATA / SATA
    // ------------------------------------------------------------

    if (state.reallocatedSectors > 0)
    {
        warnings << QStringLiteral("%1 reallocated sectors").arg(state.reallocatedSectors);
    }

    if (state.pendingSectors > 0)
    {
        warnings << QStringLiteral("%1 current pending sectors").arg(state.pendingSectors);
    }


    if (state.offlineUncorrectable > 0)
    {
        warnings << QStringLiteral("%1 offline uncorrectable sectors").arg(state.offlineUncorrectable);
    }

    // ------------------------------------------------------------
    // NVMe
    // ------------------------------------------------------------

    if (state.nvmeMediaErrors > 0)
    {
        warnings << QStringLiteral("%1 NVMe media errors").arg(state.nvmeMediaErrors);
    }

    if (state.nvmeAvailableSpare >= 0 && state.nvmeAvailableSpareThreshold >= 0)
    {
        if (state.nvmeAvailableSpare <= state.nvmeAvailableSpareThreshold)
        {
            warnings << QStringLiteral("NVMe available spare low (%1%, threshold %2%)").arg(state.nvmeAvailableSpare, state.nvmeAvailableSpareThreshold);
        }
    }

    constexpr int NVME_PERCENTAGE_USED_WARNING = 95;

    if (state.nvmePercentageUsed >= NVME_PERCENTAGE_USED_WARNING)
    {
        warnings << QStringLiteral("NVMe SSD life expectancy low (%1% used)").arg(state.nvmePercentageUsed);
    }

    // ------------------------------------------------------------
    // EVENTUALE TEMPERATURA DI WARNING
    // ------------------------------------------------------------

    /*
     * Anche questa è una nostra policy,
     * non una soglia SMART universale.
     */
    constexpr int WARNING_TEMPERATURE_C = 55;

    if (state.temperatureCelsius >= WARNING_TEMPERATURE_C && state.temperatureCelsius <  CRITICAL_TEMPERATURE_C)
    {
        warnings << QStringLiteral("High temperature: %1°C").arg(state.temperatureCelsius);
    }


    // ============================================================
    // WARNING TROVATI
    // ============================================================

    if (!warnings.isEmpty())
    {
        state.health = SmartHealthState::Warning;
        state.healthReason = warnings.join(QStringLiteral(", "));
        return;
    }

    // ============================================================
    // FASE 3
    // OK
    // ============================================================

    /*
     * Arriviamo a OK solamente DOPO:
     *
     * - critical warning
     * - overall SMART failure
     * - temperatura critica
     * - settori riallocati
     * - settori pending
     * - settori uncorrectable
     * - media errors NVMe
     * - spare NVMe
     * - usura NVMe
     * - temperatura warning
     */
    if (state.smartPassed == 1)
    {
        state.health = SmartHealthState::Ok;
        state.healthReason = QStringLiteral("SMART overall-health self-assessment passed and monitored indicators are clean");
        return;
    }

    // ============================================================
    // SMART PRESENTE MA OVERALL STATUS NON DISPONIBILE
    // ============================================================

    state.health = SmartHealthState::Unknown;
    state.healthReason = QStringLiteral("SMART data available but overall-health status is unavailable");
}


bool SmartMonitor::markAllDevicesNotPresent()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        return false;
    }

    QSqlQuery query(db);

    if (!query.exec(QStringLiteral("UPDATE smart_devices SET present = 0, updated_at = strftime('%s','now')")))
    {
        qWarning() << "❌ Cannot mark SMART devices not present:" << query.lastError().text();
        return false;
    }

    return true;
}


bool SmartMonitor::saveDeviceState(const SmartDeviceState &state)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen() || state.devicePath.isEmpty())
    {
        return false;
    }

    QSqlQuery query(db);

    query.prepare(QStringLiteral(R"(
        INSERT INTO smart_devices (
            device_path, device_type, protocol,
            model_name, serial_number, firmware_version,
            capacity_bytes,
            present, read_ok,
            smart_supported, smart_enabled, smart_passed,
            temperature_celsius,
            power_on_hours, power_cycle_count,
            reallocated_sectors, pending_sectors,
            offline_uncorrectable, crc_errors,
            nvme_critical_warning,
            nvme_available_spare,
            nvme_available_spare_threshold,
            nvme_percentage_used,
            nvme_media_errors,
            nvme_unsafe_shutdowns,
            smartctl_exit_status,
            health_state, health_reason,
            last_error,
            last_seen, last_smart_read,
            created_at, updated_at
        )
        VALUES (
            :device_path, :device_type, :protocol,
            :model_name, :serial_number, :firmware_version,
            :capacity_bytes,
            1, :read_ok,
            :smart_supported, :smart_enabled, :smart_passed,
            :temperature_celsius,
            :power_on_hours, :power_cycle_count,
            :reallocated_sectors, :pending_sectors,
            :offline_uncorrectable, :crc_errors,
            :nvme_critical_warning,
            :nvme_available_spare,
            :nvme_available_spare_threshold,
            :nvme_percentage_used,
            :nvme_media_errors,
            :nvme_unsafe_shutdowns,
            :smartctl_exit_status,
            :health_state, :health_reason,
            :last_error,
            strftime('%s','now'),
            CASE WHEN :read_ok_time = 1
                 THEN strftime('%s','now')
                 ELSE NULL END,
            strftime('%s','now'),
            strftime('%s','now')
        )
        ON CONFLICT(device_path)
        DO UPDATE SET
            device_type = excluded.device_type,
            protocol = excluded.protocol,
            model_name = CASE WHEN excluded.read_ok = 1 THEN excluded.model_name ELSE smart_devices.model_name END,
            serial_number = CASE WHEN excluded.read_ok = 1 THEN excluded.serial_number ELSE smart_devices.serial_number END,
            firmware_version = CASE WHEN excluded.read_ok = 1 THEN excluded.firmware_version ELSE smart_devices.firmware_version END,
            capacity_bytes = CASE WHEN excluded.read_ok = 1 THEN excluded.capacity_bytes ELSE smart_devices.capacity_bytes END,
            present = 1,
            read_ok = excluded.read_ok,
            smart_supported = CASE WHEN excluded.read_ok = 1 THEN excluded.smart_supported ELSE smart_devices.smart_supported END,
            smart_enabled = CASE WHEN excluded.read_ok = 1 THEN excluded.smart_enabled ELSE smart_devices.smart_enabled END,
            smart_passed = CASE WHEN excluded.read_ok = 1 THEN excluded.smart_passed ELSE smart_devices.smart_passed END,
            temperature_celsius = CASE WHEN excluded.read_ok = 1 THEN excluded.temperature_celsius ELSE smart_devices.temperature_celsius END,
            power_on_hours = CASE WHEN excluded.read_ok = 1 THEN excluded.power_on_hours ELSE smart_devices.power_on_hours END,
            power_cycle_count = CASE WHEN excluded.read_ok = 1 THEN excluded.power_cycle_count ELSE smart_devices.power_cycle_count END,
            reallocated_sectors = CASE WHEN excluded.read_ok = 1 THEN excluded.reallocated_sectors ELSE smart_devices.reallocated_sectors END,
            pending_sectors = CASE WHEN excluded.read_ok = 1 THEN excluded.pending_sectors ELSE smart_devices.pending_sectors END,
            offline_uncorrectable = CASE WHEN excluded.read_ok = 1 THEN excluded.offline_uncorrectable ELSE smart_devices.offline_uncorrectable END,
            crc_errors = CASE WHEN excluded.read_ok = 1 THEN excluded.crc_errors ELSE smart_devices.crc_errors END,
            nvme_critical_warning = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_critical_warning ELSE smart_devices.nvme_critical_warning END,
            nvme_available_spare = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_available_spare ELSE smart_devices.nvme_available_spare END,
            nvme_available_spare_threshold = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_available_spare_threshold ELSE smart_devices.nvme_available_spare_threshold END,
            nvme_percentage_used = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_percentage_used ELSE smart_devices.nvme_percentage_used END,
            nvme_media_errors = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_media_errors ELSE smart_devices.nvme_media_errors END,
            nvme_unsafe_shutdowns = CASE WHEN excluded.read_ok = 1 THEN excluded.nvme_unsafe_shutdowns ELSE smart_devices.nvme_unsafe_shutdowns END,
            smartctl_exit_status = excluded.smartctl_exit_status,
            health_state = CASE WHEN excluded.read_ok = 1 THEN excluded.health_state ELSE smart_devices.health_state END,
            health_reason = CASE WHEN excluded.read_ok = 1 THEN excluded.health_reason ELSE smart_devices.health_reason END,
            last_error = excluded.last_error,
            last_seen = strftime('%s','now'),
            last_smart_read = CASE WHEN excluded.read_ok = 1 THEN strftime('%s','now') ELSE smart_devices.last_smart_read END,
            updated_at = strftime('%s','now')
    )"));

    query.bindValue(QStringLiteral(":device_path"), state.devicePath);
    query.bindValue(QStringLiteral(":device_type"), state.deviceType);
    query.bindValue(QStringLiteral(":protocol"), state.protocol);
    query.bindValue(QStringLiteral(":model_name"), state.modelName);
    query.bindValue(QStringLiteral(":serial_number"), state.serialNumber);
    query.bindValue(QStringLiteral(":firmware_version"), state.firmwareVersion);
    query.bindValue(QStringLiteral(":capacity_bytes"), static_cast<qulonglong>(state.capacityBytes));
    query.bindValue(QStringLiteral(":read_ok"), state.readOk ? 1 : 0);
    query.bindValue(QStringLiteral(":read_ok_time"), state.readOk ? 1 : 0);
    query.bindValue(QStringLiteral(":smart_supported"), state.smartSupported);
    query.bindValue(QStringLiteral(":smart_enabled"), state.smartEnabled);
    query.bindValue(QStringLiteral(":smart_passed"), state.smartPassed);
    query.bindValue(QStringLiteral(":temperature_celsius"), state.temperatureCelsius);
    query.bindValue(QStringLiteral(":power_on_hours"), static_cast<qulonglong>(state.powerOnHours));
    query.bindValue(QStringLiteral(":power_cycle_count"), static_cast<qulonglong>(state.powerCycleCount));
    query.bindValue(QStringLiteral(":reallocated_sectors"), static_cast<qulonglong>(state.reallocatedSectors));
    query.bindValue(QStringLiteral(":pending_sectors"), static_cast<qulonglong>(state.pendingSectors));
    query.bindValue(QStringLiteral(":offline_uncorrectable"), static_cast<qulonglong>(state.offlineUncorrectable));
    query.bindValue(QStringLiteral(":crc_errors"), static_cast<qulonglong>(state.crcErrors));
    query.bindValue(QStringLiteral(":nvme_critical_warning"), state.nvmeCriticalWarning);
    query.bindValue(QStringLiteral(":nvme_available_spare"), state.nvmeAvailableSpare);
    query.bindValue(QStringLiteral(":nvme_available_spare_threshold"), state.nvmeAvailableSpareThreshold);
    query.bindValue(QStringLiteral(":nvme_percentage_used"), state.nvmePercentageUsed);
    query.bindValue(QStringLiteral(":nvme_media_errors"), static_cast<qulonglong>(state.nvmeMediaErrors));
    query.bindValue(QStringLiteral(":nvme_unsafe_shutdowns"), static_cast<qulonglong>(state.nvmeUnsafeShutdowns));
    query.bindValue(QStringLiteral(":smartctl_exit_status"), state.smartctlExitStatus);
    query.bindValue(QStringLiteral(":health_state"), static_cast<int>(state.health));
    query.bindValue(QStringLiteral(":health_reason"), state.healthReason);
    query.bindValue(QStringLiteral(":last_error"), state.lastError);

    if (!query.exec())
    {
        qWarning() << "❌ Cannot save SMART state for" << state.devicePath << query.lastError().text();
        return false;
    }

    return true;
}

void SmartMonitor::scanNow()
{
    if (!m_ready || !isAvailable())
    {
        return;
    }

    QList<DeviceCandidate> devices;
    QString discoveryError;

    if (!discoverDevices(devices, discoveryError))
    {
        qWarning() << "❌ SMART discovery failed:" << discoveryError;
        return;
    }

    QList<SmartDeviceState> states;
    states.reserve(devices.size());

    int readableDevices = 0;

    //for (const DeviceCandidate &candidate : devices)
        // supporta C++17 o successivi
    for (const DeviceCandidate &candidate : std::as_const(devices))
    {
        SmartDeviceState state;

        if (readDevice(candidate, state))
        {
            ++readableDevices;
        }
        else
        {
            state.devicePath = candidate.path;
            state.deviceType = candidate.type;
            state.present = true;
            state.readOk = false;
            state.health = SmartHealthState::Unknown;
            state.healthReason = QStringLiteral("SMART data could not be read");
        }

        states.append(state);
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        return;
    }

    if (!db.transaction())
    {
        qWarning() << "❌ Cannot start SMART DB transaction:" << db.lastError().text();
        return;
    }

    if (!markAllDevicesNotPresent())
    {
        db.rollback();
        return;
    }

   // ciclo di persistenza: In questo modo smart_devices e smart_events partecipano alla stessa
   // transazione SQLite.
    for (const SmartDeviceState &state : states)
    {
        if (!saveDeviceState(state))
        {
            db.rollback();
            return;
        }

        if (!processSmartEvents(state))
        {
            qWarning() << "❌ Cannot persist SMART events for" << state.devicePath;
            db.rollback();
            return;
        }
    }

    if (!db.commit())
    {
        qWarning() << "❌ Cannot commit SMART DB transaction:" << db.lastError().text();
        db.rollback();
        return;
    }

    for (const SmartDeviceState &state : states)
        emit deviceUpdated(state.devicePath);

    emit scanCompleted(devices.size(), readableDevices);
}

bool SmartMonitor::acknowledgeEvent(qint64 eventId)
{
    if (!m_eventManager || !m_eventManager->isReady())
    {
        qWarning() << "Cannot acknowledge SMART event:" << "event manager unavailable";
        return false;
    }

    return m_eventManager->acknowledgeEvent(eventId);
}

bool SmartMonitor::resolveEvent(qint64 eventId)
{
    if (!m_eventManager || !m_eventManager->isReady())
    {
        qWarning() << "Cannot resolve SMART event:" << "event manager unavailable";
        return false;
    }

    return m_eventManager->resolveEvent(eventId);
}

bool SmartMonitor::clearResolvedEvents()
{
    if (!m_eventManager || !m_eventManager->isReady())
    {
        qWarning() << "Cannot clear SMART event history:" << "event manager unavailable";
        return false;
    }

    return m_eventManager->clearResolvedEvents();
}

bool SmartMonitor::processSmartEvents(const SmartDeviceState &state)
{
    if (!m_eventManager || !m_eventManager->isReady())
    {
        return true;
    }

    /*
     * Se smartctl non ha prodotto una lettura valida oppure lo
     * stato è Unknown, NON risolviamo gli eventi già attivi.
     * Non abbiamo prova che il device sia tornato sano.
     */
    if (!state.readOk || state.health == SmartHealthState::Unknown)
    {
        return true;
    }

    if (state.health == SmartHealthState::Warning)
    {
        SmartEventRecord event;

        event.devicePath = state.devicePath;
        event.serialNumber = state.serialNumber;
        event.modelName = state.modelName;
        event.eventType = QStringLiteral("smart_warning");
        event.severity = QStringLiteral("warning");
        event.title = QStringLiteral("SMART device warning");
        event.message = state.healthReason;
        event.active = true;
        event.acknowledged = false;

        if (!m_eventManager->recordOrUpdateActiveEvent(event))
            return false;

        if (!m_eventManager->resolveActiveEvent(
                state.devicePath,
                QStringLiteral("smart_critical")))
        {
            return false;
        }

        return true;
    }

    if (state.health == SmartHealthState::Error)
    {
        SmartEventRecord event;

        event.devicePath = state.devicePath;
        event.serialNumber = state.serialNumber;
        event.modelName = state.modelName;
        event.eventType = QStringLiteral("smart_critical");
        event.severity = QStringLiteral("critical");
        event.title = QStringLiteral("SMART device critical");
        event.message = state.healthReason;
        event.active = true;
        event.acknowledged = false;

        if (!m_eventManager->recordOrUpdateActiveEvent(event))
            return false;

        if (!m_eventManager->resolveActiveEvent(
                state.devicePath,
                QStringLiteral("smart_warning")))
        {
            return false;
        }

        return true;
    }

    if (state.health == SmartHealthState::Ok)
    {
        if (!m_eventManager->resolveActiveEvent(
                state.devicePath,
                QStringLiteral("smart_warning")))
        {
            return false;
        }

        if (!m_eventManager->resolveActiveEvent(
                state.devicePath,
                QStringLiteral("smart_critical")))
        {
            return false;
        }
    }

    return true;
}