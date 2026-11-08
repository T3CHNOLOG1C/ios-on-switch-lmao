import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Core Animation's states, one per class of the raw pack's corpus (gpu/CAStateSweep.swift), each drawn by the raw
/// facade and by the m2v device it wraps, their pixels compared byte for byte: m2v is the oracle. A state whose
/// command buffer the facade sent to m2v proves nothing and is counted apart (on_m2v=). close= are 8-bit channels
/// one unit apart, wrong= anything more; the first wrong states are named and their attachments land in
/// Documents/gpu/gpu.ca.sweep/ (raw and m2v PNGs of 4-byte formats, every state's verdict in states.txt).
/// HWTEST_SWEEP_ALL=1 in the app's environment draws every corpus state instead of one per class.
public struct CAStateSweepTest: HWTest {
    public let id = "gpu.ca.sweep"
    public let name = "Raw facade against m2v: Core Animation's corpus states, pixel for pixel"
    public let subsystem = Subsystem.metal
    public var timeout: TimeInterval { 900 }

    public init() {}

    enum Verdict: String {
        case exact, close, wrong
    }

    public func run(_ context: TestContext) async throws -> Outcome {
        let raw = try GPU.make()
        guard let m2vDevice = RawFacade.m2vDevice(of: raw.device) else {
            return .skip("no m2v device behind the default one (not the raw facade, or a release build)")
        }
        let m2v = try GPU.make(device: m2vDevice)
        guard let url = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        let everyState = ProcessInfo.processInfo.environment["HWTEST_SWEEP_ALL"] == "1"
        let states = CAStateSweep.states(text, everyState: everyState)
        let libraries = (
            raw: try raw.device.makeLibrary(URL: CAState.library), m2v: try m2v.device.makeLibrary(URL: CAState.library)
        )
        let directory = context.documents.appendingPathComponent("gpu").appendingPathComponent(id)
        try? FileManager.default.removeItem(at: directory)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var counts: [String: Int] = [:]
        var wrongStates: [String] = []
        var listing = ""
        for line in states {
            let verdict: String
            do {
                let ours = try await CAStateSweep.draw(line, library: libraries.raw, gpu: raw)
                let oracle = try await CAStateSweep.draw(line, library: libraries.m2v, gpu: m2v)
                if ours.path != "raw" {
                    verdict = "on_m2v \(ours.path)"
                    counts["on_m2v", default: 0] += 1
                } else {
                    let judged = judge(line, ours.attachments, oracle.attachments)
                    verdict = judged.rawValue
                    counts[verdict, default: 0] += 1
                    if judged == .wrong {
                        wrongStates.append(line.shortLabel)
                        save(line, ours.attachments, oracle.attachments, directory)
                    }
                }
            } catch let unsupported as CAStateSweep.Unsupported {
                verdict = "skipped \(unsupported.description)"
                counts["skipped", default: 0] += 1
            } catch {
                verdict = "failed \(error)"
                counts["failed", default: 0] += 1
            }
            listing += "\(verdict) \(line.shortLabel) \(line.vertex.name) \(line.fragment.name)\n"
        }
        try? listing.write(to: directory.appendingPathComponent("states.txt"), atomically: true, encoding: .utf8)
        var metrics = Metrics()
        metrics.set("states", states.count)
        for key in ["exact", "close", "wrong", "on_m2v", "skipped", "failed"] { metrics.set(key, counts[key] ?? 0) }
        if !wrongStates.isEmpty { metrics.set("wrong_states", wrongStates.prefix(12).joined(separator: ",")) }
        let checked = (counts["exact"] ?? 0) + (counts["close"] ?? 0) + (counts["wrong"] ?? 0)
        if checked == 0 { return .fail("no state was drawn by the raw path", metrics) }
        if !wrongStates.isEmpty {
            return .fail("\(wrongStates.count) states drew other pixels than m2v's", metrics, proof: raw.proof)
        }
        return .pass(metrics, proof: raw.proof)
    }

    /// exact: every byte equal; close: only 8-bit unorm channels one unit apart; wrong: anything else.
    func judge(_ line: PipelineLine, _ ours: [[UInt8]], _ oracle: [[UInt8]]) -> Verdict {
        var verdict = Verdict.exact
        for (index, color) in line.colors.sorted(by: { $0.index < $1.index }).enumerated() {
            let (a, b) = (ours[index], oracle[index])
            if a == b { continue }
            let unorm8 = [70, 71, 80, 81, 10, 30, 1].contains(color.format)
            guard unorm8, a.count == b.count, zip(a, b).allSatisfy({ abs(Int($0) - Int($1)) <= 1 }) else {
                return .wrong
            }
            verdict = .close
        }
        return verdict
    }

    func save(_ line: PipelineLine, _ ours: [[UInt8]], _ oracle: [[UInt8]], _ directory: URL) {
        for (index, color) in line.colors.sorted(by: { $0.index < $1.index }).enumerated() {
            let stem = "\(line.shortLabel)-c\(color.index)"
            guard let format: TexelFormat = [70: .rgba8Unorm, 80: .bgra8Unorm][Int(color.format)] else {
                try? Data(ours[index]).write(to: directory.appendingPathComponent("\(stem)-raw.bin"))
                try? Data(oracle[index]).write(to: directory.appendingPathComponent("\(stem)-m2v.bin"))
                continue
            }
            let (w, h) = (CAStateSweep.width, CAStateSweep.height)
            let got = format.raster(ours[index], width: w, height: h)
            let want = format.raster(oracle[index], width: w, height: h)
            let check = SceneCheck(
                label: stem, got: got, want: want, diff: RasterDiff.compare(got, want, tolerance: 1.01 / 255))
            try? check.save(to: directory, stem: stem)
        }
    }
}
