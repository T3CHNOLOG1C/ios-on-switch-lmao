import Foundation
import GameController
import HWTestCore

/// The game controllers connected now, without asking anyone to press anything: how many, their names and profiles,
/// and the lowest battery level among them.
struct ControllersTest: HWTest {
    let id = "input.controllers"
    let name = "Game controllers connected now"
    let subsystem = Subsystem.input

    func run(_ context: TestContext) async throws -> Outcome {
        let controllers = await MainActor.run { GCController.controllers() }
        var metrics = Metrics()
        metrics.set("connected", controllers.count)
        guard !controllers.isEmpty else { return .skip("no game controller connected", metrics) }
        metrics.set("names", controllers.map { $0.vendorName ?? "?" }.joined(separator: ","))
        metrics.set("extended", controllers.filter { $0.extendedGamepad != nil }.count)
        if let level = controllers.compactMap({ $0.battery?.batteryLevel }).min() {
            metrics.set("battery_min", Double(level), digits: 2)
        }
        let profiled = controllers.allSatisfy { $0.extendedGamepad != nil || $0.microGamepad != nil }
        return .judged([("every controller has a gamepad profile", profiled)], metrics)
    }
}
