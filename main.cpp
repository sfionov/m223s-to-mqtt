#include <memory>
#include <utility>
#include <vector>
#include <optional>
#include <functional>
#include <thread>
#include <iomanip>
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <mosquitto.h>
#include <expat.h>
#include <magic_enum.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#define LOG(f, ...) fmt::print(stderr, FMT_STRING(f "\n"), ##__VA_ARGS__)
#define FMT(f, ...) fmt::format(FMT_STRING(f), ##__VA_ARGS__)

using namespace std::literals::chrono_literals;
static constexpr char M223S_OFF_TOPIC[] = "home/m223s/off";
static constexpr char M223S_STATE_TOPIC[] = "home/m223s/state";
static constexpr char M223S_ADDR[] = "F9:DA:73:71:23:4A";
static constexpr uint8_t M223S_KEY[8] = {0xa4, 0x3b, 0x64, 0xb0, 0xa3, 0xfb, 0xae, 0xcb};
static constexpr std::string_view RX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr std::string_view TX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr int CMD_CODE_AUTH = 0xff;
static constexpr int CMD_CODE_QUERY = 0x06;
static constexpr int CMD_CODE_OFF = 0x04;
static constexpr auto DISCOVERY_MIN_INTERVAL = 60s;
static constexpr auto POLLING_INTERVAL = 7.5s;
static constexpr auto WRITE_VALUE_TIMEOUT = 10s;

template <typename T>
std::chrono::microseconds to_usecs(T t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t);
}

enum Program {
    Frying = 0,
    Cereals = 1,
    Multicooker = 2,
    Pilau = 3,
    Steam = 4,
    Baking = 5,
    Stew = 6,
    Soup = 7,
    Milk_porridge = 8,
    Yoghurt = 9,
    Express = 10,
    Warming = 11
};

enum State : int {
    Disconnected = -3,
    Connected = -2,
    Authorized = -1,
    Off = 0,
    Setting = 1,
    Delayed = 2,
    Heating = 3,
    Unknown = 4,
    On = 5,
    Keep_warm = 6
};

struct DeviceState {
    uint8_t ctr = 0;
    Program program = Frying;
    State state = Disconnected;
    int temperature = 0;
    int hours = 0;
    int minutes = 0;
    void publish();

    std::string to_json();

    void update_state(State state);

    void update_state(State state, Program program, int temperature, int hours, int minutes);
};

struct {
    sd_bus *bus = nullptr;
    mosquitto *mqtt = nullptr;
    sd_event *event = nullptr;
    std::vector<std::string> adapters;
    std::string device_path;
    std::string tx_path;
    std::string rx_path;
    sd_bus_slot *rx_slot = nullptr;
    int event_fd = -1;
    std::chrono::steady_clock::time_point last_start_discovery_time{std::chrono::seconds{0}};
    DeviceState device_state{};
} g;

sd_bus *init_sd_bus() {
    sd_bus *bus;
    int r = sd_bus_default_system(&bus);
    if (r < 0) {
        LOG("Can't open system bus");
        exit(0);
    }
    return bus;
}

std::pair<std::vector<std::string>, std::string> introspect(const std::string &dest, const std::string &path) {
    std::pair<std::vector<std::string>, std::string> ret;

    sd_bus_message *reply = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g.bus, dest.c_str(), path.c_str(), "org.freedesktop.DBus.Introspectable", "Introspect", &e, &reply, "");
    if (r < 0) {
        LOG("Can't enumerate nodes: {}", r);
        return ret;
    }

    const char *s = nullptr;
    sd_bus_message_read(reply, "s", &s);
    //LOG("{}", s);

    auto parser = XML_ParserCreate("utf-8");
    auto onStartElement = [&](const char *name, const char **attrs){
        if (!strcmp(name, "node")) {
            for (const char **it = attrs; *it; it += 2) {
                if (!strcmp(it[0], "name")) {
                    ret.first.emplace_back(it[1]);
                }
            }
        }
        if (!strcmp(name, "interface")) {
            for (const char **it = attrs; *it; it += 2) {
                if (!strcmp(it[0], "name")) {
                    if (!strncmp(it[1], dest.data(), dest.size())) {
                        ret.second = it[1];
                    }
                }
            }
        }
    };
    XML_SetUserData(parser, &onStartElement);
    XML_SetElementHandler(parser, [](void *arg, const char *name, const char **attrs){
        auto *f = (decltype(onStartElement) *)arg;
        (*f)(name, attrs);
    },nullptr);
    XML_Parse(parser, s, strlen(s), true);
    XML_ParserFree(parser);
    sd_bus_message_unref(reply);
    return ret;
}

void walk(const std::string &dest, const std::string &path, const std::function<void(const std::string &node, const std::string &interface)> &f) {
    auto info = introspect(dest, path);
    f(path, info.second);
    for (auto &node : info.first) {
        std::string leaf = path;
        leaf += "/";
        leaf += node;
        walk(dest, leaf, f);
    }
}

bool start_discovery(const std::string &adapter_name) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g.bus, "org.bluez", FMT("/org/bluez/{}", adapter_name).c_str(),
                               "org.bluez.Adapter1", "StartDiscovery", &e, &reply, "");
    if (r < 0) {
        LOG("Can't start discovery on {}: {}", adapter_name, strerror(-r));
        return false;
    }
    LOG("Started discovery on {}", adapter_name);
    sd_bus_message_unref(reply);
    return true;
}

int stop_discovery(const std::string &adapter_name) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g.bus, "org.bluez", FMT("/org/bluez/{}", adapter_name).c_str(),
                               "org.bluez.Adapter1", "StopDiscovery", &e, &reply, "");
    if (r < 0) {
        LOG("Can't stop discovery on {}: {}", adapter_name, r);
        return r;
    } else {
        LOG("Stopped discovery on {}", adapter_name);
    }
    sd_bus_message_unref(reply);
    return r;
}

bool start_discovery() {
    if (g.last_start_discovery_time + DISCOVERY_MIN_INTERVAL > std::chrono::steady_clock::now()) {
        LOG("Skipping discovery");
        return false;
    }

    g.last_start_discovery_time = std::chrono::steady_clock::now();
    bool r = false;
    for (auto &s : g.adapters) {
        if (bool rv = start_discovery(s); rv) {
            r = rv;
        }
    }
    return r;
}

int stop_discovery() {
    int r = -1;
    for (auto &s : g.adapters) {
        if (int rv = stop_discovery(s); rv > 0) {
            r = rv;
        }
    }
    return r;
}

std::string get_string_property(const std::string &node, const std::string &interface, const std::string &member) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property(g.bus, "org.bluez", node.c_str(),
                                interface.c_str(), member.c_str(), &e, &reply, "s");
    if (r < 0) {
        return "";
    }
    const char *str;
    sd_bus_message_read(reply, "s", &str);
    std::string ret_str = str;
    sd_bus_message_unref(reply);
    return ret_str;
}

bool get_boolean_property(const std::string &node, const std::string &interface, const std::string &member) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property(g.bus, "org.bluez", node.c_str(),
                                interface.c_str(), member.c_str(), &e, &reply, "b");
    if (r < 0) {
        return false;
    }
    bool ret = false;
    sd_bus_message_read(reply, "b", &ret);
    sd_bus_message_unref(reply);
    return ret;
}

std::string wait_for_device() {
    std::string ret;
    bool discovery_started = false;
    bool discovery_tried = false;

    for (int i = 0; i < 5; i++) {
        for (auto &adapter : g.adapters) {
            std::string adapter_path = FMT("/org/bluez/{}", adapter);
            auto nodes = introspect("org.bluez", adapter_path);
            for (auto &node : nodes.first) {
                std::string node_path = FMT("{}/{}", adapter_path, node);
                std::string addr = get_string_property(node_path, "org.bluez.Device1", "Address");
                if (addr == M223S_ADDR) {
                    ret = node_path;
                }
            }
        }
        if (!ret.empty()) {
            break;
        }
        if (!discovery_tried) {
            discovery_started = start_discovery();
            discovery_tried = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (discovery_started) {
        stop_discovery();
    }
    return ret;
}

void connect(const std::function<void(const std::string &path)> &f) {
    if (get_boolean_property(g.device_path, "org.bluez.Device1", "Connected")) {
        f(g.device_path);
        return;
    }
    g.device_state = DeviceState{};
    g.device_state.update_state(Disconnected);

    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    LOG("Connecting...");
    int r = sd_bus_call_method(g.bus, "org.bluez", g.device_path.c_str(),
                               "org.bluez.Device1", "Connect", &e, &reply, "");
    if (r >= 0) {
        LOG("Connected");
        g.device_state.update_state(Connected);
        sd_bus_message_unref(reply);
        f(g.device_path);
    } else {
        LOG("Can't connect");
    }
}

void disconnect() {
    {
        sd_bus_message *reply = nullptr;
        sd_bus_error e = SD_BUS_ERROR_NULL;
        LOG("Stopping notify on RX");
        int r = sd_bus_call_method(g.bus, "org.bluez", g.rx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", "StopNotify",
                                 &e, &reply, "");
        if (r >= 0) {
            LOG("Stopped notify on RX");
            sd_bus_message_unref(reply);
        } else {
            LOG("Can't stop notify on RX");
        }
    }
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    LOG("Disconnecting...");
    int r = sd_bus_call_method(g.bus, "org.bluez", g.device_path.c_str(),
                               "org.bluez.Device1", "Disconnect", &e, &reply, "");
    if (r >= 0) {
        LOG("Disconnected");
        sd_bus_message_unref(reply);
    } else {
        LOG("Can't disconnect");
    }
}

std::string friendly(std::string_view sv) {
    std::string s(sv);
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}

std::string DeviceState::to_json() {
    return fmt::format("{{ \"state\": {}, "
                       "\"program\": {}, "
                       "\"temperature\": {}, "
                       "\"hours\": {}, "
                       "\"minutes\": {}}}",
                       std::quoted(friendly(magic_enum::enum_name(state))),
                       std::quoted(friendly(magic_enum::enum_name(program))),
                       temperature,
                       hours,
                       minutes);
}

void DeviceState::update_state(State state_) {
    state = state_;
    publish();
}

void DeviceState::update_state(State state_, Program program_, int temperature_, int hours_, int minutes_) {
    state = state_;
    program = program_;
    temperature = temperature_;
    hours = hours_;
    minutes = minutes_;
    publish();
}

void DeviceState::publish() {
    int mid = -1;
    std::string state_json = to_json();
    mosquitto_publish(g.mqtt, &mid, M223S_STATE_TOPIC, state_json.size(), state_json.data(), true, false);
}

void on_new_value(const std::vector<uint8_t> &value) {
    if (value.size() < 4) {
        LOG("Value too short :(");
        return;
    }
    if (value[2] == CMD_CODE_AUTH) {
        g.device_state.update_state(value[3] ? Authorized : Connected);

    } else if (value[2] == CMD_CODE_QUERY) {
        if (value.size() < 20) {
            LOG("Value too short :(");
            return;
        }
        g.device_state.update_state((State)value[11], (Program)value[3], value[5], value[8], value[9]);
    }
}

int on_rx_message(sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
    (void)m;
    (void)userdata;
    (void)ret_error;

    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r;
    r = sd_bus_get_property(g.bus, "org.bluez", g.rx_path.c_str(),
                            "org.bluez.GattCharacteristic1", "Value", &e, &reply, "ay");
    if (r >= 0) {
        fmt::print(stderr, "New value:");
        const void *arr = nullptr;
        size_t len = 0;
        sd_bus_message_read_array(reply, 'y', &arr, &len);
        for (int i = 0; i < len; i++) {
            fmt::print(stderr, " {:02x}", ((uint8_t *)arr)[i]);
        }
        fmt::print(stderr, "\n");
        on_new_value(std::vector<uint8_t>{(const uint8_t *)arr, (const uint8_t *)arr + len});
    } else {
        LOG("Can't process new RX value: {}", strerror(-r));
    }
    return 0;
}

void initialize_paths(const std::string &path) {
    walk("org.bluez", path, [&](const std::string &node, const std::string &interface){
        std::string uuid = get_string_property(node, interface, "UUID");
        if (uuid == TX_UUID) {
            g.tx_path = node;
        } else if (uuid == RX_UUID) {
            g.rx_path = node;
        }
    });
    if (!g.rx_path.empty() && !g.rx_slot) {
        sd_bus_attach_event(g.bus, g.event, 0);
        int r = sd_bus_match_signal(g.bus, &g.rx_slot, "org.bluez", g.rx_path.c_str(),
                                    "org.freedesktop.DBus.Properties", "PropertiesChanged", on_rx_message, nullptr);
        if (r >= 0) {
            LOG("Initialized RX notify slot");
        } else {
            LOG("Failed to initialize RX notify slot");
        }
    }
}

void write_value(const std::vector<uint8_t> &value, std::function<void()> then) {
    int r;
    sd_bus_message *m;
    r = sd_bus_message_new_method_call(g.bus, &m, "org.bluez", g.tx_path.c_str(),
                                   "org.bluez.GattCharacteristic1", "WriteValue");
    if (r < 0) {
        LOG("write_value: failed to create method: {}", strerror(-r));
        return;
    }
    r = sd_bus_message_append_array(m, 'y', value.data(), value.size());
    if (r < 0) {
        LOG("write_value: failed to push method parameters - data: {}", strerror(-r));
        return;
    }
    r = sd_bus_message_append(m, "a{sv}", 1, "type", "s", "command");
    if (r < 0) {
        LOG("write_value: failed to push method parameters - options: {}", strerror(-r));
        return;
    }
    sd_bus_call_async(g.bus, nullptr, m, [](sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
        return sd_event_add_time_relative(g.event, nullptr, CLOCK_MONOTONIC, 100'000, 0, [](sd_event_source *s, uint64_t usec, void *userdata){
            auto *f = (std::function<void()> *)userdata;
            (*f)();
            return 0;
        }, userdata);
    }, new std::function<void()>(std::move(then)), to_usecs(WRITE_VALUE_TIMEOUT).count());
    sd_bus_message_unrefp(&m);
}

void start_notify(std::function<void()> then) {
    if (g.device_state.state >= Authorized) {
        then();
        return;
    }

    LOG("Starting notify on RX");
    sd_bus_call_method_async(g.bus, nullptr, "org.bluez", g.rx_path.c_str(),
                             "org.bluez.GattCharacteristic1", "StartNotify",
                             [](sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        LOG("Finished starting notify on RX");
        if (ret_error && ret_error->message) {
         LOG(": {}", ret_error->message);
        }
        LOG("");
        auto *f = (std::function<void()> *) userdata;
        (*f)();
        return 0;
    }, new std::function<void()>(std::move(then)), "");
}

void authorize(const std::function<void()>& then) {
    if (g.device_state.state >= Authorized) {
        then();
        return;
    }
    start_notify([=]{
        LOG("Writing authorization request...");
        std::vector<uint8_t> cmd{0x55, g.device_state.ctr++, CMD_CODE_AUTH};
        std::copy(std::begin(M223S_KEY), std::end(M223S_KEY), std::back_inserter(cmd));
        cmd.push_back(0xaa);
        write_value(cmd, [=]{
            LOG("Authorization request sent");
            then();
        });
    });
}

void query() {
    LOG("Sending query");
    write_value({0x55, g.device_state.ctr++, CMD_CODE_QUERY, 0xaa}, []{
        LOG("Sent query");
    });
}

void turnoff() {
    LOG("Sending turnoff");
    write_value({0x55, g.device_state.ctr++, CMD_CODE_OFF, 0xaa}, []{
        LOG("Sent turnoff");
    });
}

void update_m223s_state() {
    LOG("Updating M223S state");
    g.device_path = wait_for_device();
    if (!g.device_path.empty()) {
        connect([](const std::string &path){
            if (g.rx_path.empty() || g.tx_path.empty()) {
                initialize_paths(path);
            }
            if (!g.rx_path.empty() && !g.tx_path.empty()) {
                authorize([]{
                    LOG("Ready");
                    query();
                });
            } else {
                LOG("Services not discovered yet");
            }
        });
    } else {
        LOG("Device not found");
    }
}

int main() {
    g.bus = init_sd_bus();
    sd_event_new(&g.event);
    LOG("systemd sd-bus initialized");

    g.mqtt = mosquitto_new(nullptr, true, nullptr);
    LOG("mqtt initialized");

    g.event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    g.adapters = introspect("org.bluez", "/org/bluez").first;
    LOG("Found {} adapters", g.adapters.size());

    mosquitto_connect_callback_set(g.mqtt, [](mosquitto *, void *, int){
        int off_mid = -1;
        mosquitto_subscribe(g.mqtt, &off_mid, M223S_OFF_TOPIC, true);
    });
    mosquitto_disconnect_callback_set(g.mqtt, [](mosquitto *, void *, int){
        LOG("mqtt: disconnected");
    });
    mosquitto_message_callback_set(g.mqtt, [](mosquitto *, void *, const mosquitto_message *msg){
        LOG("mqtt: message received: {}", msg->topic);
        int64_t value = 1;
        write(g.event_fd, &value, sizeof(value));
    });
    mosquitto_log_callback_set(g.mqtt, [](mosquitto *mst, void *arg, int, const char *msg) {
        LOG("mqtt: {}", msg);
    });

    sd_event_add_time_relative(g.event, nullptr, CLOCK_MONOTONIC, 0, 0, [](sd_event_source *s, uint64_t usec, void *userdata){
        if (g.device_state.ctr * POLLING_INTERVAL > 10min) {
            disconnect();
        }
        update_m223s_state();
        sd_event_source_set_enabled(s, SD_EVENT_ON);
        sd_event_source_set_time_relative(s, to_usecs(POLLING_INTERVAL).count());
        return 0;
    }, nullptr);
    sd_event_add_io(g.event, nullptr, g.event_fd, EPOLLIN, [](sd_event_source *s, int fd, uint32_t revents, void *userdata){
        int64_t value = 0;
        read(g.event_fd, &value, sizeof(value));
        turnoff();
        return 0;
    }, nullptr);

    mosquitto_connect_async(g.mqtt, "127.0.0.1", 1883, 30);
    mosquitto_loop_start(g.mqtt);
    sd_event_loop(g.event);
    return 0;
}
