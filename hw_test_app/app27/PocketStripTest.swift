import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import SwiftUI
import UIKit

/// SwitchHWTest's own screen in small (an inset grouped List in a NavigationStack under a large title and a toolbar
/// button), scrolled so rows sit under the bar, composited by the render server (drawHierarchy(afterScreenUpdates:),
/// which draws the bar's scroll edge effect, its variable blur and pocket mask, as backboardd does). It runs with the
/// top edge effect shown and again hidden (.scrollEdgeEffectHidden). shown_rows= and hidden_rows= count the rows most
/// of whose pixels are magenta (R and B over 200, G under 80), shown_at= and hidden_at= list them ("-" for none);
/// bar= is the bar's bottom edge in pixels. pocket= is the mean difference x 255 between the two pictures from the top
/// to 40 points below the bar: 0 means the snapshot holds no edge effect, and without magenta rows the test skips.
/// Any magenta row fails it.
struct PocketStripTest: HWTest {
    let id = "display.pocket_strip"
    let name = "Navigation bar scroll edge effect: magenta rows, shown and hidden"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var reasons: [String] = []
        var pictures: [Raster] = []
        var bar = 0
        for (name, hidden) in [("shown", false), ("hidden", true)] {
            let (picture, barBottom) = await Self.snapshot(context, hidden: hidden)
            let rows = Self.magentaRows(picture)
            metrics.set(name + "_rows", rows.count)
            metrics.set(name + "_at", Self.ranges(rows))
            if !rows.isEmpty { reasons.append("\(rows.count) magenta rows with the edge effect \(name)") }
            pictures.append(picture)
            bar = barBottom
        }
        metrics.set("bar", bar)
        let scale = await MainActor.run { UIScreen.main.scale }
        let band = min(pictures[0].height, pictures[1].height, bar + Int(40 * scale))
        let pocket = Self.meanDifference(pictures[0], pictures[1], rows: band)
        metrics.set("pocket", Int((pocket * 255).rounded()))
        if !reasons.isEmpty {
            let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            for (name, picture) in zip(["shown", "hidden"], pictures) {
                try? picture.png()?.write(to: directory.appendingPathComponent("\(name).png"))
            }
            return .fail(LogLine.fitted(reasons), metrics)
        }
        if pocket * 255 < 0.5 { return .skip("the snapshots hold no scroll edge effect", metrics) }
        return .pass(metrics)
    }

    /// The composited picture and the navigation bar's bottom edge in its pixels.
    static func snapshot(_ context: TestContext, hidden: Bool) async -> (Raster, Int) {
        let host = await context.onStage(fullScreen: true) { stage -> UIHostingController<PocketList> in
            let host = UIHostingController(rootView: PocketList(hidden: hidden))
            host.view.frame = Snapshots.aligned(UIScreen.main.bounds)
            stage.addSubview(host.view)
            return host
        }
        await Snapshots.settle()
        await MainActor.run {
            guard let scroll = first(UICollectionView.self, in: host.view) ?? first(UIScrollView.self, in: host.view)
            else { return }
            scroll.contentOffset.y = 400 - scroll.adjustedContentInset.top
        }
        await Snapshots.settle()
        return await MainActor.run {
            let view = host.view!
            let bar = first(UINavigationBar.self, in: view).map { $0.convert($0.bounds, to: view).maxY } ?? 0
            return (Snapshots.composited(view), Int((bar * (view.window?.screen.scale ?? 1)).rounded()))
        }
    }

    @MainActor
    static func first<T: UIView>(_ type: T.Type, in view: UIView) -> T? {
        if let match = view as? T { return match }
        for subview in view.subviews {
            if let match = first(type, in: subview) { return match }
        }
        return nil
    }

    static func magentaRows(_ picture: Raster) -> [Int] {
        (0..<picture.height).filter { y in
            let magenta = (0..<picture.width).filter { x in
                picture[x, y, 0] > 200.0 / 255 && picture[x, y, 1] < 80.0 / 255 && picture[x, y, 2] > 200.0 / 255
            }
            return magenta.count * 2 > picture.width
        }
    }

    /// "412-413,500"; "-" for none.
    static func ranges(_ rows: [Int]) -> String {
        var runs: [(Int, Int)] = []
        for row in rows {
            if let last = runs.last, last.1 == row - 1 { runs[runs.count - 1].1 = row } else { runs.append((row, row)) }
        }
        return runs.isEmpty ? "-" : runs.map { $0.0 == $0.1 ? "\($0.0)" : "\($0.0)-\($0.1)" }.joined(separator: ",")
    }

    static func meanDifference(_ a: Raster, _ b: Raster, rows: Int) -> Float {
        let width = min(a.width, b.width)
        guard rows > 0, width > 0 else { return 0 }
        var sum: Float = 0
        for y in 0..<rows {
            for x in 0..<width {
                for c in 0..<3 { sum += abs(a[x, y, c] - b[x, y, c]) }
            }
        }
        return sum / Float(rows * width * 3)
    }
}

struct PocketList: View {
    let hidden: Bool

    var body: some View {
        NavigationStack {
            List {
                ForEach(0..<6) { section in
                    Section("Section \(section)") {
                        ForEach(0..<8) { row in
                            Label("Row \(section).\(row)  Wi-Fi  Bluetooth  General", systemImage: "circle.fill")
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollEdgeEffectHidden(hidden, for: .top)
            .navigationTitle("Hardware tests")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Run All") {} }
            }
        }
    }
}
