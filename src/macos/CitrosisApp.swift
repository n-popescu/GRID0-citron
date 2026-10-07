// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI
import Metal
import QuartzCore
import UniformTypeIdentifiers

@_silgen_name("citrosis_initialize") func initialize(_ path: UnsafePointer<CChar>)
@_silgen_name("citrosis_directories") func directoriesJSON() -> UnsafePointer<CChar>
@_silgen_name("citrosis_game") func gameJSON(_ path: UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_settings") func settingsJSON() -> UnsafePointer<CChar>
@_silgen_name("citrosis_set_setting") func setSetting(_ key: UnsafePointer<CChar>, _ value: UnsafePointer<CChar>) -> Bool
@_silgen_name("citrosis_renderer") func rendererName() -> UnsafePointer<CChar>
@_silgen_name("citrosis_friend_action") func friendAction(_ id:UnsafePointer<CChar>,_ action:UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_friends") func friendsJSON() -> UnsafePointer<CChar>
@_silgen_name("citron_ios_set_metal_layer") func setSurface(_ layer: UnsafeMutableRawPointer, _ width: Int32, _ height: Int32, _ scale: Float)
@_silgen_name("citrosis_launch") func launchGame(_ path: UnsafePointer<CChar>) -> Int32
@_silgen_name("citron_ios_pause") func pauseGame()
@_silgen_name("citron_ios_resume") func resumeGame()
@_silgen_name("citrosis_stop") func stopGame()
@_silgen_name("citron_ios_shutdown") func shutdownCore()
@_silgen_name("citron_ios_set_callbacks") func setCallbacks(_ started: @escaping @convention(c) (Int32) -> Void, _ stopped: @escaping @convention(c) (Int32) -> Void)
@_silgen_name("citrosis_keyboard") func keyboard(_ key: Int32, _ down: Bool)
@_silgen_name("citrosis_release_keys") func releaseKeys()
@_silgen_name("citrosis_paths") func pathsJSON() -> UnsafePointer<CChar>
@_silgen_name("citrosis_pump_input") func pumpInput()

@_silgen_name("citron_apple_cache_stage") func cacheStage() -> Int32
@_silgen_name("citron_apple_cache_progress") func cacheProgress() -> Int32
@_silgen_name("citron_apple_cache_total") func cacheTotal() -> Int32

@_silgen_name("citrosis_fps") func coreFPS() -> Double
@_silgen_name("citrosis_input") func inputJSON(_ player:Int32) -> UnsafePointer<CChar>
@_silgen_name("citrosis_input_options") func inputOptions(_ player:Int32,_ connected:Bool,_ style:Int32,_ vibration:Bool,_ strength:Int32) -> Bool
@_silgen_name("citrosis_input_defaults") func inputDefaults(_ player:Int32,_ device:UnsafePointer<CChar>) -> Bool
@_silgen_name("citrosis_mapping_begin") func mappingBegin(_ type:Int32)
@_silgen_name("citrosis_mapping_cancel") func mappingCancel()
@_silgen_name("citrosis_mapping_poll") func mappingPoll(_ player:Int32,_ kind:UnsafePointer<CChar>,_ index:Int32,_ part:UnsafePointer<CChar>,_ device:UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_mapping_clear") func mappingClear(_ player:Int32,_ kind:UnsafePointer<CChar>,_ index:Int32,_ part:UnsafePointer<CChar>) -> Bool
@_silgen_name("citrosis_game_folder") func gameFolder(_ path:UnsafePointer<CChar>,_ kind:UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_content_operation") func contentOperation(_ path:UnsafePointer<CChar>,_ action:UnsafePointer<CChar>,_ destination:UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_operation_prepare") func operationPrepare()
@_silgen_name("citrosis_operation_cancel") func operationCancel()
@_silgen_name("citrosis_operation_progress") func operationProgress() -> Double
@_silgen_name("citrosis_input_clear") func inputClear(_ player:Int32) -> Bool
@_silgen_name("citrosis_stick_options") func stickOptions(_ player:Int32,_ index:Int32,_ deadzone:Double,_ range:Double) -> Bool
@_silgen_name("citrosis_input_profiles") func inputProfilesJSON() -> UnsafePointer<CChar>
@_silgen_name("citrosis_input_profile") func inputProfile(_ player:Int32,_ name:UnsafePointer<CChar>,_ save:Bool,_ create:Bool) -> UnsafePointer<CChar>
@_silgen_name("citrosis_game_settings") func gameSettingsJSON(_ id:UnsafePointer<CChar>) -> UnsafePointer<CChar>
@_silgen_name("citrosis_game_setting") func gameSetting(_ id:UnsafePointer<CChar>,_ key:UnsafePointer<CChar>,_ value:UnsafePointer<CChar>,_ inherit:Bool) -> Bool
struct GameSetting:Codable,Identifiable {var setting:CoreSetting; let inherit:Bool; var id:String {setting.id}}
struct ContentResult:Codable {let error:String?; let message:String?; let path:String?}
struct InputDevice:Codable,Identifiable {let id:String; let name:String}
struct InputDirection:Codable,Identifiable {let id:String; let value:String}
struct InputBinding:Codable,Identifiable {
 let index:Int; let name:String; let value:String; let directions:[InputDirection]?; var deadzone:Double?; var range:Double?; let axis:Bool?
 var id:Int {index}
}
struct InputConfiguration:Codable {
 var connected:Bool; var style:Int; var vibration:Bool; var strength:Int
 let buttons:[InputBinding]; var analogs:[InputBinding]; let motions:[InputBinding]; let devices:[InputDevice]
}
struct MappingTarget:Equatable {let kind:String; let index:Int; let part:String; let name:String}
private func nativeKey(_ event:NSEvent) -> Int32 {
 let special:[UInt16:Int32]=[123:0x01000012,124:0x01000014,125:0x01000015,126:0x01000013,36:0x01000004,53:0x01000000,48:0x01000001,51:0x01000003,117:0x01000007]
 return special[event.keyCode] ?? Int32(event.charactersIgnoringModifiers?.uppercased().unicodeScalars.first?.value ?? 0)
}

struct Game: Codable, Identifiable {
 let id: String; let path: String; let title: String; let icon: String; let size: UInt64
 var sizeText: String { ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file) }
}
struct Choice: Codable, Hashable {
 let name: String; let value: String
 var title:String {
  let titles=["AppleHypervisor":"Apple Hypervisor", "Dynarmic":"Dynarmic", "EnglishAmerican":"English (US)", "EnglishBritish":"English (UK)", "Usa":"United States", "Gpu":"GPU", "Cpu":"CPU", "Scale1X":"1×", "Scale2X":"2×"]
  return titles[name] ?? name
 }
}
struct CoreSetting: Codable, Identifiable {
 let id: String; var value: String; let label: String; let category: String
 let boolean: Bool; let choices: [Choice]; let runtime: Bool
 var title: String {
  let titles = ["cpu_backend":"CPU backend", "backend":"Graphics backend", "resolution_setup":"Internal resolution",
    "use_docked_mode":"Console mode", "vsync_mode":"Vertical sync", "switchnet_server":"GRID0+ server",
    "switchnet_username":"Username", "switchnet_password":"Password", "volume":"Volume",
    "cpu_accuracy":"CPU accuracy", "current_user":"Player profile", "language_index":"Language",
    "region_index":"Region", "time_zone_index":"Time zone", "use_multi_core":"Multicore emulation",
    "rng_seed":"Random seed", "gpu_accuracy":"GPU accuracy", "use_disk_shader_cache":"Cache shaders",
    "use_asynchronous_shaders":"Compile shaders asynchronously", "nvdec_emulation":"Video decoding",
    "accelerate_astc":"ASTC decoding", "max_anisotropy":"Anisotropic filtering", "anti_aliasing":"Anti-aliasing"]
  return titles[id] ?? id.replacingOccurrences(of:"_",with:" ").capitalized
 }
}
struct Friend: Codable, Identifiable { let id:String; let name:String; let state:String; let code:String; let icon:String }
struct FriendRequest: Codable, Identifiable {let id:String; let other:Friend}
struct GridAccount: Codable {let name:String; let code:String}
struct FriendReply: Codable {
 let friends:[Friend]?; let incoming:[FriendRequest]?; let outgoing:[FriendRequest]?; let me:GridAccount?; let error:String?
}
struct ActionReply: Codable {let error:String?; let success:Bool?}
private func decode<T: Decodable>(_ type: T.Type, _ pointer: UnsafePointer<CChar>) -> T? {
 try? JSONDecoder().decode(type, from: Data(String(cString:pointer).utf8))
}
private let backend = DispatchQueue(label:"org.citrosis.core", qos:.userInitiated)
private let network = DispatchQueue(label:"org.citrosis.grid0", qos:.userInitiated)

@MainActor final class AppModel: ObservableObject {
 static let shared = AppModel()
 @Published var games: [Game] = []
 @Published var settings: [CoreSetting] = []
 @Published var query = ""
 @Published var grid = UserDefaults.standard.bool(forKey:"gridView")
 @Published var scanning = false
 @Published var playing: Game?
 @Published var launching = false
 @Published var launchStatus = "Preparing game…"
 @Published var paused = false
 @Published var error: String?
 @Published var friends: [Friend] = []
 @Published var incoming:[FriendRequest]=[]
 @Published var outgoing:[FriendRequest]=[]
 @Published var gridAccount:GridAccount?
 @Published var friendsTab="Friends"
 @Published var friendCode=""
 @Published var friendActionBusy=false
 @Published var friendNotice:String?
 @Published var renderer="Metal"
 @Published var fps:Double=0
 @Published var controlsVisible=true
 @Published var inputPlayer=0
 @Published var inputDevice=""
 @Published var input:InputConfiguration?
 @Published var mappingTarget:MappingTarget?
 @Published var mappingSeconds=10
 @Published var inputProfileName=""
 @Published var inputProfiles:[String]=[]
 @Published var inputNotice:String?
 @Published var propertySettings:[GameSetting]=[]
 @Published var propertyGame:Game?
 var propertyWindow:NSWindow?
 @Published var contentCanCancel=true
 @Published var contentBusy=false
 @Published var contentLabel=""
 @Published var contentProgress:Double=0
 var mappingDeadline:Double=0
 var lastPointerTime=CACurrentMediaTime()
 var lastStatsTime:Double=0
 var statsPending=false
 var eventMonitor:Any?
 weak var gameWindow:NSWindow?
 @Published var settingsPage: String? = "System"
 @Published var settingsFilter = ""
 @Published var drafts: [String:String] = [:]
 @Published var friendsLoading = false
 @Published var friendsError: String?
 var settingsWindow: NSWindow?
 var friendsWindow: NSWindow?
 var roots: [String] = []
 var pendingLaunch: Game?
 var ready = false
 var inputTimer: Timer?
 var visible: [Game] {
  games.filter { query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) || $0.id.localizedCaseInsensitiveContains(query) }
   .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
 }
 init() {
  Bundle.main.bundleURL.path.withCString { initialize($0) }
  settings = decode([CoreSetting].self,settingsJSON()) ?? []
  renderer=String(cString:rendererName())
  roots = UserDefaults.standard.stringArray(forKey:"libraryFolders") ?? decode([String].self,directoriesJSON()) ?? []
  setCallbacks({ _ in DispatchQueue.main.async { AppModel.shared.launching = false; AppModel.shared.renderer=String(cString:rendererName()) } },
   { result in DispatchQueue.main.async {
     let model=AppModel.shared; model.playing=nil; model.launching=false; model.paused=false; model.ready=false
     if result != 0 { model.error="Emulation stopped with error \(result). See the Citrosis log for details." }
   }})
  scan()
  let timer=Timer(timeInterval:1.0/60.0,repeats:true) {_ in
   pumpInput()
   if self.contentBusy {self.contentProgress=operationProgress()}
   let now=CACurrentMediaTime()
   if self.playing != nil {
    if !self.launching && now-self.lastPointerTime>3 && self.controlsVisible {self.controlsVisible=false}
    if now-self.lastStatsTime>=1 && !self.statsPending && !self.launching {
     self.lastStatsTime=now
     if self.paused {self.fps=0}
     else {
      self.statsPending=true
      backend.async {
       let fps=coreFPS()
       DispatchQueue.main.async {self.statsPending=false; if self.playing != nil && !self.paused {self.fps=fps}}
      }
     }
    }
   }
   if let target=self.mappingTarget {
    if now>=self.mappingDeadline {self.cancelMapping(); self.inputNotice="Mapping timed out. Try again when you’re ready."}
    else {
     let seconds=Int(ceil(self.mappingDeadline-now)); if seconds != self.mappingSeconds {self.mappingSeconds=seconds}
     let result=target.kind.withCString {kind in target.part.withCString {part in self.inputDevice.withCString {device in decode([String:Bool].self,mappingPoll(Int32(self.inputPlayer),kind,Int32(target.index),part,device))}}}
     if result?["mapped"]==true {self.mappingTarget=nil; self.refreshInput(); self.inputNotice="Mapped \(target.name)."}
    }
   }
   if self.launching {
    let stage=cacheStage(), total=cacheTotal()
    self.launchStatus = stage==1 && total>0 ? "Preparing shaders · \(cacheProgress()) of \(total)" : (stage==0 ? "Reading shader cache…" : "Starting emulation…")
   }
  }
  RunLoop.main.add(timer,forMode:.common); inputTimer=timer
  eventMonitor=NSEvent.addLocalMonitorForEvents(matching:[.mouseMoved,.leftMouseDragged,.rightMouseDragged,.otherMouseDragged,.leftMouseDown,.keyDown,.flagsChanged]) {event in
   if self.playing != nil && event.window===self.gameWindow && [.mouseMoved,.leftMouseDragged,.rightMouseDragged,.otherMouseDragged,.leftMouseDown].contains(event.type) {self.mouseActivity()}
   if self.mappingTarget != nil && event.window===self.settingsWindow {
    if event.type == .keyDown {
     if event.keyCode==53 {self.cancelMapping()}
     else if !event.isARepeat {let key=nativeKey(event); keyboard(key,true); keyboard(key,false)}
     return nil
    }
    if event.type == .flagsChanged {
     let modifiers:[UInt16:(NSEvent.ModifierFlags,Int32)]=[56:(.shift,0x01000020),60:(.shift,0x01000020),59:(.control,0x01000021),62:(.control,0x01000021),58:(.option,0x01000023),61:(.option,0x01000023),55:(.command,0x01000022),54:(.command,0x01000022)]
     if let (flag,key)=modifiers[event.keyCode],event.modifierFlags.contains(flag) {keyboard(key,true); keyboard(key,false)}
     return nil
    }
   }
   return event
  }
 }
 func scan() {
  guard !scanning, !contentBusy, playing == nil else { return }
  scanning=true
  let folders=Array(Set(roots))
  backend.async {
   var found: [Game]=[]; var seen=Set<String>()
   for folder in folders {
    guard let enumerator=FileManager.default.enumerator(at:URL(fileURLWithPath:folder),includingPropertiesForKeys:[.isRegularFileKey],options:[.skipsHiddenFiles,.skipsPackageDescendants]) else { continue }
    for case let url as URL in enumerator {
     guard ["nsp","xci","nro","nso","kip"].contains(url.pathExtension.lowercased()) else { continue }
     let game=url.path.withCString { decode(Game.self,gameJSON($0)) }
     if let game, seen.insert(game.id).inserted { found.append(game) }
    }
   }
   DispatchQueue.main.async { self.games=found; self.scanning=false }
  }
 }
 func open() {
  guard !contentBusy, playing == nil else { return }
  let panel=NSOpenPanel(); panel.allowedContentTypes=["nsp","xci","nro","nso","kip"].compactMap { UTType(filenameExtension:$0) }
  panel.canChooseDirectories=false; panel.allowsMultipleSelection=false
  panel.begin { response in
   guard response == .OK, let url=panel.url else { return }
   backend.async {
    let game=url.path.withCString { decode(Game.self,gameJSON($0)) }
    DispatchQueue.main.async { if let game {self.play(game)} else {self.error="This file could not be read as a supported game."} }
   }
  }
 }
 func addFolder() {
  let panel=NSOpenPanel(); panel.canChooseDirectories=true; panel.canChooseFiles=false; panel.prompt="Add Folder"
  panel.begin { response in
   guard response == .OK, let url=panel.url else { return }
   if !self.roots.contains(url.path) { self.roots.append(url.path); UserDefaults.standard.set(self.roots,forKey:"libraryFolders") }
   self.scan()
  }
 }
 func play(_ game: Game) {
  guard !contentBusy else {return}
  guard playing == nil, !scanning else { return }
  cancelMapping(); fps=0; mouseActivity(); playing=game; launching=true; ready=false; pendingLaunch=game
 }
 func surfaceReady() {
  guard !ready, let game=pendingLaunch else { return }
  ready=true; pendingLaunch=nil
  backend.async {
   let result=game.path.withCString { launchGame($0) }
   if result != 0 { DispatchQueue.main.async { self.error="Could not launch \(game.title) (error \(result))."; self.playing=nil; self.launching=false; self.ready=false } }
  }
 }
 func pause() {
  guard playing != nil, !launching else { return }
  mouseActivity(); paused.toggle(); let value=paused
  backend.async { if value {pauseGame()} else {resumeGame()} }
 }
 func stop() {
  guard playing != nil else { return }
  backend.async {stopGame(); DispatchQueue.main.async {self.settings=decode([CoreSetting].self,settingsJSON()) ?? []}}
 }
 func reveal(_ name:String) {
  if let paths=decode([String:String].self,pathsJSON()), let path=paths[name] {
   NSWorkspace.shared.open(URL(fileURLWithPath:path))
  }
 }
 func update(_ id: String, _ value: String) {
  guard playing == nil else { return }
  if id.withCString({ key in value.withCString {setSetting(key,$0)} }) {
   settings=decode([CoreSetting].self,settingsJSON()) ?? []
  }
 }
 func showSettings() {
  if settingsWindow == nil {
   let window=NSWindow(contentRect:NSRect(x:0,y:0,width:1060,height:790), styleMask:[.titled,.closable,.miniaturizable,.resizable],backing:.buffered,defer:false)
   window.title="Citrosis Settings"; window.titlebarAppearsTransparent=true; window.isReleasedWhenClosed=false
   window.contentViewController=NSHostingController(rootView:SettingsView(model:self)); window.minSize=NSSize(width:780,height:520)
   window.setContentSize(NSSize(width:1060,height:790))
   window.center(); settingsWindow=window
  }
  NSApp.activate(ignoringOtherApps:true); settingsWindow?.makeKeyAndOrderFront(nil)
 }
 func showAccount() {settingsPage="GRID0+"; settingsFilter=""; showSettings()}
 func showFriends() {
  if friendsWindow == nil {
   let window=NSWindow(contentRect:NSRect(x:0,y:0,width:640,height:520),styleMask:[.titled,.closable,.resizable],backing:.buffered,defer:false)
   window.title="GRID0+"; window.isReleasedWhenClosed=false
   window.contentViewController=NSHostingController(rootView:FriendsView(model:self))
   window.minSize=NSSize(width:580,height:460); window.setContentSize(NSSize(width:700,height:580)); window.center(); friendsWindow=window
  }
  NSApp.activate(ignoringOtherApps:true); friendsWindow?.makeKeyAndOrderFront(nil)
  refreshFriends()
 }
 func refreshFriends() {
  guard !friendsLoading else {return}; friendsLoading=true; friendsError=nil
  network.async {
   let reply=decode(FriendReply.self,friendsJSON())
   DispatchQueue.main.async {self.friends=reply?.friends ?? self.friends; self.incoming=reply?.incoming ?? self.incoming; self.outgoing=reply?.outgoing ?? self.outgoing; self.gridAccount=reply?.me; self.friendsError=reply?.error ?? (reply == nil ? "Could not read the GRID0+ response." : nil); self.friendsLoading=false}
  }
 }
 func showGameProperties(_ game:Game) {
  guard playing == nil, !contentBusy else {return}
  propertyGame=game; propertySettings=game.id.withCString {decode([GameSetting].self,gameSettingsJSON($0)) ?? []}
  if propertyWindow==nil {
   let window=NSWindow(contentRect:NSRect(x:0,y:0,width:680,height:720),styleMask:[.titled,.closable,.resizable],backing:.buffered,defer:false)
   window.isReleasedWhenClosed=false; window.contentView=NSHostingView(rootView:GamePropertiesView(model:self)); window.center(); propertyWindow=window
  }
  propertyWindow?.title="Game Properties · \(game.title)"; propertyWindow?.makeKeyAndOrderFront(nil)
 }
 func updateGameSetting(_ row:GameSetting,_ value:String,_ inherit:Bool) {
  guard let game=propertyGame, playing == nil, !contentBusy else {return}
  let saved=game.id.withCString {id in row.id.withCString {key in value.withCString {gameSetting(id,key,$0,inherit)}}}
  if saved {propertySettings=game.id.withCString {decode([GameSetting].self,gameSettingsJSON($0)) ?? []}} else {error="Could not save game settings."}
 }
 func openGameFolder(_ game:Game,_ kind:String) {
  guard !contentBusy, playing == nil else {return}
  backend.async {
   let result=game.path.withCString {path in kind.withCString {decode(ContentResult.self,gameFolder(path,$0))}}
   DispatchQueue.main.async {if let path=result?.path {NSWorkspace.shared.open(URL(fileURLWithPath:path))} else {self.error=result?.error ?? "Could not open this folder."}}
  }
 }
 func performContent(_ paths:[String],_ action:String,_ destination:String="") {
  guard !contentBusy, playing == nil, !paths.isEmpty else {return}
  contentCanCancel = !action.hasPrefix("remove_"); contentBusy=true; contentProgress=0; contentLabel=action=="install" ? "Installing content…":action=="verify" ? "Verifying integrity…":"Managing game content…"
  operationPrepare()
  backend.async {
   var messages:[String]=[]; var output:String?
   for path in paths {
    let result=path.withCString {p in action.withCString {a in destination.withCString {decode(ContentResult.self,contentOperation(p,a,$0))}}}
    if let failure=result?.error {messages.append(failure); break}
    messages.append(result?.message ?? "Operation finished."); output=result?.path
   }
   DispatchQueue.main.async {self.contentBusy=false; self.error=messages.joined(separator:"\n"); if let output {NSWorkspace.shared.open(URL(fileURLWithPath:output))}; self.scan()}
  }
 }
 func installContent() {
  guard playing == nil, !contentBusy else {return}
  let panel=NSOpenPanel(); panel.title="Install Updates and DLC to NAND"; panel.allowedContentTypes=["nsp","nca"].compactMap {UTType(filenameExtension:$0)}; panel.allowsMultipleSelection=true
  panel.begin {if $0 == .OK {self.performContent(panel.urls.map(\.path),"install")}}
 }
 func extract(_ game:Game,_ kind:String) {
  guard playing == nil, !contentBusy else {return}
  let panel=NSOpenPanel(); panel.title=kind=="exefs" ? "Choose an ExeFS Dump Folder":"Choose a RomFS Dump Folder"; panel.canChooseDirectories=true; panel.canChooseFiles=false; panel.canCreateDirectories=true; panel.prompt="Extract Here"
  panel.begin {if $0 == .OK, let url=panel.url {self.performContent([game.path],kind,url.path)}}
 }
 func removeContent(_ game:Game,_ kind:String) {
  let alert=NSAlert(); alert.messageText=kind=="remove_update" ? "Remove the installed update for \(game.title)?":"Remove all installed DLC for \(game.title)?"
  alert.informativeText="This removes installed content from NAND/SD. You will need its original package to reinstall it. Save data and your source game file are preserved."
  alert.addButton(withTitle:"Cancel"); alert.addButton(withTitle:"Remove")
  if alert.runModal() == .alertSecondButtonReturn {performContent([game.path],kind)}
 }
 func useProfile(_ name:String,_ save:Bool=false,_ create:Bool=false) {
  cancelMapping()
  let result=name.withCString {decode(ContentResult.self,inputProfile(Int32(inputPlayer),$0,save,create))}
  inputNotice=result?.error ?? result?.message; refreshInput(); if result?.error==nil {inputProfileName=name}
 }
 func newInputProfile() {
  let alert=NSAlert(); alert.messageText="New Input Profile"; alert.informativeText="Save this player's controller mappings as a reusable profile."
  let field=NSTextField(frame:NSRect(x:0,y:0,width:260,height:24)); field.placeholderString="Profile name"; alert.accessoryView=field
  alert.addButton(withTitle:"Save"); alert.addButton(withTitle:"Cancel"); alert.window.initialFirstResponder=field
  if alert.runModal() == .alertFirstButtonReturn {useProfile(field.stringValue,true,true)}
 }
 func deleteInputProfile() {
  guard !inputProfileName.isEmpty, let paths=decode([String:String].self,pathsJSON()), let root=paths["input_profiles"] else {return}
  let url=URL(fileURLWithPath:root).appendingPathComponent(inputProfileName+".ini")
  NSWorkspace.shared.recycle([url]) {_,failure in DispatchQueue.main.async {
   if let failure {self.inputNotice=failure.localizedDescription} else {self.inputProfileName=""; self.inputNotice="Input profile moved to Trash."}; self.refreshInput()
  }}
 }
 func clearInput() {
  cancelMapping()
  let alert=NSAlert(); alert.messageText="Clear Player \(inputPlayer+1) mappings?"; alert.informativeText="Buttons, sticks and motion bindings will be cleared. Saved input profiles are preserved."
  alert.addButton(withTitle:"Cancel"); alert.addButton(withTitle:"Clear")
  if alert.runModal() == .alertSecondButtonReturn {_=inputClear(Int32(inputPlayer)); refreshInput(); inputNotice="Mappings cleared."}
 }
 func stickCalibration(_ index:Int,_ deadzone:Double,_ range:Double) {
  _=stickOptions(Int32(inputPlayer),Int32(index),deadzone,range); refreshInput()
 }
 func mouseActivity() {lastPointerTime=CACurrentMediaTime(); if !controlsVisible {controlsVisible=true}}
 func refreshInput() {
  inputProfiles=decode([String].self,inputProfilesJSON()) ?? []
  input=decode(InputConfiguration.self,inputJSON(Int32(inputPlayer)))
  if inputDevice.isEmpty || !(input?.devices.contains {$0.id==inputDevice} ?? false) {inputDevice=input?.devices.first?.id ?? ""}
 }
 func selectPlayer(_ player:Int) {cancelMapping(); inputPlayer=player; inputProfileName=""; inputDevice=""; inputNotice=nil; refreshInput()}
 func saveInputOptions() {
  guard playing==nil, let input else {return}
  if !inputOptions(Int32(inputPlayer),input.connected,Int32(input.style),input.vibration,Int32(input.strength)) {inputNotice="Could not save controller options."}
  refreshInput()
 }
 func useInputDefaults() {
  guard playing==nil else {return}; cancelMapping()
  let success=inputDevice.withCString {inputDefaults(Int32(inputPlayer),$0)}
  inputNotice=success ? "Default mapping applied.":"This device has no automatic mapping. Map its controls individually."
  refreshInput()
 }
 func startMapping(_ target:MappingTarget) {
  guard playing==nil else {return}; cancelMapping(); inputNotice=nil
  mappingTarget=target; mappingDeadline=CACurrentMediaTime()+10; mappingSeconds=10
  mappingBegin(target.kind=="motion" ? 2:(target.kind=="analog" && target.part != "modifier" ? 1:0))
 }
 func cancelMapping() {mappingCancel(); mappingTarget=nil}
 func clearMapping(_ target:MappingTarget) {
  guard playing==nil else {return}; cancelMapping()
  _=target.kind.withCString {kind in target.part.withCString {mappingClear(Int32(inputPlayer),kind,Int32(target.index),$0)}}; refreshInput()
 }

 func performFriendAction(_ id:String,_ action:String) {
  guard !friendsLoading, !friendActionBusy else {return}
  friendActionBusy=true; friendNotice=nil
  network.async {
   let reply=id.withCString {identifier in action.withCString {decode(ActionReply.self,friendAction(identifier,$0))}}
   DispatchQueue.main.async {
    self.friendActionBusy=false
    if let error=reply?.error {self.friendsError=error}
    else if reply?.success==true {
     if action=="send" {self.friendCode=""; self.friendNotice="Friend request sent."}
     self.refreshFriends()
    } else {self.friendsError="The action could not be completed. Please refresh before trying again."}
   }
  }
 }

}

struct GlassSurface<Content:View>:View {
 @ViewBuilder let content:Content
 var body:some View {
  if #available(macOS 26,*) {content.glassEffect(.regular,in:Capsule())}
  else {content.background(.regularMaterial,in:Capsule())}
 }
}
struct GlassIconButton:View {
 let title:String; let symbol:String; var selected=false; let action:()->Void
 var button:some View {
  Button(action:action) {Image(systemName:symbol).font(.system(size:16,weight:.medium)).frame(width:32,height:32)}
   .accessibilityLabel(title).help(title)
 }
 var body:some View {
  if #available(macOS 26,*) {
   if selected {button.buttonStyle(.glassProminent).buttonBorderShape(.circle)}
   else {button.buttonStyle(.glass).buttonBorderShape(.circle)}
  } else {
   if selected {button.buttonStyle(.borderedProminent).buttonBorderShape(.circle)}
   else {button.buttonStyle(.bordered).buttonBorderShape(.circle)}
  }
 }
}
// Use AppKit's segmented control rather than separate glass buttons. Native tracking
// owns click/hold/drag selection, keyboard navigation and the Liquid Glass highlight.
struct GlassSwitcher:NSViewRepresentable {
 @Binding var selection:String
 let options:[String]
 var symbols:[String]=[]
 var label:String="View"
 @Environment(\.isEnabled) private var enabled
 func makeCoordinator()->Coordinator {Coordinator(self)}
 func makeNSView(context:Context)->NSSegmentedControl {
  let control=NSSegmentedControl()
  control.trackingMode = .selectOne
  control.segmentStyle = .rounded
  control.controlSize = .large
  control.segmentDistribution = .fillEqually
  control.target=context.coordinator
  control.action=#selector(Coordinator.select(_:))
  control.setContentHuggingPriority(.required,for:.horizontal)
  control.setContentHuggingPriority(.required,for:.vertical)
  configure(control,context:context)
  return control
 }
 func updateNSView(_ control:NSSegmentedControl,context:Context) {
  configure(control,context:context)
 }
 private func configure(_ control:NSSegmentedControl,context:Context) {
  context.coordinator.parent=self
  if control.segmentCount != options.count {control.segmentCount=options.count}
  for index in options.indices {
   let symbol=index < symbols.count ? symbols[index]:""
   let image=symbol.isEmpty ? nil:NSImage(systemSymbolName:symbol,accessibilityDescription:options[index])?.withSymbolConfiguration(.init(pointSize:16,weight:.medium))
   control.setImage(image,forSegment:index)
   control.setLabel(image == nil ? options[index]:"",forSegment:index)
   control.setToolTip(options[index],forSegment:index)
   control.setWidth(image == nil ? 36:42,forSegment:index)
  }
  control.selectedSegment=options.firstIndex(of:selection) ?? -1
  control.isEnabled=enabled
  control.setAccessibilityLabel(label)
 }
 func sizeThatFits(_ proposal:ProposedViewSize,nsView:NSSegmentedControl,context:Context)->CGSize? {
  nsView.intrinsicContentSize
 }
 final class Coordinator:NSObject {
  var parent:GlassSwitcher
  init(_ parent:GlassSwitcher) {self.parent=parent}
  @objc func select(_ control:NSSegmentedControl) {
   guard parent.options.indices.contains(control.selectedSegment) else {return}
   parent.selection=parent.options[control.selectedSegment]
  }
 }
}
struct GameArtwork: View {
 let game: Game; let size: CGFloat
 var body: some View {
  Group {
   if let image=NSImage(contentsOfFile:game.icon) { Image(nsImage:image).resizable().scaledToFill() }
   else { ZStack {Color.accentColor.opacity(0.15); Image(systemName:"gamecontroller.fill").font(.largeTitle).foregroundStyle(.secondary)} }
  }.frame(width:size,height:size).clipShape(RoundedRectangle(cornerRadius:size*0.16))
 }
}
struct LibraryView: View {
 @ObservedObject var model: AppModel
 @Environment(\.accessibilityReduceMotion) var reduceMotion
 var body: some View {
  VStack(spacing:0) {
   if model.playing == nil {
   HStack(spacing:16) {
    VStack(alignment:.leading,spacing:3) {
     Text(model.playing?.title ?? "Library").font(.title2.weight(.semibold))
     Text("\(model.games.count) game\(model.games.count == 1 ? "":"s")").font(.caption).foregroundStyle(.secondary)
    }
    Spacer()
    HStack(spacing:10) {
     GlassIconButton(title:"Open Game",symbol:"folder",action:model.open)
     GlassIconButton(title:"Add Game Folder",symbol:"folder.badge.plus",action:model.addFolder)
     GlassSwitcher(selection:Binding(get:{model.grid ? "Grid":"List"},set:{model.grid=$0=="Grid"; UserDefaults.standard.set(model.grid,forKey:"gridView")}),options:["Grid","List"],symbols:["square.grid.2x2","list.bullet"])
     GlassIconButton(title:"GRID0+",symbol:"person.2",action:model.showFriends)
     GlassIconButton(title:"Settings",symbol:"gearshape",action:model.showSettings)
    }
    if model.playing == nil {
     TextField("Search your library",text:$model.query).textFieldStyle(.roundedBorder).frame(width:240)
    }
   }.padding(.horizontal,28).padding(.vertical,14).background(.bar)
   }
   if let game=model.playing {
    ZStack {
     GameSurface(model:model).background(.black)
     VStack {
      HStack(spacing:12) {
       GlassSurface {
        HStack(spacing:10) {
         Image(systemName:"gamecontroller.fill")
         VStack(alignment:.leading,spacing:2) {
          Text(game.title).font(.caption.weight(.semibold)).lineLimit(1)
          Text(model.renderer + " · " + String(format:"%.0f FPS",model.fps) + (model.paused ? " · Paused":"")).font(.caption2).monospacedDigit().foregroundStyle(.secondary)
         }
        }.padding(.horizontal,16).padding(.vertical,9)
       }
       Spacer()
       GlassIconButton(title:model.paused ? "Resume":"Pause",symbol:model.paused ? "play.fill":"pause.fill",action:model.pause).disabled(model.launching)
       GlassIconButton(title:"Stop",symbol:"stop.fill",action:model.stop)
       GlassIconButton(title:"GRID0+",symbol:"person.2",action:model.showFriends)
       GlassIconButton(title:"Settings",symbol:"gearshape",action:model.showSettings)
      }.padding(16)
      Spacer()
     }.opacity(model.controlsVisible ? 1:0).allowsHitTesting(model.controlsVisible)
      .animation(reduceMotion ? nil:.easeOut(duration:0.22),value:model.controlsVisible)

     if model.launching {
      VStack(spacing:16) { GameArtwork(game:game,size:100); Text("Launching \(game.title)…").font(.headline); Text(model.launchStatus).font(.caption).foregroundStyle(.secondary); ProgressView() }
       .padding(32).background(.regularMaterial,in:RoundedRectangle(cornerRadius:24)).allowsHitTesting(false)
     }
    }
   } else if model.scanning {
    VStack(spacing:16) {ProgressView(); Text("Reading your library…").foregroundStyle(.secondary)}.frame(maxWidth:.infinity,maxHeight:.infinity)
   } else if model.visible.isEmpty {
    ContentUnavailableView {Label(model.query.isEmpty ? "Your games, at home":"No matching games",systemImage:"gamecontroller")} description:{Text(model.query.isEmpty ? "Add a game folder to build your library.":"Try another title or title ID.")} actions:{Button("Add Game Folder",action:model.addFolder)}
   } else {
    ScrollView {
     if model.grid {
      LazyVGrid(columns:[GridItem(.adaptive(minimum:190),spacing:24)],spacing:24) {
       ForEach(model.visible) {game in
        Button {model.play(game)} label:{VStack(alignment:.leading,spacing:12) {GameArtwork(game:game,size:164); Text(game.title).font(.headline).lineLimit(2); Text(game.sizeText).font(.caption).foregroundStyle(.secondary)}.padding(16).frame(maxWidth:.infinity).background(.quaternary,in:RoundedRectangle(cornerRadius:20))}.buttonStyle(.plain).contextMenu {gameMenu(game)}
       }
      }.padding(28)
     } else {
      LazyVStack(spacing:12) {
       ForEach(model.visible) {game in
        Button {model.play(game)} label:{
         HStack(spacing:18) {GameArtwork(game:game,size:76); VStack(alignment:.leading,spacing:8) {Text(game.title).font(.headline); Text(URL(fileURLWithPath:game.path).pathExtension.uppercased()).font(.caption.weight(.medium)).foregroundStyle(.tint)}; Spacer(); VStack(alignment:.trailing,spacing:8) {Text(game.id).font(.system(.caption,design:.monospaced)).foregroundStyle(.secondary); Text(game.sizeText).font(.caption).foregroundStyle(.secondary)}; Image(systemName:"play.circle.fill").font(.title2).foregroundStyle(.tint)}
          .padding(18).background(.quaternary,in:RoundedRectangle(cornerRadius:20))
        }.buttonStyle(.plain).contextMenu {gameMenu(game)}
       }
      }.padding(28)
     }
    }.animation(reduceMotion ? nil:.easeInOut(duration:0.18),value:model.grid)
   }
  }.frame(minWidth:820,minHeight:540)
   .sheet(isPresented:$model.contentBusy) {
    VStack(spacing:18) {Text(model.contentLabel).font(.headline); ProgressView(value:model.contentProgress).frame(width:320); Text("\(Int(model.contentProgress*100))%").font(.caption.monospacedDigit()); if model.contentCanCancel {Button("Cancel",action:operationCancel)}}.padding(30).interactiveDismissDisabled()
   }
   .alert("Citrosis",isPresented:Binding(get:{model.error != nil},set:{if !$0 {model.error=nil}})) {Button("OK") {model.error=nil}} message:{Text(model.error ?? "")}
 }
 @ViewBuilder func gameMenu(_ game:Game) -> some View {
  Button("Play") {model.play(game)}.disabled(model.contentBusy || model.playing != nil)
  Button("Properties…") {model.showGameProperties(game)}.disabled(model.playing != nil || model.contentBusy)
  Button("Show in Finder") {NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath:game.path)])}
  Divider()
  Button("Open Save Data Folder") {model.openGameFolder(game,"save")}.disabled(model.playing != nil || model.contentBusy)
  Button("Open Mods Folder") {model.openGameFolder(game,"mods")}.disabled(model.playing != nil || model.contentBusy)
  Button("Open Shader Cache Folder") {model.openGameFolder(game,"shaders")}.disabled(model.playing != nil || model.contentBusy)
  Button("Open Dump Folder") {model.openGameFolder(game,"dump")}.disabled(model.playing != nil || model.contentBusy)
  Divider()
  Button("Install Updates or DLC…",action:model.installContent).disabled(model.playing != nil || model.contentBusy)
  Menu("Extract Game Files") {
   Button("RomFS…") {model.extract(game,"romfs")}
   Button("RomFS Directory Structure…") {model.extract(game,"skeleton")}
   Button("ExeFS…") {model.extract(game,"exefs")}
  }.disabled(model.playing != nil || model.contentBusy)
  Button("Verify Integrity") {model.performContent([game.path],"verify")}.disabled(model.playing != nil || model.contentBusy)
  Menu("Remove Installed Content") {
   Button("Update…") {model.removeContent(game,"remove_update")}
   Button("All DLC…") {model.removeContent(game,"remove_dlc")}
  }.disabled(model.playing != nil || model.contentBusy)
  Divider()
  Button("Copy Title ID") {NSPasteboard.general.clearContents(); NSPasteboard.general.setString(game.id,forType:.string)}
 }
}

struct SettingsView: View {
 @ObservedObject var model: AppModel
 let pages=["General","System","Graphics","Audio","Input","Network","GRID0+","Advanced"]
 let icons=["General":"gearshape","System":"cpu","Graphics":"cube","Audio":"speaker.wave.2","Input":"gamecontroller","Network":"network","GRID0+":"person.2","Advanced":"ladybug"]
 func includes(_ setting:CoreSetting) -> Bool {
  if !model.settingsFilter.isEmpty { return setting.title.localizedCaseInsensitiveContains(model.settingsFilter) }
  switch model.settingsPage ?? "System" {
   case "System": return ["cpu_backend","cpu_accuracy","use_multi_core","use_docked_mode","language_index","region_index","time_zone_index","rng_seed","use_rng_seed","current_user"].contains(setting.id)
   case "Graphics": return ["backend","resolution_setup","gpu_accuracy","vsync_mode","scaling_filter","anti_aliasing","max_anisotropy","use_disk_shader_cache","use_asynchronous_shaders","nvdec_emulation","accelerate_astc","astc_recompression","aspect_ratio"].contains(setting.id)
   case "Audio": return ["Audio","SystemAudio"].contains(setting.category)
   case "Input": return setting.category=="Controls"
   case "Network": return setting.category=="Network" && !setting.id.contains("switchnet")
   case "GRID0+": return setting.id.contains("switchnet") || setting.id.contains("private_server")
   case "General": return ["Miscellaneous","Data Storage"].contains(setting.category)
   default:return true
  }
 }
 var body: some View {
  NavigationSplitView {
   List(pages,id:\.self,selection:$model.settingsPage) {page in Label(page,systemImage:icons[page] ?? "gearshape").tag(page).padding(.vertical,5)}
    .navigationSplitViewColumnWidth(210)
  } detail:{
   VStack(alignment:.leading,spacing:18) {
    HStack {Text(model.settingsPage ?? "System").font(.largeTitle.weight(.semibold)); Spacer(); TextField("Find a setting",text:$model.settingsFilter).textFieldStyle(.roundedBorder).frame(width:220)}
    if model.playing != nil {Label("Stop emulation to change settings.",systemImage:"pause.circle").foregroundStyle(.secondary)}
    ScrollView {
     LazyVStack(spacing:8) {
      if model.settingsPage=="Input" && model.settingsFilter.isEmpty {InputSettingsView(model:model)}
      ForEach(model.settings.filter(includes)) {setting in settingRow(setting).padding(14).background(.quaternary,in:RoundedRectangle(cornerRadius:14))}
     }
    }
   }.padding(26)
  }.frame(minWidth:980,minHeight:650).onDisappear {model.cancelMapping()}
 }
 @ViewBuilder func settingRow(_ setting:CoreSetting) -> some View {
  HStack(spacing:20) {
   VStack(alignment:.leading,spacing:5) {Text(setting.title).font(.body.weight(.medium))}
   Spacer()
   if setting.boolean {
    Toggle(setting.title,isOn:Binding(get:{setting.value=="true"},set:{model.update(setting.id,$0 ? "true":"false")})).labelsHidden().toggleStyle(.switch)
   } else if !setting.choices.isEmpty {
    Picker(setting.title,selection:Binding(get:{setting.value},set:{model.update(setting.id,$0)})) {ForEach(setting.choices,id:\.value) {choice in Text(choice.title).tag(choice.value)}}.labelsHidden().frame(maxWidth:230)
   } else if setting.id.contains("password") || setting.id.contains("token") {
    SecureField("",text:draft(setting)).onSubmit {commit(setting)}.frame(width:230)
   } else {
    TextField("",text:draft(setting)).onSubmit {commit(setting)}.textFieldStyle(.roundedBorder).frame(width:230)
   }
   if model.drafts[setting.id] != nil {
    Button("Apply") {commit(setting)}.buttonStyle(.bordered)
   }
  }.disabled(model.playing != nil)
 }
 func draft(_ setting:CoreSetting) -> Binding<String> {
  Binding(get:{model.drafts[setting.id] ?? setting.value},set:{model.drafts[setting.id]=$0})
 }
 func commit(_ setting:CoreSetting) {
  guard let value=model.drafts.removeValue(forKey:setting.id) else {return}
  model.update(setting.id,value)
 }
}
struct GamePropertiesView:View {
 @ObservedObject var model:AppModel
 var body:some View {
  VStack(alignment:.leading,spacing:18) {
   if let game=model.propertyGame {
    HStack(spacing:16) {GameArtwork(game:game,size:72); VStack(alignment:.leading,spacing:5) {Text(game.title).font(.title2.weight(.semibold)); Text(game.id+" · "+game.sizeText).font(.caption.monospaced()).foregroundStyle(.secondary)}}
    Text("Overrides apply only to this game. Changes are saved automatically.").font(.callout).foregroundStyle(.secondary)
    ScrollView {
     LazyVStack(spacing:10) {ForEach(model.propertySettings) {row in
      VStack(alignment:.leading,spacing:10) {
       HStack {Text(row.setting.title).font(.headline); Spacer(); Toggle("Use Global",isOn:Binding(get:{row.inherit},set:{model.updateGameSetting(row,row.setting.value,$0)})).toggleStyle(.switch)}
       if row.setting.boolean {
        Toggle("Enabled",isOn:Binding(get:{row.setting.value=="true"},set:{model.updateGameSetting(row,$0 ? "true":"false",false)})).disabled(row.inherit)
       } else {
        Picker(row.setting.title,selection:Binding(get:{row.setting.value},set:{model.updateGameSetting(row,$0,false)})) {ForEach(row.setting.choices,id:\.value) {Text($0.title).tag($0.value)}}.labelsHidden().disabled(row.inherit)
       }
      }.padding(14).background(.quaternary,in:RoundedRectangle(cornerRadius:14))
     }}
    }
   }
  }.padding(24).frame(minWidth:580,minHeight:520).disabled(model.playing != nil || model.contentBusy)
 }
}
struct ControllerSilhouette:Shape {
 func path(in rect:CGRect) -> Path {
  let w=rect.width,h=rect.height
  var p=Path(); p.move(to:CGPoint(x:w*0.25,y:h*0.12))
  p.addCurve(to:CGPoint(x:w*0.75,y:h*0.12),control1:CGPoint(x:w*0.4,y:0),control2:CGPoint(x:w*0.6,y:0))
  p.addCurve(to:CGPoint(x:w*0.96,y:h*0.88),control1:CGPoint(x:w*0.91,y:h*0.15),control2:CGPoint(x:w,y:h*0.7))
  p.addCurve(to:CGPoint(x:w*0.68,y:h*0.72),control1:CGPoint(x:w*0.89,y:h*1.13),control2:CGPoint(x:w*0.75,y:h*0.85))
  p.addQuadCurve(to:CGPoint(x:w*0.32,y:h*0.72),control:CGPoint(x:w*0.5,y:h*0.62))
  p.addCurve(to:CGPoint(x:w*0.04,y:h*0.88),control1:CGPoint(x:w*0.25,y:h*0.85),control2:CGPoint(x:w*0.11,y:h*1.13))
  p.addCurve(to:CGPoint(x:w*0.25,y:h*0.12),control1:CGPoint(x:0,y:h*0.7),control2:CGPoint(x:w*0.09,y:h*0.15)); p.closeSubpath(); return p
 }
}
struct InputSettingsView:View {
 @ObservedObject var model:AppModel
 let styles=["Pro Controller","Joy-Con pair","Left Joy-Con","Right Joy-Con","Handheld","GameCube","Poké Ball","NES","SNES","Nintendo 64","Sega Genesis","Xbox","DualSense"]
 var body:some View {
  VStack(alignment:.leading,spacing:16) {
   GlassSwitcher(selection:Binding(get:{String(model.inputPlayer+1)},set:{if let player=Int($0) {model.selectPlayer(player-1)}}),options:(1...8).map {String($0)},label:"Player")
   if let input=model.input {
    VStack(spacing:12) {
     HStack {
      Toggle("Connect Controller",isOn:Binding(get:{model.input?.connected ?? false},set:{model.input?.connected=$0; model.saveInputOptions()})).toggleStyle(.switch)
      Spacer()
      Picker("Style",selection:Binding(get:{model.input?.style ?? 0},set:{model.input?.style=$0; model.saveInputOptions()})) {ForEach(Array(styles.enumerated()),id:\.offset) {Text($0.element).tag($0.offset)}}.frame(width:265)
     }
     HStack {
      Picker("Input Device",selection:$model.inputDevice) {ForEach(input.devices) {Text($0.name).tag($0.id)}}
      GlassIconButton(title:"Refresh Devices",symbol:"arrow.clockwise",action:model.refreshInput)
      Button("Defaults",action:model.useInputDefaults).buttonStyle(.bordered)
      Button("Clear…",action:model.clearInput).buttonStyle(.bordered)
     }
     HStack {
      Picker("Profile",selection:Binding(get:{model.inputProfileName},set:{if $0.isEmpty {model.inputProfileName=""} else {model.useProfile($0)}})) {Text("Current mappings").tag(""); ForEach(model.inputProfiles,id:\.self) {Text($0).tag($0)}}
      Button("Save") {model.useProfile(model.inputProfileName,true)}.disabled(model.inputProfileName.isEmpty)
      Button("New…",action:model.newInputProfile)
      Button("Delete",action:model.deleteInputProfile).disabled(model.inputProfileName.isEmpty)
     }
    }.padding(16).background(.quaternary,in:RoundedRectangle(cornerRadius:16))
    if let target=model.mappingTarget {
     HStack {ProgressView().controlSize(.small); Text("\(target.name) · \(model.mappingSeconds)s"); Spacer(); Button("Cancel",action:model.cancelMapping)}
      .padding(12).background(.tint.opacity(0.1),in:RoundedRectangle(cornerRadius:12))
    }
    Text(model.inputNotice ?? "Click a control to map it. Move a stick in a circle to capture its axes. Escape cancels.").font(.caption).foregroundStyle(.secondary)
    HStack(alignment:.top,spacing:14) {
     VStack(spacing:14) {stickPanel(input.analogs[0]); buttonPanel("D-Pad",[13,12,14,15])}.frame(width:185)
     VStack(spacing:14) {
      HStack(spacing:8) {buttonPanel("Left Shoulder",[6,8]); buttonPanel("Right Shoulder",[7,9])}
      controllerDiagram.frame(height:245)
      buttonPanel("System",[11,10,19,18])
      buttonPanel("Motion",[],motion:true)
     }.frame(maxWidth:.infinity)
     VStack(spacing:14) {buttonPanel("Face Buttons",[2,3,0,1]); stickPanel(input.analogs[1])}.frame(width:185)
    }
    DisclosureGroup("Joy-Con Rail Buttons") {HStack {ForEach([16,17,20,21],id:\.self) {buttonBinding($0)}}.padding(.top,8)}
    HStack {
     Toggle("Vibration",isOn:Binding(get:{model.input?.vibration ?? false},set:{model.input?.vibration=$0; model.saveInputOptions()})).toggleStyle(.switch)
     Slider(value:Binding(get:{Double(model.input?.strength ?? 100)},set:{model.input?.strength=Int($0)}),in:0...100,step:1,onEditingChanged:{if !$0 {model.saveInputOptions()}}).frame(width:120).disabled(!input.vibration)
     Text("\(model.input?.strength ?? 100)%").font(.caption.monospacedDigit())
     Spacer()
     HStack(spacing:5) {ForEach(0..<8) {i in Circle().fill(i==model.inputPlayer && input.connected ? Color.green:Color.secondary.opacity(0.3)).frame(width:7,height:7)}}.accessibilityLabel("Selected player connection")
    }.padding(14).background(.quaternary,in:RoundedRectangle(cornerRadius:14))
   }
  }.onAppear {model.refreshInput()}.disabled(model.playing != nil)
 }
 func shortLabel(_ value:String) -> String {value.replacingOccurrences(of:"sdl · button ",with:"Button ").replacingOccurrences(of:"sdl · axis ",with:"Axis ").replacingOccurrences(of:"sdl · axes ",with:"Axes ")}
 func capture(_ target:MappingTarget,_ label:String) -> some View {
  Button(model.mappingTarget==target ? "Listening…":shortLabel(label)) {model.startMapping(target)}
   .font(.caption.monospaced()).buttonStyle(.bordered).lineLimit(1).frame(maxWidth:.infinity).disabled(model.mappingTarget != nil)
   .contextMenu {if target.part.isEmpty || model.input?.analogs[target.index].axis != true {Button(target.part.isEmpty ? "Clear Binding":"Clear Direction") {model.clearMapping(target)}}}
 }
 func buttonBinding(_ index:Int) -> some View {
  let b=model.input!.buttons[index]
  return VStack(spacing:4) {Text(b.name).font(.caption).foregroundStyle(.secondary); capture(MappingTarget(kind:"button",index:index,part:"",name:b.name),b.value)}
 }
 func buttonPanel(_ title:String,_ indices:[Int],motion:Bool=false) -> some View {
  VStack(spacing:10) {
   Text(title).font(.caption.weight(.semibold)).frame(maxWidth:.infinity,alignment:.leading)
   LazyVGrid(columns:[GridItem(.flexible()),GridItem(.flexible())],spacing:10) {
    if motion {ForEach(model.input?.motions ?? []) {b in capture(MappingTarget(kind:"motion",index:b.index,part:"",name:b.name),b.value).help(b.name)}}
    else {ForEach(indices,id:\.self) {buttonBinding($0)}}
   }
  }.padding(12).background(.quaternary,in:RoundedRectangle(cornerRadius:14))
 }
 func stickPanel(_ b:InputBinding) -> some View {
  VStack(spacing:10) {
   Text(b.name).font(.caption.weight(.semibold)).frame(maxWidth:.infinity,alignment:.leading)
   capture(MappingTarget(kind:"analog",index:b.index,part:"",name:b.name),b.value)
   LazyVGrid(columns:[GridItem(.flexible()),GridItem(.flexible())],spacing:8) {
    ForEach((b.directions ?? []).filter {$0.id != "modifier"}) {d in
     VStack(spacing:3) {Text(d.id.capitalized).font(.caption2).foregroundStyle(.secondary); capture(MappingTarget(kind:"analog",index:b.index,part:d.id,name:"\(b.name) \(d.id)"),d.value)}
    }
   }
   buttonBinding(b.index+4)
   if b.axis==true {
    HStack {Text("Range").font(.caption); Spacer(); Text("\(Int((b.range ?? 1)*100))%").font(.caption.monospacedDigit())}
    Slider(value:Binding(get:{model.input?.analogs[b.index].range ?? 1},set:{model.input?.analogs[b.index].range=$0}),in:0.5...1.5,onEditingChanged:{if !$0 {model.stickCalibration(b.index,model.input?.analogs[b.index].deadzone ?? 0.15,model.input?.analogs[b.index].range ?? 1)}})
    HStack {Text("Deadzone").font(.caption); Spacer(); Text("\(Int((b.deadzone ?? 0.15)*100))%").font(.caption.monospacedDigit())}
    Slider(value:Binding(get:{model.input?.analogs[b.index].deadzone ?? 0.15},set:{model.input?.analogs[b.index].deadzone=$0}),in:0...0.95,onEditingChanged:{if !$0 {model.stickCalibration(b.index,model.input?.analogs[b.index].deadzone ?? 0.15,model.input?.analogs[b.index].range ?? 1)}})
   } else if let d=b.directions?.first(where:{$0.id=="modifier"}) {
    Text("Modifier").font(.caption2).foregroundStyle(.secondary)
    capture(MappingTarget(kind:"analog",index:b.index,part:d.id,name:"\(b.name) modifier"),d.value)
   }
  }.padding(12).background(.quaternary,in:RoundedRectangle(cornerRadius:14))
 }
 var controllerDiagram:some View {
  GeometryReader {g in
   ZStack {
    ControllerSilhouette().fill(.secondary.opacity(0.13)).overlay(ControllerSilhouette().stroke(.secondary.opacity(0.25),lineWidth:1))
    ForEach(Array(diagramButtons.enumerated()),id:\.offset) {_,item in
     Button {model.startMapping(MappingTarget(kind:"button",index:item.0,part:"",name:model.input!.buttons[item.0].name))} label:{Text(item.1).font(.system(size:13,weight:.semibold)).frame(width:28,height:28).background(.background.opacity(0.7),in:Circle())}.buttonStyle(.plain).accessibilityLabel(model.input!.buttons[item.0].name).help(model.input!.buttons[item.0].name).position(x:g.size.width*item.2,y:g.size.height*item.3).disabled(model.mappingTarget != nil)
    }
    Circle().fill(.secondary.opacity(0.2)).frame(width:48,height:48).overlay(Circle().stroke(.secondary,lineWidth:1)).position(x:g.size.width*0.25,y:g.size.height*0.33)
    Circle().fill(.secondary.opacity(0.2)).frame(width:48,height:48).overlay(Circle().stroke(.secondary,lineWidth:1)).position(x:g.size.width*0.65,y:g.size.height*0.58)
   }
  }.accessibilityElement(children:.contain).accessibilityLabel("Controller diagram")
 }
 let diagramButtons:[(Int,String,Double,Double)]=[(2,"X",0.76,0.25),(3,"Y",0.65,0.35),(0,"A",0.87,0.35),(1,"B",0.76,0.45),(11,"−",0.42,0.24),(10,"+",0.58,0.24),(13,"↑",0.36,0.47),(12,"←",0.26,0.57),(14,"→",0.46,0.57),(15,"↓",0.36,0.67)]
}

struct FriendAvatar:View {
 let friend:Friend
 var body:some View {
  ZStack {
   if let image=NSImage(contentsOfFile:friend.icon) {Image(nsImage:image).resizable().scaledToFill()}
   else {Circle().fill(.tint.opacity(0.15)); Text(String(friend.name.prefix(1)).uppercased()).font(.title3.weight(.semibold)).foregroundStyle(.tint)}
  }.frame(width:46,height:46).clipShape(Circle())
   .overlay(alignment:.bottomTrailing) {Circle().fill(friend.state.uppercased()=="ONLINE" || friend.state.uppercased()=="PLAYING" ? Color.green:Color.gray).frame(width:11,height:11).overlay(Circle().stroke(.background,lineWidth:2))}
   .accessibilityHidden(true)
 }
}
struct FriendsView: View {
 @ObservedObject var model:AppModel
 var busy:Bool {model.friendsLoading || model.friendActionBusy}
 var body:some View {
  VStack(alignment:.leading,spacing:18) {
   HStack {
    VStack(alignment:.leading,spacing:4) {
     Text("GRID0+").font(.title.weight(.semibold))
     if let me=model.gridAccount {Text("\(me.name) · \(me.code)").font(.caption).foregroundStyle(.secondary).textSelection(.enabled)}
    }
    Spacer()
    GlassIconButton(title:"Account & Server",symbol:"person.crop.circle",action:model.showAccount)
    GlassIconButton(title:"Refresh",symbol:"arrow.clockwise",action:model.refreshFriends).disabled(busy)
   }
   HStack {
    GlassSwitcher(selection:$model.friendsTab,options:["Friends","Requests"],symbols:["person.2","tray"],label:"GRID0+ tabs")
    Text(model.friendsTab=="Friends" ? "Friends · \(model.friends.count)":"Requests · \(model.incoming.count+model.outgoing.count)").font(.headline)
    Spacer()
    if busy {ProgressView().controlSize(.small)}
   }
   if let error=model.friendsError {Text(error).font(.callout).foregroundStyle(.secondary)}
   if let notice=model.friendNotice {Label(notice,systemImage:"checkmark.circle").font(.callout).foregroundStyle(.green)}
   if model.friendsTab=="Friends" {
    if model.friends.isEmpty && !busy {ContentUnavailableView("Your friends belong here",systemImage:"person.2",description:Text("Send a request using a friend code below."))}
    else {List(model.friends) {friend in
     HStack(spacing:14) {
      FriendAvatar(friend:friend)
      VStack(alignment:.leading,spacing:4) {Text(friend.name).font(.headline); Text(friend.code).font(.caption).foregroundStyle(.secondary)}
      Spacer(); Text(friend.state.isEmpty ? "Hidden":friend.state.capitalized).font(.caption).foregroundStyle(.secondary)
     }.padding(.vertical,7)
    }.listStyle(.plain)}
   } else {
    if model.incoming.isEmpty && model.outgoing.isEmpty && !busy {ContentUnavailableView("No friend requests",systemImage:"tray",description:Text("Incoming and sent requests will appear here."))}
    else {List {
     if !model.incoming.isEmpty {Section("Incoming") {ForEach(model.incoming) {request in requestRow(request,incoming:true)}}}
     if !model.outgoing.isEmpty {Section("Sent") {ForEach(model.outgoing) {request in requestRow(request,incoming:false)}}}
    }.listStyle(.plain)}
   }
   HStack(spacing:12) {
    Image(systemName:"person.badge.plus").foregroundStyle(.tint)
    TextField("Friend code · 0000-0000-0000",text:$model.friendCode).textFieldStyle(.roundedBorder).onSubmit(send)
    GlassIconButton(title:"Send Friend Request",symbol:"paperplane.fill",action:send).disabled(busy || model.friendCode.filter(\.isNumber).count != 12)
   }.padding(12).background(.quaternary,in:RoundedRectangle(cornerRadius:16))
  }.padding(24).frame(minWidth:560,minHeight:430)
 }
 func send() {
  let digits=model.friendCode.filter(\.isNumber)
  guard digits.count==12 else {return}
  let chars=Array(digits); let code=String(chars[0..<4])+"-"+String(chars[4..<8])+"-"+String(chars[8..<12])
  model.performFriendAction(code,"send")
 }
 func requestRow(_ request:FriendRequest,incoming:Bool) -> some View {
  HStack(spacing:12) {
   FriendAvatar(friend:request.other)
   VStack(alignment:.leading,spacing:4) {Text(request.other.name).font(.headline); Text(request.other.code).font(.caption).foregroundStyle(.secondary)}
   Spacer()
   if incoming {
    GlassIconButton(title:"Accept Request",symbol:"checkmark",action:{model.performFriendAction(request.id,"accept")})
    GlassIconButton(title:"Decline Request",symbol:"xmark",action:{model.performFriendAction(request.id,"deny")})
   } else {GlassIconButton(title:"Cancel Request",symbol:"xmark",action:{model.performFriendAction(request.id,"cancel")})}
  }.padding(.vertical,7).disabled(busy)
 }
}
final class SurfaceView:NSView {
 var model:AppModel?
 var lastDrawableSize=CGSize.zero
 override var acceptsFirstResponder:Bool {true}
 override func makeBackingLayer() -> CALayer { let layer=CAMetalLayer(); layer.device=MTLCreateSystemDefaultDevice(); layer.pixelFormat = .bgra8Unorm; return layer }
 override func layout() {
  super.layout()
  guard let layer=layer as? CAMetalLayer else {return}
  let scale=window?.backingScaleFactor ?? 2
  let size=CGSize(width:bounds.width*scale,height:bounds.height*scale)
  guard size.width>0, size.height>0, size != lastDrawableSize else {return}
  lastDrawableSize=size
  layer.contentsScale=scale; layer.drawableSize=size
  setSurface(Unmanaged.passUnretained(layer).toOpaque(),Int32(layer.drawableSize.width),Int32(layer.drawableSize.height),Float(scale))
  model?.surfaceReady()
 }
 override func viewDidMoveToWindow() {super.viewDidMoveToWindow(); window?.acceptsMouseMovedEvents=true; model?.gameWindow=window; window?.makeFirstResponder(self); needsLayout=true}
 override func keyDown(with event:NSEvent) {if !event.isARepeat {keyboard(nativeKey(event),true)}}
 override func keyUp(with event:NSEvent) {keyboard(nativeKey(event),false)}
 override func flagsChanged(with event:NSEvent) {
  for (flag,code) in [(NSEvent.ModifierFlags.shift,Int32(0x01000020)),(.control,0x01000021),(.option,0x01000023)] {
   keyboard(code,event.modifierFlags.contains(flag))
  }
 }
 override func resignFirstResponder() -> Bool {releaseKeys(); return super.resignFirstResponder()}

}
struct GameSurface:NSViewRepresentable {
 let model:AppModel
 func makeNSView(context:Context) -> SurfaceView {let view=SurfaceView(); view.model=model; view.wantsLayer=true; return view}
 func updateNSView(_ view:SurfaceView,context:Context) {view.needsLayout=true}
}
final class AppDelegate:NSObject,NSApplicationDelegate {
 func applicationShouldTerminate(_ sender:NSApplication) -> NSApplication.TerminateReply {
  AppModel.shared.inputTimer?.invalidate()
  AppModel.shared.cancelMapping()
  operationCancel()
  if let monitor=AppModel.shared.eventMonitor {NSEvent.removeMonitor(monitor)}
  backend.async {shutdownCore(); DispatchQueue.main.async {sender.reply(toApplicationShouldTerminate:true)}}
  return .terminateLater
 }
}
@main struct CitrosisApp:App {
 @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
 @StateObject var model=AppModel.shared
 var body:some Scene {
  Window("Citrosis",id:"library") {LibraryView(model:model)}.defaultSize(width:1120,height:760)
   .commands {
    CommandGroup(replacing:.appSettings) {Button("Settings…",action:model.showSettings).keyboardShortcut(",",modifiers:.command)}
    CommandGroup(replacing:.newItem) {
     Button("Open Game…",action:model.open).keyboardShortcut("o").disabled(model.playing != nil || model.contentBusy)
     Button("Install Files to NAND…",action:model.installContent).disabled(model.playing != nil || model.contentBusy)
     Button("Add Game Folder…",action:model.addFolder).keyboardShortcut("o",modifiers:[.command,.shift])
     Button("Refresh Library",action:model.scan).keyboardShortcut("r").disabled(model.playing != nil || model.contentBusy)
    }
    CommandMenu("Emulation") {
     Button(model.paused ? "Resume":"Pause",action:model.pause).keyboardShortcut("p").disabled(model.playing == nil || model.launching)
     Button("Stop",action:model.stop).disabled(model.playing == nil)
    }
    CommandMenu("Tools") {
     Button("Open Citrosis Data Folder…") {model.reveal("data")}
     Button("Open Keys Folder…") {model.reveal("keys")}
     Button("Open Shader Folder…") {model.reveal("shaders")}
     Button("Open Mods Folder…") {model.reveal("mods")}
     Button("Open Log Folder…") {model.reveal("log")}
    }
    CommandMenu("GRID0+") {Button("Friends…",action:model.showFriends); Button("Account & Server…",action:model.showAccount)}
   }
 }
}
