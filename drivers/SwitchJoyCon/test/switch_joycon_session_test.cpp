// The rail Joy-Con's HID session rule (drivers/SwitchJoyCon/SwitchJoyConSession.h, compiled as C by
// switch_joycon_session_harness.c), and the gamecontrollerd connection storm it ends, reproduced on the host.
//
// Around the header runs a model of the three parties, each step the 21H461 code path it is named after
// (GameController.framework, IOKit's IOHIDEventSystemClient, JoyConHIDServicePlugin):
//   * gamecontrollerd: its HID monitor's cache of service IDs, refreshed on a backboardd restart the way
//     __IOHIDEventSystemClientRefresh does (an ID it knew is kept with no callback, a new one gets the matching callback,
//     a vanished one is dropped with none); the manager server parking connections for IDs it has not seen
//     (_hidqueue_pushPendingDriverConnection:forRegistryID:); _GCNintendoJoyConDeviceManager's claimed, pending and
//     physical devices (acceptDriverConnection:/acceptFilterConnection: invalidate when there is no pending device,
//     claimHIDService: block .109 moves pending to physical, block .110 retires a physical device with the same
//     identifier, relinquishHIDService: on removal; the driver-connection invalidation handler only clears it);
//   * backboardd: one JoyConHIDServicePlugin per Joy-Con service, which opens the HID device, and whose filter and
//     plugin connections reconnect at once when gamecontrollerd cancels them;
//   * the kernel: SwitchJoyConDevice registry IDs, the session the device keeps (the header), and the rail's republish.
// With the session rule off a respring is the storm; with it, gamecontrollerd takes the new devices and refuses nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "switch_joycon_session_harness.h"

namespace {

constexpr int kNone = 0, kClosed = 1, kReplaced = 2;
constexpr unsigned kBudget = 4000;  // connections: far past any healthy run (four per respring)

// A map that keeps insertion order, for the model's maps whose iteration order the model depends on.
template <class K, class V> class Ordered {
public:
    V *find(const K &key)
    {
        for (auto &entry : entries)
            if (entry.first == key)
                return &entry.second;
        return nullptr;
    }

    bool contains(const K &key) const
    {
        return std::any_of(entries.begin(), entries.end(), [&](const auto &e) {
            return e.first == key;
        });
    }

    V &operator[](const K &key)
    {
        if (V *v = find(key))
            return *v;
        entries.emplace_back(key, V{});
        return entries.back().second;
    }

    std::optional<V> pop(const K &key)
    {
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->first == key) {
                V v = std::move(it->second);
                entries.erase(it);
                return v;
            }
        return std::nullopt;
    }

    std::vector<std::pair<K, V>> items() const
    {
        return entries;
    }

    bool empty() const
    {
        return entries.empty();
    }

    void clear()
    {
        entries.clear();
    }

private:
    std::vector<std::pair<K, V>> entries;
};

struct World;
struct Plugin;
struct Backboardd;

struct Connection {
    Plugin *plugin;
    std::string kind;  // "filter" or "driver"
    uint64_t client;
};

using ConnectionRef = std::shared_ptr<Connection>;  // compared by identity

// SwitchJoyConDevice: a registry ID and, with the rule on, the header's session (handleOpen/handleClose).
struct Device {
    World *world;
    char side;
    std::string serial;
    uint64_t id;
    switch_joycon_session session{};
    bool attached = true;  // until switch_joycon_hid_destroy's detachOutput

    Device(World *w, char s, std::string serialNumber);
    void open(uint64_t client, int32_t pid, const std::string &name);
    void close(uint64_t client);
};

// SwitchJoyConRail: publishes a running Joy-Con's device; sessionEnded marks the side and wakes the work loop.
struct Rail {
    World *world;
    Ordered<char, Device *> devices;
    Ordered<char, std::pair<int32_t, int32_t>> marks;  // side -> (served, opener)
    std::map<char, switch_joycon_republish_guard> guards;
    unsigned republished = 0;
    bool off = false;
    std::vector<std::string> logged;

    explicit Rail(World *w) : world(w)
    {}

    void publish(char side);
    void destroy(char side);  // dropHid
    void sessionEnded(char side, int32_t served, int32_t opener);
    void work();  // sessionAction -> republish(Side &)
};

// JoyConHIDServicePlugin with GamepadHIDServiceFilter on one service: opens the HID device in initWithService, the
// filter connects ~14 ms later and the plugin after the Joy-Con's device-info reply (~80 ms).
struct Plugin {
    Backboardd *server;
    Device *device;
    World *world;
    uint64_t client;
    std::map<std::string, ConnectionRef> connections;
    bool cancelled = false;

    Plugin(Backboardd *s, Device *d);
    void start();
    void connect(const std::string &kind);
    void interrupted(const ConnectionRef &connection);  // "interrupted.  Attempting to re-establish."
    void cancel(std::optional<double> closesAfter);     // service removed, or backboardd gone
};

// backboardd's HID event system: a service and a plugin for every Joy-Con device, live add/remove to monitors.
struct Backboardd {
    World *world;
    int32_t pid;
    bool alive = true;
    Ordered<uint64_t, Plugin *> services;

    Backboardd(World *w, int32_t p) : world(w), pid(p)
    {}

    void start();
    void addService(Device *device);
    void removeService(uint64_t ident);
    void die(bool closesLost);
};

struct PhysicalDevice {
    std::string identifier;
    ConnectionRef driver;
};

struct PendingDevice {
    std::string identifier;
    std::map<std::string, ConnectionRef> connections;
};

struct Gamecontrollerd {
    World *world;
    Backboardd *server = nullptr;
    std::set<uint64_t> cache, known, claimed;
    std::map<uint64_t, std::vector<ConnectionRef>> parked;
    std::map<uint64_t, PendingDevice> pending;
    std::map<uint64_t, PhysicalDevice> physical;

    explicit Gamecontrollerd(World *w) : world(w)
    {}

    void serverUp(Backboardd *s);         // __IOHIDEventSystemClientRefresh -> SetMatchingMultiple
    void serviceAdded(uint64_t ident);    // "HID Service Added" -> claimHIDService:
    void serviceRemoved(uint64_t ident);  // "HID Service Removed" -> relinquishHIDService:
    void relinquish(uint64_t ident);
    void incoming(const ConnectionRef &connection);  // acceptIncomingDriverConnection: block .323
    void accept(const ConnectionRef &connection);    // acceptDriverConnection:/acceptFilterConnection:forHIDService:
    void peerInvalidated(const ConnectionRef &connection);  // the invalidation handler only clears the connection
};

struct World {
    bool rule;
    double now = 0.0;

    struct Event {
        double at;
        uint64_t order;
        std::function<void()> fn;
    };

    struct Later {
        bool operator()(const Event &a, const Event &b) const
        {
            return a.at != b.at ? a.at > b.at : a.order > b.order;
        }
    };

    std::priority_queue<Event, std::vector<Event>, Later> queue;
    uint64_t order = 0;
    uint64_t nextClient = 0x1000;
    uint64_t nextId = 0x100000256;
    std::map<uint64_t, Device *> devices;  // registry IDs only grow, so ID order is insertion order
    Backboardd *backboardd = nullptr;
    unsigned connections = 0, rejected = 0;
    Rail rail{this};
    Gamecontrollerd gcd{this};
    // Everything the model creates lives as long as the world (old plugins and devices are still referenced).
    std::vector<std::unique_ptr<Device>> allDevices;
    std::vector<std::unique_ptr<Plugin>> allPlugins;
    std::vector<std::unique_ptr<Backboardd>> allServers;

    explicit World(bool r) : rule(r)
    {}

    uint64_t client()
    {
        return nextClient++;
    }

    void at(double delay, std::function<void()> fn)
    {
        queue.push({now + delay, order++, std::move(fn)});
    }

    void run(double until)
    {
        while (!queue.empty() && queue.top().at <= until && connections < kBudget) {
            Event e = queue.top();
            queue.pop();
            now = e.at;
            e.fn();
        }
        if (connections < kBudget)
            now = std::max(now, until);
    }

    void startBackboardd(int32_t pid)
    {
        allServers.push_back(std::make_unique<Backboardd>(this, pid));
        backboardd = allServers.back().get();
        backboardd->start();
    }

    void boot()
    {
        for (char side : {'L', 'R'})
            rail.publish(side);
        startBackboardd(52);
        run(2000);
    }

    void respring(int32_t pid, bool closesLost = false, double gap = 400)
    {
        backboardd->die(closesLost);
        run(now + gap);
        startBackboardd(pid);
        run(now + 5000);
    }

    // gamecontrollerd has one live controller per seated Joy-Con: the current device, driven by the plugin of the
    // backboardd that runs now.
    bool healthy()
    {
        std::set<uint64_t> current, physicalIds;
        for (const auto &[side, device] : rail.devices.items())
            current.insert(device->id);
        for (const auto &[ident, device] : gcd.physical)
            physicalIds.insert(ident);
        if (physicalIds != current || !gcd.pending.empty())
            return false;
        for (const auto &[ident, device] : gcd.physical)
            if (!device.driver || device.driver->plugin->server != backboardd)
                return false;
        return true;
    }
};

Device::Device(World *w, char s, std::string serialNumber)
    : world(w), side(s), serial(std::move(serialNumber)), id(w->nextId++)
{}

void Device::open(uint64_t client, int32_t pid, const std::string &name)
{
    if (!world->rule || !attached)
        return;
    const int event = jc_session_open(&session, (uintptr_t)client, pid, name == "backboardd");
    if (event)
        world->rail.sessionEnded(side, session.pid, pid);
}

void Device::close(uint64_t client)
{
    if (!world->rule || !attached)
        return;
    if (jc_session_close(&session, (uintptr_t)client))
        world->rail.sessionEnded(side, session.pid, 0);
}

void Rail::publish(char side)
{
    world->allDevices.push_back(
        std::make_unique<Device>(world, side, side == 'L' ? "48:f1:eb:b6:cf:96" : "48:f1:eb:b6:9e:d1"));
    Device *device = world->allDevices.back().get();
    devices[side] = device;
    world->devices[device->id] = device;
    device->open(0, 0, "kernel_task");  // IOHIDEventDummyService: a kernel client
    Backboardd *server = world->backboardd;
    if (server && server->alive)
        world->at(1, [server, device] {
            server->addService(device);
        });
}

void Rail::destroy(char side)
{
    if (std::optional<Device *> device = devices.pop(side)) {
        (*device)->attached = false;
        world->devices.erase((*device)->id);
        Backboardd *server = world->backboardd;
        const uint64_t ident = (*device)->id;
        if (server && server->alive)
            world->at(0.5, [server, ident] {
                server->removeService(ident);
            });
    }
    marks.pop(side);
}

void Rail::sessionEnded(char side, int32_t served, int32_t opener)
{
    marks[side] = {served, opener};
    world->at(0.1, [this] {
        work();
    });  // interruptOccurred on the software source
}

void Rail::work()
{
    std::vector<char> sides;
    for (const auto &[side, mark] : marks.items())
        sides.push_back(side);
    for (char side : sides) {
        const std::optional<std::pair<int32_t, int32_t>> mark = marks.pop(side);
        if (!mark)
            continue;
        const int32_t served = mark->first;
        if (off || !devices.contains(side))
            continue;
        switch_joycon_republish_guard &guard = guards[side];
        if (!jc_republish_allowed(&guard, served, (uint64_t)world->now)) {
            off = true;
            logged.push_back("backboardd[" + std::to_string(served) + "] ended a second HID session on Joy-Con (" +
                             std::string(1, side) + ")");
            continue;
        }
        destroy(side);
        publish(side);
        ++republished;
    }
}

Plugin::Plugin(Backboardd *s, Device *d) : server(s), device(d), world(s->world), client(world->client())
{}

void Plugin::start()
{
    device->open(client, server->pid, "backboardd");
    world->at(14, [this] {
        connect("filter");
    });
    world->at(80, [this] {
        connect("driver");
    });
}

void Plugin::connect(const std::string &kind)
{
    if (cancelled)
        return;
    ++world->connections;
    ConnectionRef connection = std::make_shared<Connection>(Connection{this, kind, world->client()});
    connections[kind] = connection;
    Gamecontrollerd *gcd = &world->gcd;
    world->at(1, [gcd, connection] {
        gcd->incoming(connection);
    });
}

void Plugin::interrupted(const ConnectionRef &connection)
{
    auto it = connections.find(connection->kind);
    if (!cancelled && it != connections.end() && it->second == connection) {
        const std::string kind = connection->kind;
        world->at(0.5, [this, kind] {
            connect(kind);
        });
    }
}

void Plugin::cancel(std::optional<double> closesAfter)
{
    cancelled = true;
    for (const auto &[kind, connection] : connections)
        world->gcd.peerInvalidated(connection);
    if (closesAfter)
        world->at(*closesAfter, [this] {
            device->close(client);
        });
}

void Backboardd::start()
{
    std::vector<Device *> current;
    for (const auto &[id, device] : world->devices)
        current.push_back(device);
    for (Device *device : current)
        addService(device);
    world->at(70, [this] {
        world->gcd.serverUp(this);
    });  // "IOHIDEventSystemServer died. Reestablishing connection."
}

void Backboardd::addService(Device *device)
{
    if (!alive || !world->devices.count(device->id) || services.contains(device->id))
        return;
    world->allPlugins.push_back(std::make_unique<Plugin>(this, device));
    Plugin *plugin = world->allPlugins.back().get();
    services[device->id] = plugin;
    plugin->start();
    if (world->gcd.server == this) {
        const uint64_t ident = device->id;
        world->at(2, [this, ident] {
            world->gcd.serviceAdded(ident);
        });
    }
}

void Backboardd::removeService(uint64_t ident)
{
    if (std::optional<Plugin *> plugin = services.pop(ident)) {
        (*plugin)->cancel(0.2);
        if (world->gcd.server == this)
            world->at(1, [this, ident] {
                world->gcd.serviceRemoved(ident);
            });
    }
}

void Backboardd::die(bool closesLost)
{
    alive = false;
    for (const auto &[ident, plugin] : services.items())
        plugin->cancel(closesLost ? std::nullopt : std::optional<double>(80));
    services.clear();
    world->gcd.server = nullptr;
}

void Gamecontrollerd::serverUp(Backboardd *s)
{
    if (!s->alive)
        return;
    const std::set<uint64_t> old = cache;
    server = s;
    cache.clear();
    for (const auto &[ident, plugin] : s->services.items())
        cache.insert(ident);
    for (const auto &[ident, plugin] : s->services.items())
        if (!old.count(ident))
            serviceAdded(ident);  // RefreshServiceCallback: only IDs the cache did not hold
}

void Gamecontrollerd::serviceAdded(uint64_t ident)
{
    auto device = world->devices.find(ident);
    if (device == world->devices.end() || claimed.count(ident))
        return;
    cache.insert(ident);
    known.insert(ident);
    claimed.insert(ident);
    pending[ident] = PendingDevice{device->second->serial, {}};
    auto waiting = parked.find(ident);
    if (waiting == parked.end())
        return;
    const std::vector<ConnectionRef> connections = waiting->second;
    parked.erase(waiting);
    for (const ConnectionRef &connection : connections)
        accept(connection);
}

void Gamecontrollerd::serviceRemoved(uint64_t ident)
{
    cache.erase(ident);
    relinquish(ident);
}

void Gamecontrollerd::relinquish(uint64_t ident)
{
    claimed.erase(ident);
    pending.erase(ident);
    physical.erase(ident);
}

void Gamecontrollerd::incoming(const ConnectionRef &connection)
{
    const uint64_t ident = connection->plugin->device->id;
    if (!known.count(ident))
        parked[ident].push_back(connection);
    else
        accept(connection);
}

void Gamecontrollerd::accept(const ConnectionRef &connection)
{
    Plugin *plugin = connection->plugin;
    const uint64_t ident = plugin->device->id;
    auto found = claimed.count(ident) ? pending.find(ident) : pending.end();
    if (found == pending.end()) {  // objectForKey: nil -> -[connection invalidate]
        ++world->rejected;
        plugin->interrupted(connection);
        return;
    }
    found->second.connections[connection->kind] = connection;
    if (connection->kind == "driver") {  // "[JoyCon Physical Device (Pending)] Prepare Device", then block .109
        const std::string identifier = found->second.identifier;
        std::vector<uint64_t> same;
        for (const auto &[other, device] : physical)  // block .110: the same identifier
            if (device.identifier == identifier)
                same.push_back(other);
        for (uint64_t other : same)
            relinquish(other);
        physical[ident] = PhysicalDevice{identifier, connection};
        pending.erase(ident);
    }
}

void Gamecontrollerd::peerInvalidated(const ConnectionRef &connection)
{
    for (auto &[ident, device] : physical)
        if (device.driver == connection)
            device.driver = nullptr;
}

TEST(SwitchJoyConSession, Layout)
{
    // The C harness's structs are the ones the C++ driver (SwitchJoyConHID.cpp) lays out.
    EXPECT_EQ(jc_session_size(), sizeof(switch_joycon_session));
    EXPECT_EQ(jc_guard_size(), sizeof(switch_joycon_republish_guard));
}

TEST(SwitchJoyConSession, FirstBackboarddClientIsTheSession)
{
    switch_joycon_session session{};
    EXPECT_EQ(jc_session_open(&session, 0, 0, 1), kNone);      // a kernel client (IOHIDEventDummyService)
    EXPECT_EQ(jc_session_open(&session, 0x10, 27, 0), kNone);  // SpringBoard
    EXPECT_EQ(session.client, 0u);
    EXPECT_EQ(jc_session_open(&session, 0x20, 52, 1), kNone);
    EXPECT_EQ(session.client, 0x20u);
    EXPECT_EQ(session.pid, 52);
    EXPECT_EQ(jc_session_open(&session, 0x21, 52, 1), kNone);  // another client of the same backboardd
    EXPECT_EQ(jc_session_close(&session, 0x21), kNone);
    EXPECT_EQ(jc_session_close(&session, 0x10), kNone);
    EXPECT_EQ(jc_session_close(&session, 0), kNone);
    EXPECT_FALSE(session.ended);
}

TEST(SwitchJoyConSession, CloseByTheServingClientEndsItOnce)
{
    switch_joycon_session session{};
    jc_session_open(&session, 0x20, 52, 1);
    EXPECT_EQ(jc_session_close(&session, 0x20), kClosed);
    EXPECT_EQ(session.pid, 52);
    EXPECT_EQ(jc_session_close(&session, 0x20), kNone);
    EXPECT_EQ(jc_session_open(&session, 0x30, 272, 1), kNone);  // the device is about to be replaced
}

TEST(SwitchJoyConSession, ADifferentBackboarddEndsIt)
{
    switch_joycon_session session{};
    jc_session_open(&session, 0x20, 52, 1);
    EXPECT_EQ(jc_session_open(&session, 0x30, 272, 1), kReplaced);
    EXPECT_EQ(session.pid, 52);                          // the session that ended
    EXPECT_EQ(jc_session_close(&session, 0x20), kNone);  // the late close of the old one
    EXPECT_EQ(jc_session_open(&session, 0x31, 300, 1), kNone);
}

TEST(SwitchJoyConSession, Guard)
{
    const unsigned gap = jc_republish_gap_ms();
    EXPECT_EQ(gap, 10000u);
    switch_joycon_republish_guard guard{};
    auto allowed = [&](int32_t pid, uint64_t now) {
        return jc_republish_allowed(&guard, pid, now) != 0;
    };
    EXPECT_TRUE(allowed(52, 0));   // the first, even at boot time 0
    EXPECT_TRUE(allowed(272, 1));  // a new backboardd at once: always
    EXPECT_TRUE(allowed(300, 2));
    EXPECT_FALSE(allowed(300, 2 + gap - 1));  // the same backboardd again within the gap
    guard = {};
    EXPECT_TRUE(allowed(52, 1000));
    EXPECT_TRUE(allowed(52, 1000 + gap));
}

TEST(SwitchJoyConSession, StormReproducedWithoutTheRule)
{
    World world(false);
    world.boot();
    EXPECT_TRUE(world.healthy());
    EXPECT_EQ(world.connections, 4u);
    EXPECT_EQ(world.rejected, 0u);
    world.respring(272);
    // Same registry IDs: no Added, no claim, no pending device, every connection refused and redone.
    EXPECT_GE(world.connections, kBudget);
    EXPECT_GT(world.rejected, kBudget - 10);
    EXPECT_FALSE(world.healthy());
}

TEST(SwitchJoyConSession, RespringWithTheRule)
{
    World world(true);
    world.boot();
    std::map<char, uint64_t> before;
    for (const auto &[side, device] : world.rail.devices.items())
        before[side] = device->id;
    EXPECT_EQ(world.rail.republished, 0u);
    world.respring(272);
    EXPECT_EQ(world.rejected, 0u);
    EXPECT_EQ(world.connections, 8u);
    EXPECT_EQ(world.rail.republished, 2u);
    for (char side : {'L', 'R'}) {
        Device **device = world.rail.devices.find(side);
        ASSERT_NE(device, nullptr) << side;
        EXPECT_NE((*device)->id, before[side]) << side;
    }
    EXPECT_TRUE(world.healthy());
    for (const auto &[side, id] : before)
        EXPECT_EQ(world.gcd.physical.count(id), 0u) << "block .110 retired " << side;
    world.respring(300, false, 100);  // again at once, as a backboardd crash right after a respring would
    EXPECT_EQ(world.rejected, 0u);
    EXPECT_TRUE(world.healthy());
    EXPECT_FALSE(world.rail.off);
}

TEST(SwitchJoyConSession, RespringWhoseClosesAreLost)
{
    World world(true);
    world.boot();
    world.respring(272, true);
    // The new backboardd's open of the old device ends its session; the old device's plugin may be refused a few times
    // before its service is removed, never more.
    EXPECT_LE(world.rejected, 4u);
    EXPECT_LT(world.connections, 20u);
    EXPECT_EQ(world.rail.republished, 2u);
    EXPECT_TRUE(world.healthy());
}

TEST(SwitchJoyConSession, DisplayCycleIsNotASessionEnd)
{
    World world(true);
    world.boot();
    for (char side : {'L', 'R'})
        world.rail.destroy(side);  // pause() then run(): the rail's own destroy detaches first
    world.run(world.now + 100);
    for (char side : {'L', 'R'})
        world.rail.publish(side);
    world.run(world.now + 2000);
    EXPECT_EQ(world.rail.republished, 0u);
    EXPECT_EQ(world.rejected, 0u);
    EXPECT_TRUE(world.healthy());
}

TEST(SwitchJoyConSession, APluginThatDropsEveryDeviceTurnsRepublishingOff)
{
    World world(true);
    world.boot();
    Backboardd *server = world.backboardd;
    for (int round = 0; round < 3; ++round) {  // the same backboardd closing the devices it serves, again and again
        for (const auto &[ident, plugin] : server->services.items())
            world.at(0, [plugin] {
                plugin->device->close(plugin->client);
            });
        world.run(world.now + 500);
    }
    EXPECT_TRUE(world.rail.off);
    EXPECT_EQ(world.rail.republished, 2u);  // once per Joy-Con, then refused
    EXPECT_EQ(world.rail.logged.size(), 1u);
}

}  // namespace
