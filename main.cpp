#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <iomanip>

#include <systemd/sd-bus.h>
#include <mosquitto.h>
#include <expat.h>
#include "magic_enum.hpp"

struct {
    sd_bus *bus = nullptr;
    mosquitto *mqtt = nullptr;
    std::vector<std::string> adapters;
    std::optional<std::string> device_adapter;
    std::string tx_path;
    std::string rx_path;
    sd_bus_slot *rx_slot = nullptr;
    sd_event *event = nullptr;
} g;

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

enum State {
    Off = 0,
    Setting = 1,
    Delayed = 2,
    Heating = 3,
    Unknown = 4,
    On = 5,
    Keep_warm = 6
};

struct GlobalState {
    bool authorized = false;
    Program program = Frying;
    State state = Off;
    int temperature = 0;
    int hours = 0;
    int minutes = 0;
} state;

static constexpr char M223S_OFF_TOPIC[] = "home/m223s/off";
static constexpr char M223S_STATE_TOPIC[] = "home/m223s/state";
static constexpr char M223S_ADDR[] = "F9:DA:73:71:23:4A";
static std::string M223S_ADDR_NODE = "dev_F9_DA_73_71_23_4A";
static constexpr std::string_view RX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr std::string_view TX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr int CMD_CODE_AUTH = 0xff;
static constexpr int CMD_CODE_QUERY = 0x06;
static constexpr int CMD_CODE_OFF = 0x06;
static uint8_t ctr;


sd_bus *init_sd_bus() {
    sd_bus *bus;
    int r = sd_bus_default_system(&bus);
    if (r < 0) {
        fprintf(stderr, "Can't open system bus\n");
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
        fprintf(stderr, "Can't enumerate nodes: %d\n", r);
        return ret;
    }

    const char *s = nullptr;
    sd_bus_message_read(reply, "s", &s);
    //std::clog << s << "\n";

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

int start_discovery(const std::string &adapter_name) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g.bus, "org.bluez", ("/org/bluez/" + adapter_name).c_str(),
                               "org.bluez.Adapter1", "StartDiscovery", &e, &reply, "");
    if (r < 0) {
        fprintf(stderr, "Can't start discovery on %s: %d\n", adapter_name.c_str(), r);
        return r;
    }
    fprintf(stderr, "Started discovery on %s\n", adapter_name.c_str());
    sd_bus_message_unref(reply);
    return r;
}

int stop_discovery(const std::string &adapter_name) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g.bus, "org.bluez", ("/org/bluez/" + adapter_name).c_str(),
                               "org.bluez.Adapter1", "StopDiscovery", &e, &reply, "");
    if (r < 0) {
        fprintf(stderr, "Can't stop discovery on %s: %d\n", adapter_name.c_str(), r);
        return r;
    } else {
        fprintf(stderr, "Stopped discovery on %s\n", adapter_name.c_str());
    }
    sd_bus_message_unref(reply);
    return r;
}

int start_discovery() {
    int r = -1;
    for (auto &s : g.adapters) {
        if (int rv = start_discovery(s); rv > 0) {
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

std::optional<std::string> wait_for_device() {
    std::optional<std::string> ret;
    bool discovery_started = false;

    for (int i = 0; i < 5; i++) {
        for (auto &adapter : g.adapters) {
            auto nodes = introspect("org.bluez", "/org/bluez/" + adapter);
            for (auto &node : nodes.first) {
                if (node == M223S_ADDR_NODE) {
                    ret = adapter;
                }
            }
        }
        if (ret) {
            break;
        }
        if (!discovery_started) {
            start_discovery();
            discovery_started = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (discovery_started) {
        stop_discovery();
    }
    return ret;
}

std::string get_uuid(const std::string &node, const std::string &interface) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property(g.bus, "org.bluez", node.c_str(),
                                interface.c_str(), "UUID", &e, &reply, "s");
    if (r < 0) {
        return "";
    }
    const char *uuid;
    sd_bus_message_read(reply, "s", &uuid);
    std::string uuid_str = uuid;
    sd_bus_message_unref(reply);
    return uuid_str;
}

void connect(const std::function<void(const std::string &path)> &f) {
    std::string path = "/org/bluez/" + *g.device_adapter + "/" + M223S_ADDR_NODE;
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    fprintf(stderr, "Connecting...\n");
    int r = sd_bus_call_method(g.bus, "org.bluez", path.c_str(),
                               "org.bluez.Device1", "Connect", &e, &reply, "");
    if (r >= 0) {
        fprintf(stderr, "Connected\n");
        sd_bus_message_unref(reply);
        f(path);
    } else {
        fprintf(stderr, "Can't connect\n");
    }
}

std::string quoted_friendly(std::string_view str) {
    std::ostringstream os;
    os << std::quoted(str);
    std::string ret = os.str();
    for (auto &ch : ret) {
        if (ch == '_') ch = ' ';
    }
    return ret;
}

std::string state_to_json() {
    std::ostringstream os;
    os << "{ " << std::quoted("authorized") << ": " << (state.authorized ? "true" : "false") << ", "
        << std::quoted("state") << ": " << quoted_friendly(magic_enum::enum_name(state.state)) << ", "
        << std::quoted("program") << ": " << quoted_friendly(magic_enum::enum_name(state.program)) << ", "
        << std::quoted("temperature") << ": " << state.temperature << ", "
        << std::quoted("hours") << ": " << state.hours << ", "
        << std::quoted("minutes") << ": " << state.minutes << " }";
    return os.str();
}

void on_new_value(const std::vector<uint8_t> &value) {
    bool need_report = false;
    if (value.size() < 4) {
        fprintf(stderr, "Value too short :(\n");
        return;
    }
    if (value[2] == CMD_CODE_AUTH) {
        state.authorized = value[3];
    }
    if (!state.authorized) {
        need_report = true;
        state = GlobalState{};
    } else if (value[2] == CMD_CODE_QUERY) {
        if (value.size() < 20) {
            fprintf(stderr, "Value too short :(\n");
            return;
        }
        need_report = true;
        state.program = (Program)value[3];
        state.temperature = (Program)value[5];
        state.hours = value[8];
        state.minutes = value[9];
        state.state = (State)value[11];
    }

    if (need_report) {
        std::string state_json = state_to_json();
        int mid = -1;
        mosquitto_publish(g.mqtt, &mid, M223S_STATE_TOPIC, state_json.size(), state_json.data(), true, false);
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
        fprintf(stderr, "New value:");
        const void *arr = NULL;
        size_t len = 0;
        sd_bus_message_read_array(reply, 'y', &arr, &len);
        for (int i = 0; i < len; i++) {
            fprintf(stderr, " %02x", ((uint8_t *)arr)[i]);
        }
        fprintf(stderr, "\n");
        on_new_value(std::vector<uint8_t>{(const uint8_t *)arr, (const uint8_t *)arr + len});
    } else {
        fprintf(stderr, "Can't process new RX value: %s", strerror(-r));
    }
    return 0;
}

void initialize_paths(const std::string &path) {
    walk("org.bluez", path, [&](const std::string &node, const std::string &interface){
        std::string uuid = get_uuid(node, interface);
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
            fprintf(stderr, "Initialized RX notify slot\n");
        } else {
            fprintf(stderr, "Failed to initialize RX notify slot\n");
        }
    }
}

void write_value(const std::vector<uint8_t> &value, std::function<void()> then) {
    sd_bus_message *reply = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r;
    sd_bus_message *m;
    r = sd_bus_message_new_method_call(g.bus, &m, "org.bluez", g.tx_path.c_str(),
                                   "org.bluez.GattCharacteristic1", "WriteValue");
    if (r < 0) {
        fprintf(stderr, "write_value: failed to create method: %s\n", strerror(-r));
        return;
    }
    r = sd_bus_message_append_array(m, 'y', value.data(), value.size());
    if (r < 0) {
        fprintf(stderr, "write_value: failed to push method parameters - data: %s\n", strerror(-r));
        return;
    }
    r = sd_bus_message_append(m, "a{sv}", 1, "type", "s", "command");
    if (r < 0) {
        fprintf(stderr, "write_value: failed to push method parameters - options: %s\n", strerror(-r));
        return;
    }
    sd_bus_call_async(g.bus, nullptr, m, [](sd_bus_message *m, void *userdata, sd_bus_error *ret_error){
        return sd_event_add_time_relative(g.event, nullptr, CLOCK_MONOTONIC, 100'000, 0, [](sd_event_source *s, uint64_t usec, void *userdata){
            auto *f = (std::function<void()> *)userdata;
            (*f)();
            return 0;
        }, userdata);
    }, new std::function<void()>(then), 10'000'000);
}

void start_notify(std::function<void()> then) {
    fprintf(stderr, "Starting notify on RX\n");
    sd_bus_call_method_async(g.bus, nullptr, "org.bluez", g.rx_path.c_str(),
                             "org.bluez.GattCharacteristic1", "StartNotify",
                             [](sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        fprintf(stderr, "Finished starting notify on RX");
        if (ret_error && ret_error->message) {
         fprintf(stderr, ": %s", ret_error->message);
        }
        fprintf(stderr, "\n");
        auto *f = (std::function<void()> *) userdata;
        (*f)();
        return 0;
    }, new std::function<void()>(std::move(then)), "");
}

void authorize(const std::function<void()>& then) {
    start_notify([=]{
        fprintf(stderr, "Writing authorization request...\n");
        write_value({0x55, ctr++, 0xff, 0xb5, 0x4c, 0x75, 0xb1, 0xb4, 0x0c, 0x88, 0xef, 0xaa}, [=]{
            fprintf(stderr, "Authorization request sent\n");
            then();
        });
    });
}

void query() {
    fprintf(stderr, "Sending query\n");
    write_value({0x55, ctr++, 0x06, 0xaa}, []{
        fprintf(stderr, "Sent query\n");
    });
}

void turnoff() {
    fprintf(stderr, "Sending turnoff\n");
    write_value({0x55, ctr++, 0x06, 0xaa}, []{
        fprintf(stderr, "Sent turnoff\n");
    });
}

void update_m223s_state() {
    fprintf(stderr, "Updating M223S state\n");
    g.device_adapter = wait_for_device();
    if (g.device_adapter) {
        connect([](const std::string &path){
            if (g.rx_path.empty() || g.tx_path.empty()) {
                initialize_paths(path);
            }
            if (!g.rx_path.empty() && !g.tx_path.empty()) {
                authorize([]{
                    fprintf(stderr, "Ready\n");
                    query();
                });
            } else {
                fprintf(stderr, "Services not discovered yet\n");
            }
        });
    } else {
        fprintf(stderr, "Device not found\n");
    }
}

int main() {
    g.bus = init_sd_bus();
    sd_event_new(&g.event);
    fprintf(stderr, "systemd sd-bus initialized\n");

    g.mqtt = mosquitto_new(nullptr, true, nullptr);
    fprintf(stderr, "mqtt initialized\n");
    g.adapters = introspect("org.bluez", "/org/bluez").first;
    fprintf(stderr, "Found %zd adapters\n", g.adapters.size());

    mosquitto_connect_callback_set(g.mqtt, [](mosquitto *, void *, int){
        int off_mid = -1;
        mosquitto_subscribe(g.mqtt, &off_mid, M223S_OFF_TOPIC, true);
    });
    mosquitto_disconnect_callback_set(g.mqtt, [](mosquitto *, void *, int){
        fprintf(stderr, "mqtt: disconnected\n");
    });
    mosquitto_message_callback_set(g.mqtt, [](mosquitto *, void *, const mosquitto_message *msg){
        fprintf(stderr, "mqtt: message received: %s\n", msg->topic);
        turnoff();
    });
    mosquitto_log_callback_set(g.mqtt, [](mosquitto *mst, void *arg, int, const char *msg) {
        fprintf(stderr, "mqtt: %s\n", msg);
    });

    sd_event_add_time_relative(g.event, nullptr, CLOCK_MONOTONIC, 1'000'000, 0, [](sd_event_source *s, uint64_t usec, void *userdata){
        sd_event_source_set_enabled(s, SD_EVENT_ON);
        sd_event_source_set_time_relative(s, 10'000'000);
        update_m223s_state();
        return 0;
    }, nullptr);

    mosquitto_connect_async(g.mqtt, "127.0.0.1", 1883, 10);
    mosquitto_loop_start(g.mqtt);
    sd_event_loop(g.event);
    return 0;
}
