#include <chrono>

#include "PoolManager.h"

using namespace std;
using namespace dev;
using namespace eth;

PoolManager* PoolManager::m_this = nullptr;

PoolManager::PoolManager(PoolClient* client, MinerType const& minerType, unsigned maxTries,
    unsigned failoverTimeout, unsigned ergodicity)
  : m_io_strand(g_io_service),
    m_failovertimer(g_io_service),
    m_submithrtimer(g_io_service),
    m_minerType(minerType)
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "PoolManager::PoolManager() begin");

    m_this = this;
    p_client = client;
    m_ergodicity = ergodicity;
    m_maxConnectionAttempts = maxTries;
    m_failoverTimeout = failoverTimeout;
    m_currentWp.header = h256();

    p_client->onConnected([&]() {
        {
            Guard l(m_activeConnectionMutex);
            m_selectedHost.append(p_client->ActiveEndPoint());
            cnote << "Established connection to " << m_selectedHost;

            // Reset current WorkPackage
            m_currentWp.job = h256();

            // Shuffle if needed
            if (m_ergodicity == 1)
                Farm::f().shuffle();

            // Rough implementation to return to primary pool
            // after specified amount of time
            if (m_activeConnectionIdx != 0 && m_failoverTimeout > 0)
            {
                m_failovertimer.expires_from_now(boost::posix_time::minutes(m_failoverTimeout));
                m_failovertimer.async_wait(m_io_strand.wrap(boost::bind(
                    &PoolManager::failovertimer_elapsed, this, boost::asio::placeholders::error)));
            }
            else
            {
                m_failovertimer.cancel();
            }
        }

        if (!Farm::f().isMining())
        {
            cnote << "Spinning up miners...";
            if (m_minerType == MinerType::CL)
                Farm::f().start("opencl", false);
            else if (m_minerType == MinerType::CUDA)
                Farm::f().start("cuda", false);
            else if (m_minerType == MinerType::Mixed)
            {
                Farm::f().start("cuda", false);
                Farm::f().start("opencl", true);
            }
        }
        else if (Farm::f().paused())
        {
            cnote << "Resume mining ...";
            Farm::f().resume();
        }

        // Activate timing for HR submission
        m_submithrtimer.expires_from_now(boost::posix_time::seconds(m_hrReportingInterval));
        m_submithrtimer.async_wait(m_io_strand.wrap(boost::bind(
            &PoolManager::submithrtimer_elapsed, this, boost::asio::placeholders::error)));
    });

    p_client->onDisconnected([&]() {
        cnote << "Disconnected from " << m_selectedHost;

        // Clear current connection
        p_client->unsetConnection();
        m_currentWp.header = h256();

        // Stop timing actors
        m_failovertimer.cancel();
        m_submithrtimer.cancel();

        if (m_stopping.load(std::memory_order_relaxed))
        {
            if (Farm::f().isMining())
            {
                cnote << "Shutting down miners...";
                Farm::f().stop();
            }
            m_running.store(false, std::memory_order_relaxed);
        }
        else
        {
            // Suspend mining and submit new connection request
            cnote << "No connection. Suspend mining ...";
            Farm::f().pause();
            g_io_service.post(m_io_strand.wrap(boost::bind(&PoolManager::rotateConnect, this)));
        }
    });

    p_client->onWorkReceived([&](WorkPackage const& wp) {
        // Should not happen !
        if (!wp)
            return;

        bool newEpoch = (wp.seed != m_currentWp.seed);
        bool newDiff = (wp.boundary != m_currentWp.boundary);
        m_currentWp = wp;

        if (newEpoch)
        {
            m_epochChanges.fetch_add(1, std::memory_order_relaxed);
            if (m_currentWp.block > 0)
                m_currentWp.epoch = m_currentWp.block / 30000;
            else
                m_currentWp.epoch =
                    ethash::find_epoch_number(ethash::hash256_from_bytes(m_currentWp.seed.data()));
            showEpoch();
        }
        if (newDiff)
            showDifficulty();

        cnote << "Job: " EthWhite "#" << m_currentWp.header.abridged()
              << (m_currentWp.block != -1 ? (" block " + to_string(m_currentWp.block)) : "")
              << EthReset << " " << m_selectedHost;

        // Shuffle if needed
        if (m_ergodicity == 2 && m_currentWp.exSizeBytes == 0)
            Farm::f().shuffle();

        Farm::f().setWork(m_currentWp);
    });

    p_client->onSolutionAccepted([&](bool const& stale, std::chrono::milliseconds const& elapsedMs,
                                     unsigned const& miner_index) {
        std::stringstream ss;
        ss << std::setw(4) << std::setfill(' ') << elapsedMs.count() << " ms."
           << " " << m_selectedHost;
        cnote << EthLime "**Accepted" EthReset << (stale ? EthYellow "(stale)" EthReset : "")
              << ss.str();
        Farm::f().acceptedSolution(stale, miner_index);
    });

    p_client->onSolutionRejected([&](bool const& stale, std::chrono::milliseconds const& elapsedMs,
                                     unsigned const& miner_index) {
        std::stringstream ss;
        ss << std::setw(4) << std::setfill(' ') << elapsedMs.count() << "ms."
           << "   " << m_selectedHost;
        cwarn << EthRed "**Rejected" EthReset << (stale ? EthYellow "(stale)" EthReset : "")
              << ss.str();
        Farm::f().rejectedSolution(miner_index);
    });

    Farm::f().onSolutionFound([&](const Solution& sol) {
        // Solution should passthrough only if client is
        // properly connected. Otherwise we'll have the bad behavior
        // to log nonce submission but receive no response

        if (p_client->isConnected())
        {
            p_client->submitSolution(sol);
        }
        else
        {
            cnote << string(EthRed "Solution 0x") + toHex(sol.nonce)
                  << " wasted. Waiting for connection...";
        }

        return false;
    });

    Farm::f().onMinerRestart([&]() {
        dev::setThreadName("main");
        cnote << "Restart miners...";

        if (Farm::f().isMining())
        {
            cnote << "Shutting down miners...";
            Farm::f().stop();
        }

        cnote << "Spinning up miners...";
        if (m_minerType == MinerType::CL)
            Farm::f().start("opencl", false);
        else if (m_minerType == MinerType::CUDA)
            Farm::f().start("cuda", false);
        else if (m_minerType == MinerType::Mixed)
        {
            Farm::f().start("cuda", false);
            Farm::f().start("opencl", true);
        }
    });

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "PoolManager::PoolManager() end");
}

void PoolManager::stop()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "PoolManager::stop() begin");
    if (m_running.load(std::memory_order_relaxed))
    {
        m_stopping.store(true, std::memory_order_relaxed);

        if (p_client->isConnected())
        {
            p_client->disconnect();
            // Wait for async operations to complete
            while (m_running.load(std::memory_order_relaxed))
                this_thread::sleep_for(chrono::milliseconds(500));
        }
        else
        {
            // Stop timing actors
            m_failovertimer.cancel();
            m_submithrtimer.cancel();

            if (Farm::f().isMining())
            {
                cnote << "Shutting down miners...";
                Farm::f().stop();
            }
        }
    }
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "PoolManager::stop() end");
}

void PoolManager::addConnection(URI& conn)
{
    Guard l(m_activeConnectionMutex);
    m_connections.push_back(conn);
}

/*
 * Remove a connection
 * Returns:  0 on success
 *          -1 failure (out of bounds)
 *          -2 failure (active connection should be deleted)
 */
int PoolManager::removeConnection(unsigned int idx)
{
    Guard l(m_activeConnectionMutex);
    if (idx >= m_connections.size())
        return -1;
    if (idx == m_activeConnectionIdx)
        return -2;
    m_connections.erase(m_connections.begin() + idx);
    if (m_activeConnectionIdx > idx)
    {
        m_activeConnectionIdx--;
    }
    return 0;
}

void PoolManager::clearConnections()
{
    {
        Guard l(m_activeConnectionMutex);
        m_connections.clear();
    }
    if (p_client && p_client->isConnected())
        p_client->disconnect();
}

/*
 * Sets the active connection
 * Returns: 0 on success, -1 on failure (out of bounds)
 */
int PoolManager::setActiveConnection(unsigned int idx)
{
    // Sets the active connection to the requested index
    UniqueGuard l(m_activeConnectionMutex);
    if (idx >= m_connections.size())
        return -1;
    if (idx == m_activeConnectionIdx)
        return 0;

    m_connectionSwitches.fetch_add(1, std::memory_order_relaxed);
    m_activeConnectionIdx = idx;
    m_connectionAttempt = 0;
    l.unlock();
    p_client->disconnect();

    // Suspend mining if applicable as we're switching
    cnote << "No connection. Suspend mining ...";
    Farm::f().pause();
    return 0;
}

URI PoolManager::getActiveConnectionCopy()
{
    Guard l(m_activeConnectionMutex);
    if (m_connections.size() > m_activeConnectionIdx)
        return m_connections[m_activeConnectionIdx];
    return URI(":0");
}

Json::Value PoolManager::getConnectionsJson()
{
    // Returns the list of configured connections
    Json::Value jRes;
    Guard l(m_activeConnectionMutex);

    for (size_t i = 0; i < m_connections.size(); i++)
    {
        Json::Value JConn;
        JConn["index"] = (unsigned)i;
        JConn["active"] = (i == m_activeConnectionIdx ? true : false);
        JConn["uri"] = m_connections[i].String();
        jRes.append(JConn);
    }

    return jRes;
}

void PoolManager::start()
{
    Guard l(m_activeConnectionMutex);
    m_running.store(true, std::memory_order_relaxed);
    m_connectionSwitches.fetch_add(1, std::memory_order_relaxed);
    g_io_service.post(m_io_strand.wrap(boost::bind(&PoolManager::rotateConnect, this)));
}

void PoolManager::rotateConnect()
{
    if (p_client->isConnected())
        return;

    UniqueGuard l(m_activeConnectionMutex);

    // Check we're within bounds
    if (m_activeConnectionIdx >= m_connections.size())
        m_activeConnectionIdx = 0;

    // If this connection is marked Unrecoverable then discard it
    if (m_connections.at(m_activeConnectionIdx).IsUnrecoverable())
    {
        m_connections.erase(m_connections.begin() + m_activeConnectionIdx);
        m_connectionAttempt = 0;
        if (m_activeConnectionIdx >= m_connections.size())
            m_activeConnectionIdx = 0;
        m_connectionSwitches.fetch_add(1, std::memory_order_relaxed);
    }
    else if (m_connectionAttempt >= m_maxConnectionAttempts)
    {
        // If this is the only connection we can't rotate
        // forever
        if (m_connections.size() == 1)
        {
            m_connections.erase(m_connections.begin() + m_activeConnectionIdx);
        }
        // Rotate connections if above max attempts threshold
        else
        {
            m_connectionAttempt = 0;
            m_activeConnectionIdx++;
            if (m_activeConnectionIdx >= m_connections.size())
                m_activeConnectionIdx = 0;
            m_connectionSwitches.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!m_connections.empty() && m_connections.at(m_activeConnectionIdx).Host() != "exit")
    {
        // Count connectionAttempts
        m_connectionAttempt++;

        // Invoke connections
        m_selectedHost = m_connections.at(m_activeConnectionIdx).Host() + ":" +
                         to_string(m_connections.at(m_activeConnectionIdx).Port());
        p_client->setConnection(&m_connections.at(m_activeConnectionIdx));
        cnote << "Selected pool " << m_selectedHost;

        l.unlock();
        p_client->connect();
    }
    else
    {
        l.unlock();

        if (m_connections.empty())
        {
            cnote << "No more connections to try. Exiting...";
        }
        else
        {
            cnote << "'exit' failover just got hit. Exiting...";
        }

        // Stop mining if applicable
        if (Farm::f().isMining())
        {
            cnote << "Shutting down miners...";
            Farm::f().stop();
        }

        m_running.store(false, std::memory_order_relaxed);
        raise(SIGTERM);
    }
}

void PoolManager::showEpoch()
{
    if (m_currentWp)
        cnote << "Epoch : " EthWhite << m_currentWp.epoch << EthReset;
}

void PoolManager::showDifficulty()
{
    std::stringstream ss;
    ss << fixed << setprecision(2) << getCurrentDifficulty() / 1000000000.0 << "K megahash";
    cnote << "Difficulty : " EthWhite << ss.str() << EthReset;
}

void PoolManager::failovertimer_elapsed(const boost::system::error_code& ec)
{
    if (!ec)
    {
        if (m_running.load(std::memory_order_relaxed))
        {
            UniqueGuard l(m_activeConnectionMutex);
            if (m_activeConnectionIdx != 0)
            {
                m_activeConnectionIdx = 0;
                m_connectionAttempt = 0;
                m_connectionSwitches.fetch_add(1, std::memory_order_relaxed);
                l.unlock();
                cnote << "Failover timeout reached, retrying connection to primary pool";
                p_client->disconnect();
            }
        }
    }
}

void PoolManager::submithrtimer_elapsed(const boost::system::error_code& ec)
{
    if (!ec)
    {
        if (m_running.load(std::memory_order_relaxed) && p_client->isConnected())
        {
            auto mp = Farm::f().miningProgress();
            std::string h = toHex(toCompactBigEndian(uint64_t(mp.hashRate), 1));
            std::string res = h[0] != '0' ? h : h.substr(1);

            // Should be 32 bytes
            // https://github.com/ethereum/wiki/wiki/JSON-RPC#eth_submithashrate
            std::ostringstream ss;
            ss << std::setw(64) << std::setfill('0') << res;
            p_client->submitHashrate("0x" + ss.str());

            // Resubmit actor
            m_submithrtimer.expires_from_now(boost::posix_time::seconds(m_hrReportingInterval));
            m_submithrtimer.async_wait(m_io_strand.wrap(boost::bind(
                &PoolManager::submithrtimer_elapsed, this, boost::asio::placeholders::error)));
        }
    }
}


unsigned int dev::eth::PoolManager::getCurrentEpoch()
{
    if (!m_currentWp)
        return 0;
    return m_currentWp.epoch;
}

double PoolManager::getCurrentDifficulty()
{
    if (!m_currentWp)
        return 0.0;

    using namespace boost::multiprecision;
    static const uint256_t dividend(
        "0xffff000000000000000000000000000000000000000000000000000000000000");
    const uint256_t divisor(string("0x") + m_currentWp.boundary.hex());
    std::stringstream ss;
    return double(dividend / divisor);
}

unsigned PoolManager::getConnectionSwitches()
{
    return m_connectionSwitches.load(std::memory_order_relaxed);
}

unsigned PoolManager::getEpochChanges()
{
    return m_epochChanges.load(std::memory_order_relaxed);
}
