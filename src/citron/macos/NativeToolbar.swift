// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

private typealias Command = @convention(c) (UnsafeMutableRawPointer?, Int32) -> Void

@MainActor private final class ToolbarModel: ObservableObject {
    @Published var enabled = [true, true, false, false, true, true]
    @Published var pauseTitle = "Pause"
    @Published var paused = false
    let context: UnsafeMutableRawPointer?
    let command: Command
    init(context: UnsafeMutableRawPointer?, command: @escaping Command) {
        self.context = context
        self.command = command
    }
}

private struct NativeToolbar: View {
    @ObservedObject var model: ToolbarModel
    private func control(_ title: String, icon: String, id: Int) -> some View {
        Button { model.command(model.context, Int32(id)) } label: {
            Label(title, systemImage: icon)
        }
        .disabled(!model.enabled[id])
        .help(title)
        .accessibilityLabel(title)
    }
    @ViewBuilder private var controls: some View {
        control("Open", icon: "folder", id: 0)
        control("Add Folder", icon: "folder.badge.plus", id: 1)
        Spacer(minLength: 16)
        if model.enabled[3] {
            control(model.pauseTitle, icon: model.paused ? "play.fill" : "pause.fill", id: 2)
            control("Stop", icon: "stop.fill", id: 3)
        }
        control("GRID0+", icon: "person.2", id: 5)
        control("Settings", icon: "gearshape", id: 4).labelStyle(.iconOnly)
    }
    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Citrosis").font(.headline)
                Text(model.enabled[3] ? "Playing" : "Game Library")
                    .font(.caption).foregroundStyle(.secondary)
            }.frame(minWidth: 100, alignment: .leading)
            if #available(macOS 26.0, *) {
                controls.buttonStyle(.glass).buttonBorderShape(.capsule)
            } else {
                controls.buttonStyle(.bordered).buttonBorderShape(.capsule)
            }
        }
        .padding(.horizontal, 20).frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(.bar)
    }
}

private typealias ToolbarHost = NSHostingView<NativeToolbar>

@_cdecl("citrosis_toolbar_create")
@MainActor func createToolbar(_ context: UnsafeMutableRawPointer?, _ command: @escaping @convention(c) (UnsafeMutableRawPointer?, Int32) -> Void) -> UnsafeMutableRawPointer {
    let model = ToolbarModel(context: context, command: command)
    let host = ToolbarHost(rootView: NativeToolbar(model: model))
    host.sizingOptions = []
    return Unmanaged.passRetained(host).toOpaque()
}
@_cdecl("citrosis_toolbar_update")
@MainActor func updateToolbar(_ pointer: UnsafeMutableRawPointer, _ id: Int32, _ enabled: Bool, _ paused: Bool, _ title: UnsafePointer<CChar>) {
    let host = Unmanaged<ToolbarHost>.fromOpaque(pointer).takeUnretainedValue()
    let model = host.rootView.model
    guard model.enabled.indices.contains(Int(id)) else { return }
    model.enabled[Int(id)] = enabled
    if id == 2 {
        model.paused = paused
        model.pauseTitle = String(cString: title).replacingOccurrences(of: "&", with: "")
    }
}
@_cdecl("citrosis_toolbar_release")
@MainActor func releaseToolbar(_ pointer: UnsafeMutableRawPointer) {
    Unmanaged<ToolbarHost>.fromOpaque(pointer).release()
}
