#include "SystemData.h"

#include "SystemConf.h"
#include "utils/FileSystemUtil.h"
#include "utils/ThreadPool.h"
#include "CollectionSystemManager.h"
#include "FileFilterIndex.h"
#include "FileSorts.h"
#include "Gamelist.h"
#include "Log.h"
#include "platform.h"
#include "Settings.h"
#include "ThemeData.h"
#include "views/UIModeController.h"
#include <fstream>
#include "Window.h"
#include "LocaleES.h"
#include "utils/StringUtil.h"

using namespace Utils;

std::vector<SystemData*> SystemData::sSystemVector;
std::vector<SystemData*> SystemData::sFileSystemVector; //batocera

SystemData::SystemData(const std::string& name, const std::string& fullName, SystemEnvironmentData* envData, const std::string& themeFolder, std::map<std::string, std::vector<std::string>*>* emulators, bool CollectionSystem) : // batocera
	mName(name), mFullName(fullName), mEnvData(envData), mThemeFolder(themeFolder), mIsCollectionSystem(CollectionSystem), mIsGameSystem(true)
{
	mSortId = Settings::getInstance()->getInt(getName() + ".sort"),

	mFilterIndex = nullptr;
	mEmulators = emulators; // batocera

	// if it's an actual system, initialize it, if not, just create the data structure
	if(!CollectionSystem)
	{
		mRootFolder = new FolderData(mEnvData->mStartPath, this);
		mRootFolder->metadata.set("name", mFullName);

		std::unordered_map<std::string, FileData*> fileMap;

		if (!Settings::getInstance()->getBool("ParseGamelistOnly"))
		{
			populateFolder(mRootFolder, fileMap);
			if (mRootFolder->getChildren().size() == 0)
				return;
		}

		if(!Settings::getInstance()->getBool("IgnoreGamelist"))
			parseGamelist(this, fileMap);

		if (mSortId >= 0 && mSortId < FileSorts::SortTypes.size())
			mRootFolder->sort(FileSorts::SortTypes.at(mSortId));
		else
			mRootFolder->sort(FileSorts::SortTypes.at(0));
	}
	else
	{
		// virtual systems are updated afterwards, we're just creating the data structure
		mRootFolder = new FolderData("" + name, this);
	}

	setIsGameSystemStatus();
	loadTheme();
}

SystemData::~SystemData()
{
	delete mRootFolder;

	if (mFilterIndex != nullptr)
		delete mFilterIndex;
}

void SystemData::setIsGameSystemStatus()
{
	// we exclude non-game systems from specific operations (i.e. the "RetroPie" system, at least)
	// if/when there are more in the future, maybe this can be a more complex method, with a proper list
	// but for now a simple string comparison is more performant
	mIsGameSystem = (mName != "retropie");
}

void SystemData::populateFolder(FolderData* folder, std::unordered_map<std::string, FileData*>& fileMap)
{
	const std::string& folderPath = folder->getPath();
	if(!Utils::FileSystem::isDirectory(folderPath))
	{
		LOG(LogWarning) << "Error - folder with path \"" << folderPath << "\" is not a directory!";
		return;
	}

	//make sure that this isn't a symlink to a thing we already have
	if(Utils::FileSystem::isSymlink(folderPath))
	{
		//if this symlink resolves to somewhere that's at the beginning of our path, it's gonna recurse
		if(folderPath.find(Utils::FileSystem::getCanonicalPath(folderPath)) == 0)
		{
			LOG(LogWarning) << "Skipping infinitely recursive symlink \"" << folderPath << "\"";
			return;
		}
	}

	std::string filePath;
	std::string extension;
	bool isGame;
	bool showHidden = Settings::getInstance()->getBool("ShowHiddenFiles");

	Utils::FileSystem::fileList dirContent = Utils::FileSystem::getDirectoryFiles(folderPath);
	for (auto fileInfo : dirContent)
	{
		filePath = fileInfo.path;

		// skip hidden files and folders
		if(!showHidden && fileInfo.hidden)
			continue;

		//this is a little complicated because we allow a list of extensions to be defined (delimited with a space)
		//we first get the extension of the file itself:
		extension = Utils::String::toLower(Utils::FileSystem::getExtension(filePath));

		//fyi, folders *can* also match the extension and be added as games - this is mostly just to support higan
		//see issue #75: https://github.com/Aloshi/EmulationStation/issues/75

		isGame = false;
		if(mEnvData->isValidExtension(extension))
		{
			FileData* newGame = new FileData(GAME, filePath, this);

			// preventing new arcade assets to be added
			if(!newGame->isArcadeAsset())
			{
				folder->addChild(newGame);
				fileMap[filePath] = newGame;
				isGame = true;
			}
		}

		//add directories that also do not match an extension as folders
		if(!isGame && fileInfo.directory)
		{
			// Don't loose time looking in downloaded_images, downloaded_videos & media folders
			if(filePath.rfind("downloaded_") != std::string::npos || filePath.rfind("media") != std::string::npos)
				continue;

			FolderData* newFolder = new FolderData(filePath, this);
			populateFolder(newFolder, fileMap);

			//ignore folders that do not contain games
			if(newFolder->getChildren().size() == 0)
				delete newFolder;
			else 
			{
				const std::string& key = newFolder->getPath();
				if (fileMap.find(key) == fileMap.end())
				{
					folder->addChild(newFolder);
					fileMap[key] = newFolder;
				}
			}
		}
	}
}

FileFilterIndex* SystemData::getIndex(bool createIndex)
{
	if (mFilterIndex == nullptr && createIndex)
	{
		mFilterIndex = new FileFilterIndex();
		indexAllGameFilters(mRootFolder);
		mFilterIndex->setUIModeFilters();
	}

	return mFilterIndex;
}

void SystemData::deleteIndex()
{
	if (mFilterIndex != nullptr)
	{
		delete mFilterIndex;
		mFilterIndex = nullptr;
	}
}

void SystemData::indexAllGameFilters(const FolderData* folder)
{
	const std::vector<FileData*>& children = folder->getChildren();

	for(std::vector<FileData*>::const_iterator it = children.cbegin(); it != children.cend(); ++it)
	{
		switch((*it)->getType())
		{
			case GAME:   { mFilterIndex->addToIndex(*it); } break;
			case FOLDER: { indexAllGameFilters((FolderData*)*it); } break;
		}
	}
}

std::vector<std::string> readList(const std::string& str, const char* delims = " \t\r\n,")
{
	std::vector<std::string> ret;

	size_t prevOff = str.find_first_not_of(delims, 0);
	size_t off = str.find_first_of(delims, prevOff);
	while(off != std::string::npos || prevOff != std::string::npos)
	{
		ret.push_back(str.substr(prevOff, off - prevOff));

		prevOff = str.find_first_not_of(delims, off);
		off = str.find_first_of(delims, prevOff);
	}

	return ret;
}

//creates systems from information located in a config file
bool SystemData::loadConfig(Window* window)
{
	deleteSystems();
	ThemeData::setDefaultTheme(nullptr);

	std::string path = getConfigPath(false);

	LOG(LogInfo) << "Loading system config file " << path << "...";

	if (!Utils::FileSystem::exists(path))
	{
		LOG(LogError) << "es_systems.cfg file does not exist!";
		writeExampleConfig(getConfigPath(true));
		return false;
	}

	pugi::xml_document doc;
	pugi::xml_parse_result res = doc.load_file(path.c_str());

	if (!res)
	{
		LOG(LogError) << "Could not parse es_systems.cfg file!";
		LOG(LogError) << res.description();
		return false;
	}

	//actually read the file
	pugi::xml_node systemList = doc.child("systemList");
	if (!systemList)
	{
		LOG(LogError) << "es_systems.cfg is missing the <systemList> tag!";
		return false;
	}

	std::vector<std::string> systemsNames;

	int systemCount = 0;
	for (pugi::xml_node system = systemList.child("system"); system; system = system.next_sibling("system"))
	{
		systemsNames.push_back(system.child("fullname").text().get());
		systemCount++;
	}

	if (systemCount == 0)
	{
		LOG(LogError) << "no system found in es_systems.cfg";
		return false;
	}

	int currentSystem = 0;

	typedef SystemData* SystemDataPtr;

	ThreadPool* pThreadPool = NULL;
	SystemDataPtr* systems = NULL;

	// Allow threaded loading only if processor threads > 2 so it does not apply on machines like Pi0.
	if (std::thread::hardware_concurrency() > 2 && Settings::getInstance()->getBool("ThreadedLoading"))
	{
		pThreadPool = new ThreadPool();

		systems = new SystemDataPtr[systemCount];
		for (int i = 0; i < systemCount; i++)
			systems[i] = nullptr;

		pThreadPool->queueWorkItem([] { CollectionSystemManager::get()->loadCollectionSystems(true); });
	}

	int processedSystem = 0;

	for (pugi::xml_node system = systemList.child("system"); system; system = system.next_sibling("system"))
	{
		if (pThreadPool != NULL)
		{
			pThreadPool->queueWorkItem([system, currentSystem, systems, &processedSystem]
			{
				systems[currentSystem] = loadSystem(system);
				processedSystem++;
			});
		}
		else
		{
			std::string fullname = system.child("fullname").text().get();

			if (window != NULL)
				window->renderLoadingScreen(fullname, systemCount == 0 ? 0 : (float)currentSystem / (float)(systemCount + 1));

			std::string nm = system.child("name").text().get();

			SystemData* pSystem = loadSystem(system);
			if (pSystem != nullptr)
				sSystemVector.push_back(pSystem);
		}

		currentSystem++;
	}

	if (pThreadPool != NULL)
	{
		if (window != NULL)
		{
			pThreadPool->wait([window, &processedSystem, systemCount, &systemsNames]
			{
				int px = processedSystem - 1;
				if (px >= 0 && px < systemsNames.size())
					window->renderLoadingScreen(systemsNames.at(px), (float)px / (float)(systemCount + 1));
			}, 10);
		}
		else
			pThreadPool->wait();

		for (int i = 0; i < systemCount; i++)
		{
			SystemData* pSystem = systems[i];
			if (pSystem != nullptr)
				sSystemVector.push_back(pSystem);
		}

		delete[] systems;
		delete pThreadPool;

		if (window != NULL)
			window->renderLoadingScreen(_("Favorites"), systemCount == 0 ? 0 : currentSystem / systemCount);

		// updateSystemsList can't be run async, systems have to be created before
		CollectionSystemManager::get()->updateSystemsList();
	}
	else
	{
		if (window != NULL)
			window->renderLoadingScreen(_("Favorites"), systemCount == 0 ? 0 : currentSystem / systemCount);

		CollectionSystemManager::get()->loadCollectionSystems();
	}

	if (SystemData::sSystemVector.size() > 0)
		ThemeData::setDefaultTheme(SystemData::sSystemVector.at(0)->getTheme().get());

	return true;
}

SystemData* SystemData::loadSystem(pugi::xml_node system)
{
	std::string name, fullname, path, cmd, themeFolder;

	name = system.child("name").text().get();
	fullname = system.child("fullname").text().get();
	path = system.child("path").text().get();

	// convert extensions list from a string into a vector of strings
	std::vector<std::string> extensions = readList(system.child("extension").text().get());

	cmd = system.child("command").text().get();

	// platform id list
	const char* platformList = system.child("platform").text().get();
	std::vector<std::string> platformStrs = readList(platformList);
	std::vector<PlatformIds::PlatformId> platformIds;
	for (auto it = platformStrs.cbegin(); it != platformStrs.cend(); it++)
	{
		const char* str = it->c_str();
		PlatformIds::PlatformId platformId = PlatformIds::getPlatformId(str);

		if (platformId == PlatformIds::PLATFORM_IGNORE)
		{
			// when platform is ignore, do not allow other platforms
			platformIds.clear();
			platformIds.push_back(platformId);
			break;
		}

		// if there appears to be an actual platform ID supplied but it didn't match the list, warn
		if (platformId != PlatformIds::PLATFORM_UNKNOWN)
			platformIds.push_back(platformId);
		else if (str != NULL && str[0] != '\0' && platformId == PlatformIds::PLATFORM_UNKNOWN)
			LOG(LogWarning) << "  Unknown platform for system \"" << name << "\" (platform \"" << str << "\" from list \"" << platformList << "\")";
	}

	// theme folder
	themeFolder = system.child("theme").text().as_string(name.c_str());

	//validate
	if (name.empty() || path.empty() || extensions.empty() || cmd.empty())
	{
		LOG(LogError) << "System \"" << name << "\" is missing name, path, extension, or command!";
		return nullptr;
	}

	//convert path to generic directory seperators
	path = Utils::FileSystem::getGenericPath(path);

	//expand home symbol if the startpath contains ~
	if (path[0] == '~')
	{
		path.erase(0, 1);
		path.insert(0, Utils::FileSystem::getHomePath());
	}

	//create the system runtime environment data
	SystemEnvironmentData* envData = new SystemEnvironmentData;
	envData->mStartPath = path;
	envData->mSearchExtensions = extensions;
	envData->mLaunchCommand = cmd;
	envData->mPlatformIds = platformIds;

	// batocera
// emulators and cores
	std::map<std::string, std::vector<std::string>*> * systemEmulators = new std::map<std::string, std::vector<std::string>*>();
	pugi::xml_node emulatorsNode = system.child("emulators");
	for (pugi::xml_node emuNode = emulatorsNode.child("emulator"); emuNode; emuNode = emuNode.next_sibling("emulator")) {
		std::string emulatorName = emuNode.attribute("name").as_string();
		(*systemEmulators)[emulatorName] = new std::vector<std::string>();
		pugi::xml_node coresNode = emuNode.child("cores");
		for (pugi::xml_node coreNode = coresNode.child("core"); coreNode; coreNode = coreNode.next_sibling("core")) {
			std::string corename = coreNode.text().as_string();
			(*systemEmulators)[emulatorName]->push_back(corename);
		}
	}

	SystemData* newSys = new SystemData(name, fullname, envData, themeFolder, systemEmulators); // batocera
	if (newSys->getRootFolder()->getChildren().size() == 0)
	{
		LOG(LogWarning) << "System \"" << name << "\" has no games! Ignoring it.";
		delete newSys;
		return nullptr;
	}

	return newSys;
}

void SystemData::writeExampleConfig(const std::string& path)
{
	std::ofstream file(path.c_str());

	file << "<!-- This is the EmulationStation Systems configuration file.\n"
			"All systems must be contained within the <systemList> tag.-->\n"
			"\n"
			"<systemList>\n"
			"	<!-- Here's an example system to get you started. -->\n"
			"	<system>\n"
			"\n"
			"		<!-- A short name, used internally. Traditionally lower-case. -->\n"
			"		<name>nes</name>\n"
			"\n"
			"		<!-- A \"pretty\" name, displayed in menus and such. -->\n"
			"		<fullname>Nintendo Entertainment System</fullname>\n"
			"\n"
			"		<!-- The path to start searching for ROMs in. '~' will be expanded to $HOME on Linux or %HOMEPATH% on Windows. -->\n"
			"		<path>~/roms/nes</path>\n"
			"\n"
			"		<!-- A list of extensions to search for, delimited by any of the whitespace characters (\", \\r\\n\\t\").\n"
			"		You MUST include the period at the start of the extension! It's also case sensitive. -->\n"
			"		<extension>.nes .NES</extension>\n"
			"\n"
			"		<!-- The shell command executed when a game is selected. A few special tags are replaced if found in a command:\n"
			"		%ROM% is replaced by a bash-special-character-escaped absolute path to the ROM.\n"
			"		%BASENAME% is replaced by the \"base\" name of the ROM.  For example, \"/foo/bar.rom\" would have a basename of \"bar\". Useful for MAME.\n"
			"		%ROM_RAW% is the raw, unescaped path to the ROM. -->\n"
			"		<command>retroarch -L ~/cores/libretro-fceumm.so %ROM%</command>\n"
			"\n"
			"		<!-- The platform to use when scraping. You can see the full list of accepted platforms in src/PlatformIds.cpp.\n"
			"		It's case sensitive, but everything is lowercase. This tag is optional.\n"
			"		You can use multiple platforms too, delimited with any of the whitespace characters (\", \\r\\n\\t\"), eg: \"genesis, megadrive\" -->\n"
			"		<platform>nes</platform>\n"
			"\n"
			"		<!-- The theme to load from the current theme set.  See THEMES.md for more information.\n"
			"		This tag is optional. If not set, it will default to the value of <name>. -->\n"
			"		<theme>nes</theme>\n"
			"	</system>\n"
			"</systemList>\n";

	file.close();

	LOG(LogError) << "Example config written!  Go read it at \"" << path << "\"!";
}

void SystemData::deleteSystems()
{
	bool saveOnExit = !Settings::getInstance()->getBool("IgnoreGamelist") && Settings::getInstance()->getBool("SaveGamelistsOnExit");

	for (unsigned int i = 0; i < sSystemVector.size(); i++)
	{
		SystemData* pData = sSystemVector.at(i);

		if (saveOnExit && !pData->mIsCollectionSystem)
			updateGamelist(pData);

		delete pData;
	}

	sSystemVector.clear();
}

std::string SystemData::getConfigPath(bool forWrite)
{
	std::string path = Utils::FileSystem::getEsConfigPath() + "/es_systems.cfg"; // batocera
	if(forWrite || Utils::FileSystem::exists(path))
		return path;

	return "/etc/emulationstation/es_systems.cfg";
}

bool SystemData::isVisible()
{
	if (SystemConf::getInstance()->get(getName() + ".hide") == "1") // batocera (hide systems)
	    return false; // batocera (hide systems)

	return (getDisplayedGameCount() > 0 || 
           (UIModeController::getInstance()->isUIModeFull() && mIsCollectionSystem) ||
           (mIsCollectionSystem && mName == "favorites"));
}

SystemData* SystemData::getNext() const
{
	std::vector<SystemData*>::const_iterator it = getIterator();

	do {
		it++;
		if (it == sSystemVector.cend())
			it = sSystemVector.cbegin();
	} while (!(*it)->isVisible());
	// as we are starting in a valid gamelistview, this will always succeed, even if we have to come full circle.

	return *it;
}

SystemData* SystemData::getPrev() const
{
	std::vector<SystemData*>::const_reverse_iterator it = getRevIterator();

	do {
		it++;
		if (it == sSystemVector.crend())
			it = sSystemVector.crbegin();
	} while (!(*it)->isVisible());
	// as we are starting in a valid gamelistview, this will always succeed, even if we have to come full circle.

	return *it;
}

std::string SystemData::getGamelistPath(bool forWrite) const
{
	std::string filePath;

	filePath = mRootFolder->getPath() + "/gamelist.xml";
	if(Utils::FileSystem::exists(filePath))
		return filePath;

	//filePath = "/userdata/system/configs/emulationstation/gamelists/" + mName + "/gamelist.xml"; // batocera
	if(forWrite) // make sure the directory exists if we're going to write to it, or crashes will happen
		Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(filePath));
	if(forWrite || Utils::FileSystem::exists(filePath))
		return filePath;

	return "/etc/emulationstation/gamelists/" + mName + "/gamelist.xml";
}

std::string SystemData::getThemePath() const
{
	// where we check for themes, in order:
	// 1. [SYSTEM_PATH]/theme.xml
	// 2. system theme from currently selected theme set [CURRENT_THEME_PATH]/[SYSTEM]/theme.xml
	// 3. default system theme from currently selected theme set [CURRENT_THEME_PATH]/theme.xml

	// first, check game folder
	std::string localThemePath = mRootFolder->getPath() + "/theme.xml";
	if(Utils::FileSystem::exists(localThemePath))
		return localThemePath;

	// not in game folder, try system theme in theme sets
	localThemePath = ThemeData::getThemeFromCurrentSet(mThemeFolder);

	if (Utils::FileSystem::exists(localThemePath))
		return localThemePath;

	// not system theme, try default system theme in theme set
	localThemePath = Utils::FileSystem::getParent(Utils::FileSystem::getParent(localThemePath)) + "/theme.xml";

	return localThemePath;
}

bool SystemData::hasGamelist() const
{
	return (Utils::FileSystem::exists(getGamelistPath(false)));
}

unsigned int SystemData::getGameCount() const
{
	return (unsigned int)mRootFolder->getFilesRecursive(GAME).size();
}

SystemData* SystemData::getRandomSystem()
{
	//  this is a bit brute force. It might be more efficient to just to a while (!gameSystem) do random again...
	unsigned int total = 0;
	for(auto it = sSystemVector.cbegin(); it != sSystemVector.cend(); it++)
	{
		if ((*it)->isGameSystem())
			total ++;
	}

	// get random number in range
	int target = (int)Math::round((std::rand() / (float)RAND_MAX) * (total - 1));
	for (auto it = sSystemVector.cbegin(); it != sSystemVector.cend(); it++)
	{
		if ((*it)->isGameSystem())
		{
			if (target > 0)
			{
				target--;
			}
			else
			{
				return (*it);
			}
		}
	}

	// if we end up here, there is no valid system
	return NULL;
}

FileData* SystemData::getRandomGame()
{
	std::vector<FileData*> list = mRootFolder->getFilesRecursive(GAME, true);
	unsigned int total = (int)list.size();
	int target = 0;
	// get random number in range
	if (total == 0)
		return NULL;
	target = (int)Math::round((std::rand() / (float)RAND_MAX) * (total - 1));
	return list.at(target);
}

unsigned int SystemData::getDisplayedGameCount() const
{
	return (unsigned int)mRootFolder->getFilesRecursive(GAME, true).size();
}

void SystemData::loadTheme()
{
	mTheme = std::make_shared<ThemeData>();

	std::string path = getThemePath();

	if(!Utils::FileSystem::exists(path)) // no theme available for this platform
		return;

	try
	{
		// build map with system variables for theme to use,
		std::map<std::string, std::string> sysData;
		sysData.insert(std::pair<std::string, std::string>("system.name", getName()));
		sysData.insert(std::pair<std::string, std::string>("system.theme", getThemeFolder()));
		sysData.insert(std::pair<std::string, std::string>("system.fullName", getFullName()));
		
		mTheme->loadFile(getThemeFolder(), sysData, path);
	} catch(ThemeException& e)
	{
		LOG(LogError) << e.what();
		mTheme = std::make_shared<ThemeData>(); // reset to empty
	}
}

// batocera
std::map<std::string, std::vector<std::string>*>* SystemData::getEmulators() {
	return mEmulators;
}

void SystemData::setSortId(const unsigned int sortId)
{
	mSortId = sortId;
	Settings::getInstance()->setInt(getName() + ".sort", mSortId);
}