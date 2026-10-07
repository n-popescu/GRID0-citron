// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cmath>
#include <atomic>
#include <set>
#include "frontend_common/content_manager.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/romfs.h"
#include <nlohmann/json.hpp>
#include "ios/native.h"
#include "frontend_common/config.h"
#include "common/fs/path_util.h"
#include "common/settings_common.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "common/param_package.h"
#include "core/internal_network/network_interface.h"
#include "core/hle/service/acc/profile_manager.h"
#include "common/string_util.h"
#include "core/hle/service/friend/grid0_friends.h"
#include "core/hle/service/sockets/private_server.h"
#include "core/hle/service/acc/switchnet_account.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"

namespace {
using Json = nlohmann::json;
class NativeConfig final : public Config {
public:
 NativeConfig(std::string name="citrosis-native", ConfigType type=ConfigType::GlobalConfig) : Config(type) {
  Initialize(name); if(type!=ConfigType::InputProfile) ReadBindings();
  if(type==ConfigType::GlobalConfig) ReadNetworkLogging();
 }
 void ReadProfile(size_t index) {
  BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));
  auto& p=Settings::values.players.GetValue()[index];
  p.controller_type=static_cast<Settings::ControllerType>(std::clamp<s64>(ReadIntegerSetting("type",static_cast<int>(p.controller_type)),0,12));
  for(size_t i=0;i<p.buttons.size();++i) p.buttons[i]=ReadStringSetting(Settings::NativeButton::mapping[i],p.buttons[i]);
  for(size_t i=0;i<p.analogs.size();++i) p.analogs[i]=ReadStringSetting(Settings::NativeAnalog::mapping[i],p.analogs[i]);
  for(size_t i=0;i<p.motions.size();++i) p.motions[i]=ReadStringSetting(Settings::NativeMotion::mapping[i],p.motions[i]);
  EndGroup();
 }
 void SaveProfile(size_t index) {
  BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));
  SavePlayerValues(index);
  auto& p=Settings::values.players.GetValue()[index];
  const auto write=[&](const std::string& key,const std::string& value) {WriteStringSetting(key,value); WriteBooleanSetting(key+"\\default",false);};
  for(size_t i=0;i<p.buttons.size();++i) write(Settings::NativeButton::mapping[i],p.buttons[i]);
  for(size_t i=0;i<p.analogs.size();++i) write(Settings::NativeAnalog::mapping[i],p.analogs[i]);
  for(size_t i=0;i<p.motions.size();++i) write(Settings::NativeMotion::mapping[i],p.motions[i]);
  EndGroup(); WriteToIni();
 }
 void ReadBindings() {
  BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));
  constexpr std::array<int,Settings::NativeButton::NumButtons> keys = {'C','X','V','Z','F','G','Q','E','R','T','M','N',0x01000012,0x01000013,0x01000014,0x01000015,'Q','E',0,0,'Q','E'};
  constexpr std::array<std::array<int,4>,2> sticks = {{{'W','S','A','D'},{'I','K','J','L'}}};
  for(size_t p=0;p<Settings::values.players.GetValue().size();++p) {
   auto& player=Settings::values.players.GetValue()[p];
   if(IsCustomConfig() && player.profile_name.empty()) continue;
   auto prefix=fmt::format("player_{}_",p);
   for(size_t i=0;i<player.buttons.size();++i)
    player.buttons[i]=ReadStringSetting(prefix+Settings::NativeButton::mapping[i],InputCommon::GenerateKeyboardParam(keys[i]));
   for(size_t i=0;i<player.analogs.size();++i)
    player.analogs[i]=ReadStringSetting(prefix+Settings::NativeAnalog::mapping[i],InputCommon::GenerateAnalogParamFromKeys(sticks[i][0],sticks[i][1],sticks[i][2],sticks[i][3],i==0 ? 0x01000020 : 0,0.5f));
   for(size_t i=0;i<player.motions.size();++i)
    player.motions[i]=ReadStringSetting(prefix+Settings::NativeMotion::mapping[i],InputCommon::GenerateKeyboardParam('7'+i));
  }
  EndGroup();
 }
 bool network_logging{};
 void ReadNetworkLogging() {
  BeginGroup("NetworkDiagnostics");
  network_logging=ReadBooleanSetting("enabled",false);
  EndGroup();
 }
 void ReloadAllValues() override { Reload(); ReadBindings(); ReadNetworkLogging(); }
 void SaveAllValues() override {
  SaveValues();
  BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));
  for(size_t p=0;p<Settings::values.players.GetValue().size();++p) {
   const auto& player=Settings::values.players.GetValue()[p];
   const auto prefix=fmt::format("player_{}_",p);
   const auto write=[&](const std::string& key,const std::string& value) {
    WriteStringSetting(prefix+key,value);
    WriteBooleanSetting(prefix+key+"\\default",false);
   };
   for(size_t i=0;i<player.buttons.size();++i) write(Settings::NativeButton::mapping[i],player.buttons[i]);
   for(size_t i=0;i<player.analogs.size();++i) write(Settings::NativeAnalog::mapping[i],player.analogs[i]);
   for(size_t i=0;i<player.motions.size();++i) write(Settings::NativeMotion::mapping[i],player.motions[i]);
  }
  EndGroup();
  BeginGroup("NetworkDiagnostics");
  WriteBooleanSetting("enabled",network_logging);
  WriteBooleanSetting("enabled\\default",false);
  EndGroup(); WriteToIni();
 }
#define EMPTY(name) void name() override {}
 EMPTY(ReadHidbusValues) EMPTY(ReadDebugControlValues) EMPTY(ReadPathValues)
 EMPTY(ReadShortcutValues) EMPTY(ReadUIValues) EMPTY(ReadUIGamelistValues)
 EMPTY(ReadUILayoutValues) EMPTY(ReadMultiplayerValues) EMPTY(SaveHidbusValues)
 EMPTY(SaveDebugControlValues) EMPTY(SavePathValues) EMPTY(SaveShortcutValues)
 EMPTY(SaveUIValues) EMPTY(SaveUIGamelistValues) EMPTY(SaveUILayoutValues) EMPTY(SaveMultiplayerValues)
#undef EMPTY
 std::vector<Settings::BasicSetting*>& FindRelevantList(Settings::Category c) override {
  return Settings::values.linkage.by_category[c];
 }
};
std::unique_ptr<NativeConfig> config;
std::mutex config_mutex;
void ApplyNativeLogFilter() {
 Common::Log::Filter filter;
 auto effective=Settings::values.log_filter.GetValue();
 const bool diagnostic=config && config->network_logging;
 if(diagnostic) effective+=" Network:Debug Service.ACC:Debug Service.SSL:Debug Service.NIFM:Debug Service.BCAT:Debug";
 filter.ParseFilterString(effective);
 Service::Sockets::SetPrivateServerTraceEnabled(diagnostic);
 Common::Log::SetGlobalFilter(filter);
}
std::atomic<bool> operation_cancel{false};
std::atomic<double> operation_progress{0};
thread_local std::string output;
const char* Reply(const Json& json) { output = json.dump(); return output.c_str(); }
template <typename T> Json Choices() {
 Json result = Json::array();
 for (auto& [name, value] : Settings::EnumMetadata<T>::Canonicalizations())
  result.push_back({{"name",name},{"value",std::to_string(static_cast<int>(value))}});
 return result;
}
std::string SettingSection(Settings::Category category) {
 using C=Settings::Category;
 switch(category) {
 case C::CpuDebug: case C::CpuUnsafe:category=C::Cpu; break;
 case C::RendererAdvanced: case C::RendererDebug:category=C::Renderer; break;
 case C::SystemAudio:category=C::System; break;
 case C::UiAudio:category=C::Audio; break;
 case C::DebuggingGraphics:category=C::Debugging; break;
 case C::Network:category=C::Services; break;
 default:break;
 }
 std::string result=Settings::TranslateCategory(category); boost::replace_all(result," ","%20"); return result;
}
std::string BindingLabel(const std::string& serialized) {
 Common::ParamPackage p{serialized};
 const auto engine=p.Get("engine","");
 if(engine.empty()) return "Unbound";
 if(engine=="keyboard") {
  const auto code=p.Get("code",0);
  if(code>=33 && code<=126) return std::string(1,static_cast<char>(code));
  switch(code) {
   case 32:return "Space"; case 0x01000020:return "Shift"; case 0x01000021:return "Control";
   case 0x01000023:return "Option"; case 0x01000012:return "←"; case 0x01000013:return "↑";
   case 0x01000014:return "→"; case 0x01000015:return "↓"; case 0x01000004:return "Return";
   case 0:return "Unbound"; default:return fmt::format("Key {}",code);
  }
 }
 if(engine=="analog_from_button") return "Directional buttons";
 if(p.Has("axis_x") && p.Has("axis_y")) return fmt::format("{} · axes {} / {}",engine,p.Get("axis_x",0),p.Get("axis_y",0));
 if(p.Has("button")) return fmt::format("{} · button {}",engine,p.Get("button",0));
 if(p.Has("axis")) return fmt::format("{} · axis {}",engine,p.Get("axis",0));
 if(p.Has("hat")) return fmt::format("{} · {}",engine,p.Get("direction","hat"));
 return engine+" · motion";
}
std::string* Binding(int player,const std::string& kind,int index) {
 auto& players=Settings::values.players.GetValue();
 if(player<0 || static_cast<size_t>(player)>=players.size() || index<0) return nullptr;
 auto& p=players[player];
 if(kind=="button" && static_cast<size_t>(index)<p.buttons.size()) return &p.buttons[index];
 if(kind=="analog" && static_cast<size_t>(index)<p.analogs.size()) return &p.analogs[index];
 if(kind=="motion" && static_cast<size_t>(index)<p.motions.size()) return &p.motions[index];
 return nullptr;
}

}
extern "C" {
void citrosis_initialize(const char* directory) {
 IOS::EmulationSession::GetInstance().Initialize(directory);
 auto root = Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir);
 auto native = root / "citrosis-native.ini";
 if (!std::filesystem::exists(native) && std::filesystem::exists(root / "qt-config.ini"))
  std::filesystem::copy_file(root / "qt-config.ini",native);
 config = std::make_unique<NativeConfig>();
 ApplyNativeLogFilter();
 auto& system=IOS::EmulationSession::GetInstance().System();
 system.Initialize();
 system.GetFileSystemController().InitializeContentSystem(*system.GetFilesystem());
}
const char* citrosis_directories() {
 CSimpleIniA ini; ini.LoadFile((Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"qt-config.ini").c_str());
 Json result = Json::array();
 CSimpleIniA::TNamesDepend keys;
 ini.GetAllKeys("UI",keys);
 for(const auto& key:keys) {
  const std::string name=key.pItem;
  if(name.find("gamedirs")==std::string::npos || !name.ends_with("path")) continue;
  auto path = ini.GetValue("UI",key.pItem,"");
  if(std::filesystem::is_directory(path)) result.push_back(path);
 }
 return Reply(result);
}
const char* citrosis_game(const char* path) {
 try {
  auto& system = IOS::EmulationSession::GetInstance().System();
  auto loader = Loader::GetLoader(system,system.GetFilesystem()->OpenFile(path,FileSys::OpenMode::Read));
  if(!loader) return Reply(nullptr);
  std::string title; u64 id{}; std::vector<u8> icon;
  loader->ReadTitle(title); loader->ReadProgramId(id); loader->ReadIcon(icon);
  if(!id) return Reply(nullptr);
  auto iconPath = Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir)/"native-icons";
  std::filesystem::create_directories(iconPath);
  iconPath /= fmt::format("{:016X}.jpg",id);
  if(!icon.empty()) {std::ofstream file(iconPath,std::ios::binary); file.write(reinterpret_cast<const char*>(icon.data()),icon.size());}
  return Reply({{"id",fmt::format("{:016X}",id)},{"path",path},
   {"title",title.empty()?std::filesystem::path(path).stem().string():title},
   {"icon",icon.empty()?"":iconPath.string()}, {"size",std::filesystem::file_size(path)}});
 } catch(const std::exception& e) { return Reply({{"error",e.what()}}); }
}
// Run content operations on the frontend's serial backend queue.
const char* citrosis_game_folder(const char* path,const char* kind) {
 try {
  auto& system=IOS::EmulationSession::GetInstance().System();
  auto loader=Loader::GetLoader(system,system.GetFilesystem()->OpenFile(path,FileSys::OpenMode::Read));
  u64 id{}; if(!loader || loader->ReadProgramId(id)!=Loader::ResultStatus::Success) return Reply({{"error","Could not read the title ID."}});
  std::filesystem::path folder;
  const std::string type=kind;
  if(type=="mods") folder=Common::FS::GetCitronPath(Common::FS::CitronPath::LoadDir)/fmt::format("{:016X}",id);
  else if(type=="shaders") folder=Common::FS::GetCitronPath(Common::FS::CitronPath::ShaderDir)/fmt::format("{:016x}",id);
  else if(type=="dump") folder=Common::FS::GetCitronPath(Common::FS::CitronPath::DumpDir)/fmt::format("{:016X}",id);
  else if(type=="save") {
   if(auto it=Settings::values.mirrored_save_paths.find(id);it!=Settings::values.mirrored_save_paths.end() && std::filesystem::is_directory(it->second)) folder=it->second;
   else if(auto it=Settings::values.custom_save_paths.find(id);it!=Settings::values.custom_save_paths.end() && std::filesystem::is_directory(it->second)) folder=it->second;
   else {
    auto nand=Common::FS::GetCitronPath(Common::FS::CitronPath::NANDDir);
    if(Settings::values.global_custom_save_path_enabled.GetValue() && std::filesystem::is_directory(Settings::values.global_custom_save_path.GetValue())) nand=Settings::values.global_custom_save_path.GetValue();
    std::filesystem::create_directories(nand);
    auto dir=system.GetFilesystem()->OpenDirectory(nand.string(),FileSys::OpenMode::Read);
    if(!dir) return Reply({{"error","The NAND directory could not be opened."}});
    Service::Account::ProfileManager profiles;
    auto user=profiles.GetUser(Settings::values.current_user.GetValue());
    FileSys::NACP control; loader->ReadControlData(control);
    const bool device=control.GetDefaultNormalSaveSize()==0 && control.GetDeviceSaveDataSize()>0;
    if(!device && !user) return Reply({{"error","Select a valid player profile in System settings first."}});
    auto relative=FileSys::SaveDataFactory::GetFullPath({},dir,FileSys::SaveDataSpaceId::User,FileSys::SaveDataType::Account,id,device?u128{}:user->AsU128(),0);
    folder=Common::FS::ConcatPathSafe(nand,relative);
   }
  } else return Reply({{"error","Unknown folder."}});
  std::filesystem::create_directories(folder);
  return Reply({{"path",folder.string()}});
 } catch(const std::exception& e) {return Reply({{"error",e.what()}});}
}
void citrosis_operation_cancel() {operation_cancel=true;}
double citrosis_operation_progress() {return operation_progress.load();}
void citrosis_operation_prepare() {operation_cancel=false; operation_progress=0;}
const char* citrosis_content_operation(const char* path,const char* action,const char* destination) {
 try {
  auto& session=IOS::EmulationSession::GetInstance();
  if(session.IsRunning()) return Reply({{"error","Stop emulation before managing content."}});
  if(operation_cancel) return Reply({{"error","Operation cancelled."}});
  auto& system=session.System(); auto vfs=system.GetFilesystem();
  const std::string command=action;
  auto callback=[](size_t total,size_t done) {operation_progress=total?static_cast<double>(done)/total:0; return operation_cancel.load();};
  if(command=="install") {
   ContentManager::InstallResult result;
   auto extension=Common::ToLower(std::filesystem::path(path).extension().string());
   if(extension==".nsp") result=ContentManager::InstallNSP(system,*vfs,path,callback);
   else if(extension==".nca") {
    FileSys::NCA nca{vfs->OpenFile(path,FileSys::OpenMode::Read)};
    const auto id=nca.GetTitleId();
    const auto type=(id&0x800)?FileSys::TitleType::Update:FileSys::TitleType::AOC;
    if((id&0xFFF)==0) return Reply({{"error","Choose an update or DLC NCA, not a base game."}});
    result=ContentManager::InstallNCA(*vfs,path,*system.GetFileSystemController().GetUserNANDContents(),type,callback);
   }
   else return Reply({{"error","Choose an NSP or NCA update/DLC package."}});
   if(operation_cancel) return Reply({{"error","Installation cancelled."}});
   if(result==ContentManager::InstallResult::BaseInstallAttempted) return Reply({{"error","Base games should be opened from the library. Install only updates and DLC to NAND."}});
   if(result==ContentManager::InstallResult::Failure) return Reply({{"error","Installation failed. Check that the package and decryption keys are valid."}});
   return Reply({{"message",result==ContentManager::InstallResult::Overwrite?"Installed content replaced successfully.":"Content installed successfully."}});
  }
  session.PrepareContent(path);
  auto file=vfs->OpenFile(path,FileSys::OpenMode::Read); auto loader=Loader::GetLoader(system,file);
  u64 id{}; if(!loader || loader->ReadProgramId(id)!=Loader::ResultStatus::Success) return Reply({{"error","Could not read this game."}});
  if(command=="verify") {
   auto result=ContentManager::VerifyGameContents(system,path,callback);
   if(operation_cancel) return Reply({{"error","Verification cancelled."}});
   return Reply({{"message",result==ContentManager::GameVerificationResult::Success?"Integrity check passed.":result==ContentManager::GameVerificationResult::NotImplemented?"This file format does not support integrity verification.":"Integrity verification failed."}});
  }
  if(command=="remove_update") return Reply({{"message",ContentManager::RemoveUpdate(system.GetFileSystemController(),id)?"Installed update removed.":"No installed update was found."}});
  if(command=="remove_dlc") return Reply({{"message",fmt::format("Removed {} installed DLC packages.",ContentManager::RemoveAllDLC(system,id))}});
  std::shared_ptr<FileSys::NCA> nca;
  FileSys::VirtualDir exefs;
  const auto extension=Common::ToLower(std::filesystem::path(path).extension().string());
  if(extension==".nsp") {FileSys::NSP package{file}; nca=package.GetNCA(id,FileSys::ContentRecordType::Program); exefs=package.GetExeFS();}
  else if(extension==".xci") {FileSys::XCI cartridge{file}; nca=cartridge.GetNCAByType(FileSys::NCAContentType::Program);}
  else if(extension==".nca") nca=std::make_shared<FileSys::NCA>(file);
  FileSys::VirtualDir source;
  FileSys::PatchManager pm{id,system.GetFileSystemController(),system.GetContentProvider()};
  if(command=="romfs" || command=="skeleton") {
   FileSys::VirtualFile romfs,packed; loader->ReadUpdateRaw(packed);
   if(nca) romfs=pm.PatchRomFS(nca.get(),nca->GetRomFS(),FileSys::ContentRecordType::Program,packed,false);
   else loader->ReadRomFS(romfs);
   if(romfs) source=FileSys::ExtractRomFS(romfs);
  } else if(command=="exefs") {
   if(nca) {
    exefs=nca->GetExeFS();
    FileSys::VirtualFile packed; loader->ReadUpdateRaw(packed);
    if(packed) {auto update=std::make_shared<FileSys::NCA>(packed,nca.get()); if(update->GetExeFS()) exefs=update->GetExeFS();}
   }
   source=pm.PatchExeFS(exefs);
  } else return Reply({{"error","Unknown content operation."}});
  if(!source) return Reply({{"error","This game’s filesystem could not be decrypted or extracted."}});
  const auto output=std::filesystem::path(destination)/fmt::format("{:016X}",id)/(command=="exefs"?"exefs":"romfs");
  if(std::filesystem::exists(output)) return Reply({{"error","The destination already exists. Choose a different folder to preserve the existing dump."}});
  size_t total=0,done=0;
  std::function<void(const FileSys::VirtualDir&)> count=[&](const auto& dir){for(auto& f:dir->GetFiles()) total+=f->GetSize(); for(auto& d:dir->GetSubdirectories()) count(d);}; count(source);
  if(command!="skeleton" && std::filesystem::space(destination).available<total) return Reply({{"error","Not enough free space for this dump."}});
  std::function<bool(const FileSys::VirtualDir&,const std::filesystem::path&)> copy=[&](const auto& dir,const auto& target){
   if(operation_cancel) return false;
   std::filesystem::create_directories(target);
   for(const auto& f:dir->GetFiles()) {
    const auto name=f->GetName(); if(name.empty() || name=="." || name==".." || name.find('/')!=std::string::npos) return false;
    if(command=="skeleton") continue;
    std::ofstream out(target/name,std::ios::binary); if(!out) return false;
    std::vector<u8> buffer(1024*1024);
    for(size_t offset=0;offset<f->GetSize();) {
     if(operation_cancel) return false;
     const auto read=f->Read(buffer.data(),std::min(buffer.size(),f->GetSize()-offset),offset);
     if(!read) return false;
     out.write(reinterpret_cast<const char*>(buffer.data()),read); if(!out) return false;
     offset+=read; done+=read; operation_progress=total?static_cast<double>(done)/total:0;
    }
   }
   for(const auto& d:dir->GetSubdirectories()) {const auto name=d->GetName(); if(name.empty() || name=="." || name==".." || name.find('/')!=std::string::npos || !copy(d,target/name)) return false;}
   return true;
  };
  if(!copy(source,output)) {std::filesystem::remove_all(output); return Reply({{"error",operation_cancel?"Extraction cancelled.":"Extraction failed; the incomplete dump was removed."}});}
  return Reply({{"message","Extraction complete."},{"path",output.string()}});
 } catch(const std::exception& e) {return Reply({{"error",e.what()}});}
}

const char* citrosis_settings() {
 std::scoped_lock lock{config_mutex};
 Json list = Json::array();
 list.push_back({{"id","network_logging"},{"value",config->network_logging ? "true":"false"},
                 {"label","Network Logging"},{"category","Network"},{"boolean",true},
                 {"choices",Json::array()},{"runtime",false}});
 for(auto& [key,s] : Settings::values.linkage.by_key) {
  if(!s->Save()) continue;
  Json choices = Json::array();
  if(key=="cpu_backend") choices=Choices<Settings::CpuBackend>();
  if(key=="backend") choices=Choices<Settings::RendererBackend>();
  if(key=="use_docked_mode") choices=Choices<Settings::ConsoleMode>();
  if(key=="resolution_setup") choices=Choices<Settings::ResolutionSetup>();
  if(key=="vsync_mode") choices=Choices<Settings::VSyncMode>();
  if(key=="sink_id") choices=Choices<Settings::AudioEngine>();
  if(key=="network_interface") {
   choices.push_back({{"name","Automatic"},{"value",""}});
   choices.push_back({{"name","None (offline)"},{"value","None"}});
   for(const auto& iface:Network::GetAvailableNetworkInterfaces())
    choices.push_back({{"name",iface.name},{"value",iface.name}});
   const auto selected=s->ToString();
   if(!selected.empty() && std::none_of(choices.begin(),choices.end(),[&](const Json& c){return c["value"]==selected;}))
    choices.push_back({{"name",selected+" (unavailable)"},{"value",selected}});
  }
  if(key=="current_user") {
   Service::Account::ProfileManager profiles;
   for(size_t i=0;i<Service::Account::MAX_USERS;++i) {
    Service::Account::ProfileBase profile{};
    if(profiles.GetProfileBase(std::optional<size_t>{i},profile)) {
     auto end=std::find(profile.username.begin(),profile.username.end(),0);
     choices.push_back({{"name",std::string(profile.username.begin(),end)},{"value",std::to_string(i)}});
    }
   }
  }
#define ENUM_CHOICES(T) if(s->IsEnum() && s->EnumIndex()==Settings::EnumMetadata<Settings::T>::Index()) choices=Choices<Settings::T>();
  ENUM_CHOICES(Language) ENUM_CHOICES(Region) ENUM_CHOICES(TimeZone)
  ENUM_CHOICES(AnisotropyMode) ENUM_CHOICES(AstcDecodeMode) ENUM_CHOICES(AstcRecompression)
  ENUM_CHOICES(GpuAccuracy) ENUM_CHOICES(CpuAccuracy) ENUM_CHOICES(MemoryLayout)
  ENUM_CHOICES(NvdecEmulation) ENUM_CHOICES(ScalingFilter) ENUM_CHOICES(AntiAliasing)
  ENUM_CHOICES(AudioMode) ENUM_CHOICES(SpirvOptimizeMode)
#undef ENUM_CHOICES
  list.push_back({{"id",key},{"value",s->ToString()},{"label",s->Canonicalize()},
   {"category",Settings::TranslateCategory(s->GetCategory())},
   {"boolean",s->TypeId()=="bool"},{"choices",choices},
   {"runtime",s->RuntimeModifiable()}});
 }
 return Reply(list);
}
const char* citrosis_game_settings(const char* title) {
 auto globals=Json::parse(citrosis_settings());
 std::scoped_lock lock{config_mutex};
 const auto id=std::stoull(title,nullptr,16);
 CSimpleIniA ini; const auto path=Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"custom"/(fmt::format("{:016X}",id)+".ini"); ini.LoadFile(path.c_str());
 Json result=Json::array();
 const std::set<std::string> visible={"cpu_backend","cpu_accuracy","use_multi_core","use_docked_mode","language_index","region_index","time_zone_index","backend","resolution_setup","gpu_accuracy","vsync_mode","scaling_filter","anti_aliasing","max_anisotropy","use_disk_shader_cache","use_asynchronous_shaders","nvdec_emulation","accelerate_astc","astc_recompression","aspect_ratio","volume","mute"};
 for(auto setting:globals) {
  const std::string key=setting["id"]; if(!visible.contains(key)) continue; auto found=Settings::values.linkage.by_key.find(key);
  if(found==Settings::values.linkage.by_key.end() || !found->second->Switchable() || (!setting["boolean"].get<bool>() && setting["choices"].empty())) continue;
  const auto section=SettingSection(found->second->GetCategory());
  const bool inherit=ini.GetBoolValue(section.c_str(),(key+"\\use_global").c_str(),true);
  if(!inherit) {
   std::string value=ini.GetBoolValue(section.c_str(),(key+"\\default").c_str(),true)?found->second->DefaultToString():ini.GetValue(section.c_str(),key.c_str(),found->second->DefaultToString().c_str());
   boost::replace_all(value,"\"",""); setting["value"]=value;
  }
  result.push_back({{"setting",setting},{"inherit",inherit}});
 }
 return Reply(result);
}
bool citrosis_game_setting(const char* title,const char* key,const char* value,bool inherit) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning()) return false;
 auto found=Settings::values.linkage.by_key.find(key); if(found==Settings::values.linkage.by_key.end() || !found->second->Switchable()) return false;
 auto id=std::stoull(title,nullptr,16);
 auto path=Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"custom"/(fmt::format("{:016X}",id)+".ini");
 std::filesystem::create_directories(path.parent_path()); CSimpleIniA ini; ini.LoadFile(path.c_str());
 const auto section=SettingSection(found->second->GetCategory()); const std::string name=key;
 ini.SetBoolValue(section.c_str(),(name+"\\use_global").c_str(),inherit);
 if(!inherit) {ini.SetBoolValue(section.c_str(),(name+"\\default").c_str(),false); ini.SetValue(section.c_str(),key,value);}
 return ini.SaveFile(path.c_str())>=0;
}

bool citrosis_set_setting(const char* key, const char* value) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning()) return false;
 if(std::string_view{key}=="network_logging") {
  config->network_logging=std::string_view{value}=="true";
  config->SaveAllValues(); ApplyNativeLogFilter();
  LOG_WARNING(Network,"Native network logging {}",config->network_logging ? "enabled":"disabled");
  Common::Log::Flush(); return true;
 }
 auto entry = Settings::values.linkage.by_key.find(key);
 if(entry==Settings::values.linkage.by_key.end()) return false;
 entry->second->LoadString(value); config->SaveAllValues();
 if(std::string_view{key}=="log_filter") ApplyNativeLogFilter();
 return true;
}
const char* citrosis_friends() {
 namespace Grid = Service::Friend::Grid0;
 auto me=Grid::FetchMe();
 if(!me) return Reply({{"error","Could not sign in. Check your GRID0+ account and server settings."}});
 auto serialize=[](const Grid::Friend& f, bool picture) {
  std::string icon;
  if(picture) {
   auto image=Grid::ProfileImage(f.nsa_id,std::chrono::milliseconds(3000));
   if(image && !image->empty()) {
    auto path=Common::FS::GetCitronPath(Common::FS::CitronPath::CacheDir)/"native-friends";
    std::error_code error; std::filesystem::create_directories(path,error);
    path/=fmt::format("{}.jpg",f.nsa_id);
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(image->data()),image->size());
    if(file) icon=path.string();
   }
  }
  return Json{{"id",std::to_string(f.nsa_id)},{"name",f.nickname},{"state",f.state},{"code",f.friend_code},{"icon",icon}};
 };
 Json friends=Json::array(), incoming=Json::array(), outgoing=Json::array();
 auto list=Grid::FetchNowBlocking();
 if(list) for(const auto& f:*list) friends.push_back(serialize(f,true));
 std::vector<Grid::FriendRequest> in,out;
 const bool requests_ok=Grid::FetchRequests(in,out);
 for(const auto& r:in) incoming.push_back({{"id",r.id},{"other",serialize(r.other,false)}});
 for(const auto& r:out) outgoing.push_back({{"id",r.id},{"other",serialize(r.other,false)}});
 Json result={{"friends",friends},{"incoming",incoming},{"outgoing",outgoing},
  {"me",{{"name",me->nickname},{"code",me->friend_code}}}};
 if(!list || !requests_ok) result["error"]="Some GRID0+ information could not be refreshed. Try again.";
 return Reply(result);
}
const char* citrosis_friend_action(const char* id, const char* action) {
 namespace Grid=Service::Friend::Grid0;
 std::optional<std::string> error;
 const std::string command=action;
 if(command=="send") error=Grid::SendRequest(id);
 else if(command=="accept" || command=="deny" || command=="cancel") error=Grid::SettleRequest(id,command);
 else if(command=="remove") {
  try {error=Grid::RemoveFriend(std::stoull(id));} catch(...) {error="Invalid friend ID.";}
 } else error="Unsupported friend action.";
 return Reply(error ? Json{{"error",*error}} : Json{{"success",true}});
}

int citrosis_launch(const char* path) {
 Settings::RestoreGlobalState(false);
 config->ReloadAllValues();
 auto& system=IOS::EmulationSession::GetInstance().System();
 auto loader=Loader::GetLoader(system,system.GetFilesystem()->OpenFile(path,FileSys::OpenMode::Read));
 u64 id{};
 if(loader && loader->ReadProgramId(id)==Loader::ResultStatus::Success) {
  auto name=fmt::format("{:016X}",id);
  if(std::filesystem::exists(Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"custom"/(name+".ini"))) {
   NativeConfig per_game(name,Config::ConfigType::PerGameConfig);
  }
 }
 ApplyNativeLogFilter();
 LOG_WARNING(Core_ARM, "Native launch CPU setting: {} ({}); renderer: {}",
             Settings::CanonicalizeEnum(Settings::values.cpu_backend.GetValue()),
             static_cast<int>(Settings::values.cpu_backend.GetValue()),
             Settings::CanonicalizeEnum(Settings::values.renderer_backend.GetValue()));
 const auto interface=Network::GetSelectedNetworkInterface();
 const auto redirect=Service::Sockets::PrivateServerAddress();
 LOG_WARNING(Network, "Native game network: selected={}, resolved={}, airplane mode={}, "
             "GRID0+ account configured={}, guest redirect={}, secondary NAT configured={}, custom CA={}",
             Settings::values.network_interface.GetValue().empty() ? "Automatic" : Settings::values.network_interface.GetValue(),
             interface ? interface->name : "unavailable/offline", Settings::values.airplane_mode.GetValue(),
             Service::Account::SwitchNet::IsConfigured(), redirect.empty() ? "disabled" : redirect,
             !Settings::values.private_server_nat_secondary_address.GetValue().empty(),
             !Settings::values.private_server_ca_bundle.GetValue().empty());
 if(!interface && !Settings::values.airplane_mode.GetValue() && Settings::values.network_interface.GetValue()!="None")
  LOG_WARNING(Network, "The game has no selected IPv4 interface. Choose an available adapter or Automatic in Network settings.");
 Common::Log::Flush();
 return static_cast<int>(IOS::EmulationSession::GetInstance().Launch(path,0));
}
void citrosis_stop() {
 IOS::EmulationSession::GetInstance().Stop();
 Common::Log::Flush();
 Settings::RestoreGlobalState(false);
 std::scoped_lock lock{config_mutex};
 config->ReloadAllValues();
}
double citrosis_fps() {
 auto& session=IOS::EmulationSession::GetInstance();
 if(!session.IsRunning() || !session.System().IsPoweredOn()) return 0;
 const double fps=session.System().GetAndResetPerfStats().average_game_fps;
 return std::isfinite(fps) ? std::max(0.0,fps) : 0;
}
const char* citrosis_input_profiles() {
 Json names=Json::array(); const auto directory=Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"input";
 if(std::filesystem::is_directory(directory)) for(const auto& item:std::filesystem::directory_iterator(directory))
  if(item.is_regular_file() && item.path().extension()==".ini") names.push_back(item.path().stem().string());
 std::sort(names.begin(),names.end()); return Reply(names);
}
const char* citrosis_input_profile(int player,const char* name,bool save,bool create) {
 std::scoped_lock lock{config_mutex}; const std::string profile=name;
 if(IOS::EmulationSession::GetInstance().IsRunning() || player<0 || player>=8) return Reply({{"error","Stop emulation before changing input profiles."}});
 if(profile.empty() || profile.size()>100 || profile=="." || profile==".." || profile.find_first_of("<>:;\"/\\|,.!?*\n\r")!=std::string::npos) return Reply({{"error","Use a profile name without punctuation or path separators."}});
 auto path=Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"input"/(profile+".ini");
 if(create && std::filesystem::exists(path)) return Reply({{"error","A profile with that name already exists."}});
 if(!save && !std::filesystem::exists(path)) return Reply({{"error","The input profile no longer exists."}});
 NativeConfig preset{profile,Config::ConfigType::InputProfile};
 if(save) preset.SaveProfile(player); else {preset.ReadProfile(player); config->SaveAllValues();}
 return Reply({{"message",save?"Input profile saved.":"Input profile loaded."}});
}
const char* citrosis_input(int player) {
 std::scoped_lock lock{config_mutex};
 auto& players=Settings::values.players.GetValue();
 if(player<0 || static_cast<size_t>(player)>=players.size()) return Reply(nullptr);
 const auto& p=players[player];
 Json buttons=Json::array(),analogs=Json::array(),motions=Json::array(),devices=Json::array();
 constexpr std::array<const char*,22> names={"A","B","X","Y","Left stick click","Right stick click","L","R","ZL","ZR","Plus","Minus","D-pad left","D-pad up","D-pad right","D-pad down","Left SL","Left SR","Home","Capture","Right SL","Right SR"};
 for(size_t i=0;i<p.buttons.size();++i) buttons.push_back({{"index",i},{"name",names[i]},{"value",BindingLabel(p.buttons[i])}});
 for(size_t i=0;i<p.analogs.size();++i) {
  Json directions=Json::array(); Common::ParamPackage param{p.analogs[i]};
  for(const auto* direction:{"up","down","left","right","modifier"}) {
   std::string label=BindingLabel(param.Get(direction,""));
   if(param.Has("axis_x") && std::string{direction}!="modifier") {
    const bool horizontal=std::string{direction}=="left" || std::string{direction}=="right";
    bool positive=std::string{direction}=="right" || std::string{direction}=="up";
    if(param.Get(horizontal?"invert_x":"invert_y","+")=="-") positive=!positive;
    label=fmt::format("Axis {}{}",param.Get(horizontal?"axis_x":"axis_y",0),positive?"+":"−");
   }
   directions.push_back({{"id",direction},{"value",label}});
  }
  analogs.push_back({{"index",i},{"name",i==0?"Left stick":"Right stick"},{"value",BindingLabel(p.analogs[i])},{"directions",directions},{"deadzone",param.Get("deadzone",0.15f)},{"range",param.Get("range",1.0f)},{"axis",param.Has("axis_x")}});
 }
 for(size_t i=0;i<p.motions.size();++i) motions.push_back({{"index",i},{"name",i==0?"Left motion":"Right motion"},{"value",BindingLabel(p.motions[i])}});
 for(const auto& d:IOS::EmulationSession::GetInstance().GetInputSubsystem().GetInputDevices()) {
  // The native surface currently forwards keyboard and controller events, not mouse bindings.
  if(d.Get("engine","")=="mouse") continue;
  devices.push_back({{"id",d.Serialize()},{"name",d.Get("display","Input device")}});
 }
 return Reply({{"connected",p.connected},{"style",static_cast<int>(p.controller_type)},
  {"vibration",p.vibration_enabled},{"strength",p.vibration_strength},
  {"buttons",buttons},{"analogs",analogs},{"motions",motions},{"devices",devices}});
}
bool citrosis_input_options(int player,bool connected,int style,bool vibration,int strength) {
 std::scoped_lock lock{config_mutex};
 auto& players=Settings::values.players.GetValue();
 if(IOS::EmulationSession::GetInstance().IsRunning() || player<0 || static_cast<size_t>(player)>=players.size() || style<0 || style>12 || strength<0 || strength>100) return false;
 auto& p=players[player]; p.connected=connected; p.controller_type=static_cast<Settings::ControllerType>(style);
 p.vibration_enabled=vibration; p.vibration_strength=strength; config->SaveAllValues(); return true;
}
bool citrosis_input_defaults(int player,const char* serialized) {
 std::scoped_lock lock{config_mutex};
 auto& players=Settings::values.players.GetValue();
 if(IOS::EmulationSession::GetInstance().IsRunning() || player<0 || static_cast<size_t>(player)>=players.size()) return false;
 auto& p=players[player]; auto& input=IOS::EmulationSession::GetInstance().GetInputSubsystem();
 Common::ParamPackage device{serialized};
 if(device.Get("engine","")=="mouse") return false;
 if(device.Get("engine","")=="keyboard") {
  constexpr std::array<int,22> keys={'C','X','V','Z','F','G','Q','E','R','T','M','N',0x01000012,0x01000013,0x01000014,0x01000015,'Q','E',0,0,'Q','E'};
  for(size_t i=0;i<p.buttons.size();++i) p.buttons[i]=InputCommon::GenerateKeyboardParam(keys[i]);
  p.analogs[0]=InputCommon::GenerateAnalogParamFromKeys('W','S','A','D',0x01000020,0.5f);
  p.analogs[1]=InputCommon::GenerateAnalogParamFromKeys('I','K','J','L',0,0.5f);
  for(size_t i=0;i<p.motions.size();++i) p.motions[i]=InputCommon::GenerateKeyboardParam('7'+i);
 } else {
  auto buttons=input.GetButtonMappingForDevice(device);
  auto analogs=input.GetAnalogMappingForDevice(device);
  auto motions=input.GetMotionMappingForDevice(device);
  if(buttons.empty() && analogs.empty()) return false;
  p.buttons.fill(""); p.analogs.fill(""); p.motions.fill("");
  for(const auto& [i,param]:buttons) p.buttons.at(i)=param.Serialize();
  for(const auto& [i,param]:analogs) p.analogs.at(i)=param.Serialize();
  for(const auto& [i,param]:motions) p.motions.at(i)=param.Serialize();
 }
 config->SaveAllValues(); return true;
}
void citrosis_mapping_begin(int type) {
 if(IOS::EmulationSession::GetInstance().IsRunning()) return;
 auto& input=IOS::EmulationSession::GetInstance().GetInputSubsystem();
 input.StopMapping(); input.GetKeyboard()->ReleaseAllKeys();
 input.BeginMapping(type==1?InputCommon::Polling::InputType::Stick:type==2?InputCommon::Polling::InputType::Motion:InputCommon::Polling::InputType::Button);
}
void citrosis_mapping_cancel() {
 auto& input=IOS::EmulationSession::GetInstance().GetInputSubsystem();
 input.StopMapping(); input.GetKeyboard()->ReleaseAllKeys();
}
const char* citrosis_mapping_poll(int player,const char* kind,int index,const char* component,const char* filter) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning()) return Reply(nullptr);
 auto* binding=Binding(player,kind,index); if(!binding) return Reply(nullptr);
 auto& input=IOS::EmulationSession::GetInstance().GetInputSubsystem();
 auto param=input.GetNextInput(); if(!param.Has("engine")) return Reply(nullptr);
 Common::ParamPackage device{filter};
 const auto engine=device.Get("engine","any");
 if(engine!="any" && (engine!=param.Get("engine","") ||
   (device.Has("guid") && device.Get("guid","")!=param.Get("guid","") && device.Get("guid2","")!=param.Get("guid","")) ||
   (device.Has("port") && device.Get("port",0)!=param.Get("port",0)))) return Reply(nullptr);
 const std::string part=component;
 if(std::string{kind}=="analog") {
  if(part.empty()) {
   if(!param.Has("axis_x") || !param.Has("axis_y")) return Reply(nullptr);
  } else if(!param.Has("axis_x") || !param.Has("axis_y")) {
   if(part!="up" && part!="down" && part!="left" && part!="right" && part!="modifier") return Reply(nullptr);
   Common::ParamPackage stick{*binding};
   if(stick.Get("engine","")!="analog_from_button") stick=Common::ParamPackage{{"engine","analog_from_button"}};
   stick.Set(part,param.Serialize()); stick.Set("modifier_scale",0.5f); param=stick;
  }
 }
 *binding=param.Serialize(); input.StopMapping(); input.GetKeyboard()->ReleaseAllKeys();
 config->SaveAllValues(); return Reply({{"mapped",true}});
}
bool citrosis_input_clear(int player) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning() || player<0 || player>=8) return false;
 auto& p=Settings::values.players.GetValue()[player];
 p.buttons.fill(""); p.analogs.fill(""); p.motions.fill(""); config->SaveAllValues(); return true;
}
bool citrosis_stick_options(int player,int index,double deadzone,double range) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning() || deadzone<0 || deadzone>0.95 || range<0.5 || range>1.5) return false;
 auto* binding=Binding(player,"analog",index); if(!binding) return false;
 Common::ParamPackage param{*binding}; if(!param.Has("axis_x")) return false;
 param.Set("deadzone",static_cast<float>(deadzone)); param.Set("range",static_cast<float>(range));
 *binding=param.Serialize(); config->SaveAllValues(); return true;
}
bool citrosis_mapping_clear(int player,const char* kind,int index,const char* component) {
 std::scoped_lock lock{config_mutex};
 if(IOS::EmulationSession::GetInstance().IsRunning()) return false;
 auto* binding=Binding(player,kind,index); if(!binding) return false;
 const std::string part=component;
 if(part.empty()) binding->clear();
 else {
  if(std::string{kind}!="analog" || (part!="up" && part!="down" && part!="left" && part!="right" && part!="modifier")) return false;
  Common::ParamPackage param{*binding}; if(param.Get("engine","")!="analog_from_button") return false;
  param.Set(part,""); *binding=param.Serialize();
 }
 config->SaveAllValues(); return true;
}
const char* citrosis_renderer() {
 switch(Settings::values.renderer_backend.GetValue()) {
 case Settings::RendererBackend::Metal: return "Metal";
 case Settings::RendererBackend::Vulkan: return "Vulkan · MoltenVK";
 default: return "Null renderer";
 }
}
const char* citrosis_paths() {
 Json paths;
 for(auto [name,path]:std::initializer_list<std::pair<const char*,Common::FS::CitronPath>>{
  {"data",Common::FS::CitronPath::CitronDir},{"keys",Common::FS::CitronPath::KeysDir},
  {"log",Common::FS::CitronPath::LogDir},{"shaders",Common::FS::CitronPath::ShaderDir},
  {"mods",Common::FS::CitronPath::LoadDir}}) paths[name]=Common::FS::GetCitronPathString(path);
 paths["input_profiles"]=(Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir)/"input").string();
 return Reply(paths);
}
void citrosis_keyboard(int key, bool down) {
 auto* keyboard=IOS::EmulationSession::GetInstance().GetInputSubsystem().GetKeyboard();
 if(down) keyboard->PressKey(key); else keyboard->ReleaseKey(key);
}
void citrosis_release_keys() {
 IOS::EmulationSession::GetInstance().GetInputSubsystem().GetKeyboard()->ReleaseAllKeys();
}
void citrosis_pump_input() {
 IOS::EmulationSession::GetInstance().GetInputSubsystem().PumpEvents();
}
}
