// Launcher.swift - Dead Rising's start screen on iPhone / iPad (SwiftUI), laid out like the user's Tunic port:
// a title page on the left (Play), a case file of settings on the right. Only settings the recompilation has
// (user/config.h). It checks the game files before Play and says what is missing and where it goes. No art from the
// game: colours and type only. Created by src/os/ios_launcher.mm through DRLauncher.make(...), which gets the
// chosen settings and the game folder back in onPlay.
import SwiftUI
import UIKit

private let night = Color(red: 0.03, green: 0.03, blue: 0.035)

private struct Theme {
    // a mall security room at night, and a manila case file
    static let room = Color(red: 0.075, green: 0.078, blue: 0.086)
    static let roomLight = Color(red: 0.13, green: 0.135, blue: 0.145)
    static let paper = Color(red: 0.92, green: 0.87, blue: 0.74)
    static let ink = Color(red: 0.16, green: 0.12, blue: 0.09)
    static let blood = Color(red: 0.72, green: 0.07, blue: 0.06)
    static let bloodBright = Color(red: 0.86, green: 0.12, blue: 0.09)
    static let watch = Color(red: 0.78, green: 0.86, blue: 0.36)
    static let uiInk = UIColor(red: 0.16, green: 0.12, blue: 0.09, alpha: 1)
    static let uiBlood = UIColor(red: 0.72, green: 0.07, blue: 0.06, alpha: 1)
}

/// What the game folder holds.
private struct GameFiles {
    var folder: String = ""
    var xex = false
    var archives = 0
    var ntvX = false
    var ready: Bool { xex && archives > 0 && ntvX }

    static let expectedArchives = 1171

    static func scan(documents: String) -> GameFiles {
        let fm = FileManager.default
        let inGame = documents + "/game"
        // the same choice the runtime makes (main.cpp): Documents/game if default.xex is there, else Documents
        let folder = fm.fileExists(atPath: inGame + "/default.xex") || !fm.fileExists(atPath: documents + "/default.xex") ? inGame : documents
        var files = GameFiles()
        files.folder = folder
        files.xex = fm.fileExists(atPath: folder + "/default.xex")
        let names = (try? fm.contentsOfDirectory(atPath: folder)) ?? []
        files.archives = names.filter { $0.hasPrefix("arcrom") && $0.hasSuffix(".arc") }.count
        var isDirectory: ObjCBool = false
        files.ntvX = fm.fileExists(atPath: folder + "/ntvX", isDirectory: &isDirectory) && isDirectory.boolValue
        return files
    }
}

private final class LauncherModel: ObservableObject {
    let documents: String
    @Published var files: GameFiles
    @Published var frameRate: Int
    @Published var resolutionScale: Double
    @Published var msaa: Bool
    @Published var fxaa: Bool
    @Published var motionBlur: Bool
    @Published var vsync: Bool
    @Published var showFps: Bool
    @Published var screenScaling: Int
    @Published var starting = false
    let onPlay: (NSDictionary) -> Void

    init(settings: NSDictionary, documents: String, onPlay: @escaping (NSDictionary) -> Void) {
        self.documents = documents
        self.files = GameFiles.scan(documents: documents)
        self.frameRate = (settings["frameRate"] as? NSNumber)?.intValue ?? 30
        self.resolutionScale = min((settings["resolutionScale"] as? NSNumber)?.doubleValue ?? 1.0, 2.0) // 2160p was too much for phones
        self.msaa = (settings["msaa"] as? NSNumber)?.boolValue ?? true
        self.fxaa = (settings["fxaa"] as? NSNumber)?.boolValue ?? true
        self.motionBlur = (settings["motionBlur"] as? NSNumber)?.boolValue ?? true
        self.vsync = (settings["vsync"] as? NSNumber)?.boolValue ?? true
        self.showFps = (settings["showFps"] as? NSNumber)?.boolValue ?? false
        self.screenScaling = max(0, min(2, (settings["screenScaling"] as? NSNumber)?.intValue ?? 0))
        self.onPlay = onPlay
    }

    func rescan() { files = GameFiles.scan(documents: documents) }

    func chosen() -> NSDictionary {
        let values: [String: Any] = [
            "frameRate": NSNumber(value: frameRate),
            "resolutionScale": NSNumber(value: resolutionScale),
            "msaa": NSNumber(value: msaa),
            "fxaa": NSNumber(value: fxaa),
            "motionBlur": NSNumber(value: motionBlur),
            "vsync": NSNumber(value: vsync),
            "showFps": NSNumber(value: showFps),
            "screenScaling": NSNumber(value: screenScaling),
            "gameFolder": files.folder as NSString,
        ]
        return values as NSDictionary
    }
}

/// a little give under the finger
private struct PressScale: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.965 : 1)
            .animation(.spring(response: 0.22, dampingFraction: 0.7), value: configuration.isPressed)
    }
}

/// fine grain over the room, drawn once on the CPU into a tile
private struct Grain: View {
    private static let tile: UIImage = {
        let side: CGFloat = 256
        let format = UIGraphicsImageRendererFormat.preferred()
        format.opaque = false
        return UIGraphicsImageRenderer(size: CGSize(width: side, height: side), format: format).image { r in
            var seed: UInt64 = 0xD3AD_0072
            func rnd() -> CGFloat { seed = seed &* 6364136223846793005 &+ 1442695040888963407; return CGFloat(seed >> 33) / CGFloat(1 << 31) }
            for _ in 0..<Int(side * side / 220) {
                let d = rnd() * 0.9 + 0.3
                UIColor.white.withAlphaComponent(0.018 + rnd() * 0.02).setFill()
                r.cgContext.fillEllipse(in: CGRect(x: rnd() * side, y: rnd() * side, width: d, height: d))
            }
        }
    }()
    var body: some View {
        Image(uiImage: Grain.tile).resizable(resizingMode: .tile).allowsHitTesting(false)
    }
}

private struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var appeared = false
    @State private var leaving = false

    private var device: String { UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone" }
    private var version: String { Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.1" }
    private func tick() { UISelectionFeedbackGenerator().selectionChanged() }

    private var resolutionLabel: String {
        let s = model.resolutionScale
        return s >= 1.75 ? "1440p" : s >= 1.25 ? "1080p" : "720p"
    }
    private var resolutionNote: String {
        let s = model.resolutionScale
        if s >= 1.75 { return "Twice the console's 720p: sharp, and hard work for the GPU." }
        if s >= 1.25 { return "1.5 times the console's 720p." }
        return "The console's own 720p: the fastest."
    }
    private var scalingNote: String {
        switch model.screenScaling {
        case 1: return "The 16:9 picture stretched over the whole screen."
        case 2: return "The 16:9 picture stretched to 1.943:1: black bars beside the Dynamic Island."
        default: return "The game's own 16:9 shape: black bars at the sides."
        }
    }
    private var logURL: URL { URL(fileURLWithPath: model.documents + "/DRRecomp.log") }

    var body: some View {
        GeometryReader { geo in
            ZStack {
                LinearGradient(colors: [Theme.roomLight, Theme.room], startPoint: .topLeading, endPoint: .bottomTrailing).ignoresSafeArea()
                Grain().ignoresSafeArea()
                HStack(spacing: 0) {
                    titlePage(height: geo.size.height)
                        .frame(width: geo.size.width * 0.40)
                    caseFile
                        .padding(.vertical, 14).padding(.trailing, 16)
                        .offset(x: appeared ? 0 : 24)
                }
                .opacity(appeared ? 1 : 0)
                night.ignoresSafeArea().opacity(leaving ? 1 : 0).allowsHitTesting(false)
            }
        }
        .preferredColorScheme(.dark)
        .persistentSystemOverlays(.hidden)
        .statusBarHidden(true)
        .onAppear { withAnimation(.easeOut(duration: 0.5)) { appeared = true } }
        .onChange(of: scenePhase) { phase in if phase == .active { model.rescan() } } // back from the Files app
        .onChange(of: model.frameRate) { _ in tick() }
        .onChange(of: model.resolutionScale) { _ in tick() }
        .onChange(of: model.msaa) { _ in tick() }
        .onChange(of: model.fxaa) { _ in tick() }
        .onChange(of: model.motionBlur) { _ in tick() }
        .onChange(of: model.vsync) { _ in tick() }
        .onChange(of: model.showFps) { _ in tick() }
        .onChange(of: model.screenScaling) { _ in tick() }
    }

    // MARK: title page

    private func titlePage(height: CGFloat) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Spacer(minLength: 8)
            Text("72:00:00")
                .font(.system(size: 15, weight: .bold, design: .monospaced))
                .foregroundStyle(Theme.watch)
                .padding(.horizontal, 8).padding(.vertical, 3)
                .background(Color.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 5, style: .continuous))
            Text("DEAD")
                .font(.system(size: min(height * 0.15, 58), weight: .black)).tracking(4)
                .foregroundStyle(Color.white).padding(.top, 14)
            Text("RISING")
                .font(.system(size: min(height * 0.15, 58), weight: .black)).tracking(4)
                .foregroundStyle(Theme.bloodBright)
                .padding(.top, -8)
            Rectangle().fill(Theme.blood).frame(width: 150, height: 4).padding(.top, 6)
            Text("Willamette, Colorado. You have 72 hours.")
                .font(.system(.footnote)).italic()
                .foregroundStyle(Color.white.opacity(0.62)).padding(.top, 10)
            Text("Your copy of the Xbox 360 game, recompiled for this \(device).")
                .font(.system(.caption)).foregroundStyle(Color.white.opacity(0.45)).padding(.top, 2)
            Button(action: play) {
                HStack(spacing: 10) {
                    if model.starting { ProgressView().tint(Color.white) } else { Image(systemName: "play.fill").font(.system(size: 14, weight: .bold)) }
                    Text(model.starting ? "STARTING" : "PLAY").font(.system(size: 19, weight: .heavy)).tracking(3)
                }
                .foregroundStyle(Color.white.opacity(model.files.ready ? 1 : 0.45))
                .frame(width: 200, height: 50)
                .background((model.files.ready ? Theme.blood : Color.gray.opacity(0.35)), in: Capsule())
                .overlay(Capsule().stroke(Color.white.opacity(0.18), lineWidth: 1))
            }
            .buttonStyle(PressScale())
            .disabled(model.starting || !model.files.ready)
            .padding(.top, 22)
            Text(model.files.ready ? "Version \(version)  ·  game files found" : "Copy your game files first: see the case file.")
                .font(.caption2).foregroundStyle(model.files.ready ? Color.white.opacity(0.4) : Theme.bloodBright)
                .padding(.top, 12)
            Spacer(minLength: 8)
        }
        .padding(.leading, 30).padding(.trailing, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func play() {
        model.rescan()
        guard !model.starting, model.files.ready else { return }
        model.starting = true
        UIImpactFeedbackGenerator(style: .heavy).impactOccurred()
        withAnimation(.easeIn(duration: 0.28)) { leaving = true }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { model.onPlay(model.chosen()) }
    }

    // MARK: case file

    private var caseFile: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 24) {
                if !model.files.ready { missingFiles }
                section(1, "Display") {
                    row("Resolution", note: resolutionNote) {
                        Picker("Resolution", selection: $model.resolutionScale) {
                            Text("720p").tag(1.0); Text("1080p").tag(1.5); Text("1440p").tag(2.0)
                        }
                        .pickerStyle(.segmented).frame(width: 200)
                    }
                    row("Screen scaling", note: scalingNote) {
                        Picker("Screen scaling", selection: $model.screenScaling) {
                            Text("Fit").tag(0); Text("Fill").tag(1); Text("Fill 1.943:1").tag(2)
                        }
                        .pickerStyle(.segmented).frame(width: 250)
                    }
                    row("Frame rate", note: model.frameRate == 60 ? "Twice as smooth. The game's timing stays as on the console." : "As on the console.") {
                        Picker("Frame rate", selection: $model.frameRate) { Text("30").tag(30); Text("60").tag(60) }
                            .pickerStyle(.segmented).frame(width: 130)
                    }
                }
                section(2, "Picture") {
                    row("Anti-aliasing", note: "Smooths edges the way the console does (4X MSAA).") { toggle($model.msaa) }
                    row("FXAA", note: "An extra smoothing filter over the final picture.") { toggle($model.fxaa) }
                    row("Motion blur", note: "Blur from camera and character movement, as on the console.") { toggle($model.motionBlur) }
                }
                section(3, "Performance") {
                    row("V-Sync", note: "Waits for the screen's refresh: no tearing.") { toggle($model.vsync) }
                    row("Show FPS", note: "Frames per second in the top-left corner.") { toggle($model.showFps) }
                }
                section(4, "Controls") {
                    info("Touch controls", "On screen whenever no controller is connected.")
                    info("Three-finger tap", "Opens and closes the graphics menu in the game.")
                    info("Four-finger tap", "Shows and hides the live log over the game.")
                    info("Start + Back", "Also opens the graphics menu.")
                }
                section(5, "Evidence") {
                    row("Game files") {
                        Text(model.files.ready ? "Found" : "Missing").font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(model.files.ready ? Theme.ink.opacity(0.75) : Theme.blood)
                    }
                    info("Saves and settings", "Kept on this \(device), in Dead Rising > game.")
                    row("Last run's log", note: "DRRecomp.log in the Dead Rising folder (Files app).") {
                        if FileManager.default.fileExists(atPath: logURL.path) {
                            ShareLink(item: logURL) {
                                Label("Share", systemImage: "square.and.arrow.up").font(.system(size: 14, weight: .semibold))
                            }
                            .foregroundStyle(Theme.blood)
                        } else {
                            Text("None yet").font(.system(size: 14)).foregroundStyle(Theme.ink.opacity(0.5))
                        }
                    }
                }
                Text("The same settings are in the game too: tap with three fingers, or press Start + Back.")
                    .font(.system(.caption)).italic().foregroundStyle(Theme.ink.opacity(0.55))
            }
            .padding(.horizontal, 24).padding(.vertical, 22)
        }
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.paper)
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(Theme.ink.opacity(0.12), lineWidth: 1))
                .shadow(color: Color.black.opacity(0.5), radius: 18, y: 8)
        )
        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .tint(Theme.blood)
    }

    /// what is missing and where it goes, with a button to look again
    private var missingFiles: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.blood)
                Text("GAME FILES NOT FOUND").font(.system(size: 14, weight: .heavy)).tracking(1.5).foregroundStyle(Theme.blood)
            }
            Text("Copy your own Dead Rising files into Files > On My \(device) > Dead Rising > game:")
                .font(.system(size: 14)).foregroundStyle(Theme.ink).fixedSize(horizontal: false, vertical: true)
            check("default.xex", model.files.xex)
            check("arcrom…arc files: \(model.files.archives) of \(GameFiles.expectedArchives)", model.files.archives >= GameFiles.expectedArchives)
            check("the ntvX folder", model.files.ntvX)
            Button {
                tick()
                model.rescan()
            } label: {
                Label("Check again", systemImage: "arrow.clockwise").font(.system(size: 14, weight: .semibold))
                    .padding(.horizontal, 14).padding(.vertical, 7)
                    .background(Theme.blood.opacity(0.12), in: Capsule())
            }
            .foregroundStyle(Theme.blood)
            .padding(.top, 2)
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(Theme.blood.opacity(0.55), lineWidth: 1.5))
    }

    // MARK: pieces

    private func check(_ text: String, _ ok: Bool) -> some View {
        HStack(spacing: 8) {
            Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(ok ? Color(red: 0.2, green: 0.5, blue: 0.25) : Theme.blood)
            Text(text).font(.system(size: 14)).foregroundStyle(Theme.ink)
        }
    }
    private func section<C: View>(_ n: Int, _ title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Text("\(n)").font(.system(size: 12, weight: .heavy)).foregroundStyle(Theme.paper)
                    .frame(width: 22, height: 22).background(Theme.blood, in: Circle())
                Text(title.uppercased()).font(.system(size: 12, weight: .heavy)).tracking(2.4).foregroundStyle(Theme.ink.opacity(0.8))
                Rectangle().fill(Theme.ink.opacity(0.18)).frame(height: 1)
            }
            content()
        }
    }
    private func row<C: View>(_ title: String, note: String? = nil, @ViewBuilder _ control: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .center, spacing: 12) {
                Text(title).font(.system(size: 16, weight: .medium)).foregroundStyle(Theme.ink).lineLimit(1).minimumScaleFactor(0.8)
                Spacer(minLength: 8)
                control()
            }
            if let note {
                Text(note).font(.caption).foregroundStyle(Theme.ink.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
    private func info(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.system(size: 16, weight: .medium)).foregroundStyle(Theme.ink)
            Text(text).font(.caption).foregroundStyle(Theme.ink.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
        }
    }
    private func toggle(_ b: Binding<Bool>) -> some View { Toggle("", isOn: b).labelsHidden().fixedSize() }
}

private final class LauncherHostingController: UIHostingController<LauncherView> {
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
}

@objc(DRLauncher) public final class DRLauncher: NSObject {
    /// settings: frameRate, resolutionScale, msaa, fxaa, motionBlur, vsync, showFps, screenScaling (NSNumber); documents: the app's
    /// Documents folder. onPlay gets the same keys back, plus gameFolder (NSString).
    @objc public static func make(settings: NSDictionary, documents: NSString, onPlay: @escaping (NSDictionary) -> Void) -> UIViewController {
        let seg = UISegmentedControl.appearance()
        seg.selectedSegmentTintColor = Theme.uiBlood
        seg.setTitleTextAttributes([.foregroundColor: Theme.uiInk], for: .normal)
        seg.setTitleTextAttributes([.foregroundColor: UIColor.white, .font: UIFont.systemFont(ofSize: 13, weight: .semibold)], for: .selected)
        let model = LauncherModel(settings: settings, documents: documents as String, onPlay: onPlay)
        let vc = LauncherHostingController(rootView: LauncherView(model: model))
        vc.view.backgroundColor = UIColor(red: 0.075, green: 0.078, blue: 0.086, alpha: 1)
        return vc
    }
}
