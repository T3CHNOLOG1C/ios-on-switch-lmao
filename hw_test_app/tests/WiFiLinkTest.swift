import Darwin
import Foundation
import HWTestCore

/// en0 as the kernel's interface list has it (getifaddrs): up and running, its IPv4 address, the link rate the
/// Wi-Fi driver reports and its packet and error counters.
struct WiFiLinkTest: HWTest {
    let id = "net.wifi_link"
    let name = "Wi-Fi interface: link, address, rate and errors"
    let subsystem = Subsystem.connectivity

    struct Interface {
        var flags: UInt32 = 0
        var ipv4 = ""
        var data: if_data?
    }

    static func interface(_ name: String) -> Interface? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        var found: Interface?
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard String(cString: entry.pointee.ifa_name) == name, let address = entry.pointee.ifa_addr else { continue }
            var interface = found ?? Interface()
            interface.flags = entry.pointee.ifa_flags
            if address.pointee.sa_family == UInt8(AF_LINK), let data = entry.pointee.ifa_data {
                interface.data = data.load(as: if_data.self)
            } else if address.pointee.sa_family == UInt8(AF_INET) {
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                getnameinfo(address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
                interface.ipv4 = String(cString: host)
            }
            found = interface
        }
        return found
    }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let wifi = Self.interface("en0") else { return .fail("no en0 in getifaddrs") }
        var metrics = Metrics()
        let up = wifi.flags & UInt32(IFF_UP) != 0
        let running = wifi.flags & UInt32(IFF_RUNNING) != 0
        metrics.set("up", up)
        metrics.set("running", running)
        metrics.set("ipv4", wifi.ipv4.isEmpty ? "-" : wifi.ipv4)
        if let data = wifi.data {
            metrics.set("link_mbps", Double(data.ifi_baudrate) / 1e6, digits: 1)
            metrics.set("packets_in", Int(data.ifi_ipackets))
            metrics.set("packets_out", Int(data.ifi_opackets))
            metrics.set("errors_in", Int(data.ifi_ierrors))
            metrics.set("errors_out", Int(data.ifi_oerrors))
        }
        guard !wifi.ipv4.isEmpty else {
            return .skip("en0 has no IPv4 address: not associated with a network", metrics)
        }
        return .judged(
            [
                ("en0 up and running", up && running),
                ("the driver reports a link rate", (wifi.data?.ifi_baudrate ?? 0) > 0),
            ], metrics)
    }
}
